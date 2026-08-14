//
//  zepto8-mister — MiSTer FPGA frontend for the zepto8 PICO-8 emulator
//
//  Copyright © 2024-2026 MiSTer Organize
//
//  Built on zepto8 by Sam Hocevar (WTFPL license)
//  MiSTer frontend patterns for the zepto8 PICO-8 emulator on MiSTer
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <cmath>
#include <string>
#include <vector>
#include <new>          /* std::bad_alloc -- the recorder catches it, see p8rec */
#include <memory>
#include <thread>
#include <atomic>

#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <linux/joystick.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include "native_sha1.h"   /* content identity for the recorder (step 19) */

#include <SDL/SDL.h>

#include "zepto8.h"
#include "pico8/vm.h"
#include <lol/sys/init.h>
#include "cart_browser.h"
#include "native_video_writer.h"
#include "test_trace.h"

// ── Configuration ─────────────────────────────────────────────────────

static const int PICO8_W        = 128;
static const int PICO8_H        = 128;
static const int SCREEN_W       = 320;  // Set by vmode in pico-8.sh
static const int SCREEN_H       = 240;
static const int SCREEN_BPP     = 16;   // rgb16
static const int AUDIO_RATE     = 22050;
static const int AUDIO_CHANNELS = 1;    // mono
static const int AUDIO_BUF_SAMPLES = 512;
static const int DEFAULT_FPS    = 60;   // PICO-8 BIOS expects 60 ticks/sec

// ── Globals ───────────────────────────────────────────────────────────

static volatile bool g_running = true;
static volatile bool g_return_to_browser = false;

// -- Input recorder / replay (phase 1: pause-menu only, no .s1 OSD slot) ----
//
// Records per-frame controller state and replays it deterministically. Same
// shape as OpenBOR_7533's .inp recorder, and one of the canonical pause-menu
// options every hybrid core must ship.
//
// WHY THIS IS SIMPLER THAN OPENBOR'S: OpenBOR's getinterval() returns a
// VARIABLE number of logic steps per input sample, so its recorder has to
// record that interval and force it back on playback, or gameplay desyncs
// while menus stay in sync. PICO-8 has a FIXED timestep -- mister_main runs
// exactly one g_vm->step() per frame -- so frame N is always frame N and that
// whole class of desync does not exist here.
//
// The three things determinism needs, and where each comes from:
//   1. identical start state -- title-anchor via extcmd("reset")'s _exit(0),
//      which respawns and re-mounts the same cart from scratch.
//   2. identical RNG -- Z8_TEST_SEED, set before g_vm->load(). It feeds
//      vm::private_init_ram()'s api_srand(). Reusing the seeding path the
//      golden-trace harness already proves deterministic beats inventing a
//      second one.
//   3. identical input -- captured and injected at the single choke point
//      where joystick bits become g_vm->button() calls.
//
// A recording is an INPUT STREAM, not video, so the FPS overlay can never
// contaminate one -- which is why leaving it on during record/replay is safe
// and is in fact the intended debugging pairing.

/* STABLE magic + a separate container version. The version used to live IN the
 * magic ("P8REC3"), so a take from a NEWER build failed the magic test and was
 * reported as "not a valid recording" -- telling the user their file was damaged
 * when it was fine and their core was old. For a format meant to be shared,
 * receiving a newer take is routine, so it needs its own answer. */
#define P8REC_MAGIC      "P8REC"
#define P8REC_MAGIC_LEN  5
#define P8REC_CONTAINER  5u        /* framing version; bump on any layout change */
/* v5 adds the identity section (step 19): a manifest of (name, sha1) for every
 * cart file the run opened. v4 identified content by its PATH, which was wrong
 * in both directions -- a renamed v2 matched v1's name, played, and desynced
 * silently, while the same cart under a different layout was refused. It also
 * stored the name truncated to 255 and compared the FULL string, so a deep-path
 * cart could never replay its own take. Hashes replace all of that; the name
 * survives only so a refusal can say WHICH cart is wanted. */
#define P8REC_IDENT_MAX  512u      /* manifest entries; a multicart is ~dozens */
// Bump ONLY on a shipped game-LOGIC change that would desync existing
// recordings (VM semantics, RNG, timestep, input mapping). NOT for
// render/audio/UI/perf changes -- the fixed timestep makes replay independent
// of frame cost, so those can never desync a recording.
#define P8REC_ENGINE_VER 1u
#define P8REC_MAX_FRAMES 2000000u   /* ~9.2 h at 60 fps; caps a runaway file */
#define P8REC_CART_LEN   256   /* holds a relative path now, not just a basename */

/* The ONE cartdata suffix. Both the snapshot writer and the snapshot reader
 * filter on it, and until now each spelled it as its own local literal in its
 * own function -- so changing one and not the other would have produced a
 * writer that emits takes its own reader refuses, silently and permanently.
 *
 * That is not hypothetical: OpenBOR had exactly that divergence twice on
 * 2026-08-13 (a .scr the embed loop wrote and the reader rejected, and an
 * unbounded identity name), and neither parser suite could see it because both
 * exercise only the READER. One constant removes the class by construction,
 * which beats adding a check that the two literals still match. */
#define P8REC_SNAP_EXT   ".p8d.txt"

static int         g_rec_mode = 0;   /* 0 = idle, 1 = recording, 2 = playing */
/* Set when -test is passed. The golden-trace harness owns Z8_TEST_SEED in that
 * mode, so the recorder must never unset it. Declared here (not beside
 * g_test_trace, which is defined further down) so p8rec_reset can see it. */
static bool        g_test_trace_enabled = false;
static std::vector<uint32_t> g_rec_frames;
static size_t      g_rec_pos  = 0;
static int32_t     g_rec_seed = 0;
/* The extcmd lambdas are converted to std::function and cannot capture,
 * so the cart path the writer needs lives here. Set at cart load. */
static std::string g_cart_path_for_rec;

/* One home for everything the recorder owns, top-level and per-build, matching
 * how saves/, savestates/, config/ and logs/ already work. There was never a
 * principled reason for replays alone to hide under games/.
 *
 * The old layout put takes in games/PICO-8/Replays and the scratch INSIDE the
 * real saves directory, so a stray glob over saves/PICO-8/ * would have destroyed
 * user progress -- a hazard that had to be carefully worked around rather than
 * not existing. Nothing of the user's lives under this tree, so rm -rf on it is
 * always safe.
 *
 * OSD note: MiSTer's browser always STARTS at the core's games/ home, so the
 * first "Load Replay" needs one navigation up and over. Selected_S[] then
 * remembers the directory per slot -- but it reverts to home if the path is
 * missing, which is why the handler mkdir -p's this on every launch. */
static const char *P8REC_DIR = "/media/fat/replays/PICO-8";

/* BOUNDED slot library, like savestates: <cart>_1.inp .. <cart>_8.inp, chosen
 * with left/right on the "slot N of 8" row of the Recording submenu.
 *
 * It replaced an unbounded <cart>_N library whose only access path was the OSD
 * picker -- which always starts at the core's games/ home, so reaching a
 * top-level folder means navigating up and out. That is not something a user
 * discovers. A slot number needs no browsing at all.
 *
 * Eight rather than the 4 that savestates use, because a savestate is scratch
 * you overwrite constantly while a take is an artifact you keep; and the count
 * is free here since the picker lives in our pause menu, not in CONF_STR status
 * bits where two bits would force exactly four.
 *
 * Record OVERWRITES the chosen slot -- that is what bounded means -- but never
 * silently: the row reads "used" or "empty" before you commit, and a notice
 * names the slot afterwards.
 *
 * 0 means "not chosen yet". 🛑 It resolves to SLOT 1, not to the first free
 * slot. This comment used to say "Record then takes the first FREE slot so a
 * first-timer cannot clobber anything" -- that WAS the behaviour and it was
 * deliberately removed (see p8rec_slot_now), because it made the menu open
 * somewhere different depending on what had already been recorded, which is
 * unpredictable exactly where a user wants predictability. Overwriting stays
 * VISIBLE rather than prevented: the row reads "used" before you commit and a
 * notice names the slot after. Do not "restore" first-free on the strength of
 * a stale sentence -- OpenBOR defaults to 1 too, and the two must match. */
#define P8REC_SLOTS 8
static int g_rec_slot = 0;   /* 1..P8REC_SLOTS, 0 = not yet chosen */
/* Set when a write fails AT THE FRAME CAP, so the cap block reports once
 * instead of re-attempting a full file write on every frame. Cleared by
 * p8rec_reset(), i.e. by any successful Stop or a new session. */
static bool g_rec_cap_failed = false;

/* --- Save-state snapshot, so a recording can start from YOUR progress --------
 *
 * Isolating the save dir to EMPTY made recordings deterministic but also made
 * it impossible to record from anywhere except a fresh game: Record resets to
 * the title, and with no save data there was nothing to continue from.
 *
 * So the scratch is SEEDED from the real saves instead, and a copy of that
 * seed is stored beside the .inp. Record boots with your progress, you load it
 * through the cart's own menus (that navigation is recorded, since the take is
 * title-anchored), and playback restores the same seed before booting -- so the
 * same presses land on the same menu with the same contents.
 *
 * The snapshot is what makes it exact rather than merely convenient. A cart's
 * load/continue menu changes SHAPE with which slots exist, so replaying against
 * different save data would send the recorded D-pad presses to a different slot
 * and desync from the first second.
 *
 * Real saves are only ever READ. */
/* All of it lives beside the takes in replays/PICO-8/, as DOT-directories.
 *
 * It used to sit under saves/, to keep the OSD "Load Replay" picker clean: that
 * picker lists directories as well as files, so a snapshot folder beside each
 * take turned it into a list of decoys that appear EMPTY when opened (they hold
 * .p8d.txt, and the picker filters to .inp). But hiding it inside the REAL saves
 * directory meant a careless glob over saves/PICO-8/ * would have destroyed user
 * progress -- a hazard that then had to be worked around everywhere. A leading
 * dot solves the picker problem without putting anything near the user's data.
 *
 * Not under savestates/ because none of this is an emulator save state -- it is
 * cartdata the game itself wrote. (Same reasoning applies to OpenBOR, whose .sNN
 * files are script-saves despite the directory we named "savestates".) */
/* Hidden siblings of the takes, so the OSD picker (which filters to .inp and
 * hides dotdirs) never shows them, while everything recorder-owned stays in one
 * tree.
 *
 * There is deliberately no .snapshots here. OpenBOR keeps per-take save
 * snapshots in one (pruned to 20); PICO-8 does not need it, because the
 * snapshot travels INSIDE the .inp as a payload section (p8snap_write /
 * p8snap_read) rather than beside it. The directory used to be created on every
 * launch and never written to -- worse than dead, since it implied a store a
 * future writer would have inherited with no prune on either side. */
static const char *P8REC_SCRATCH = "/media/fat/replays/PICO-8/.scratch";
static const char *P8REC_ARMSNAP = "/media/fat/replays/PICO-8/.armsnap";
static const char *P8_REAL_SAVES = "/media/fat/saves/PICO-8";
static std::string g_rec_file;   /* the .inp actually opened; pairs it with its snapshot */

/* --- The snapshot travels INSIDE the .inp -----------------------------------
 *
 * A recording is meant to be shareable: hand someone the .inp and it replays on
 * their MiSTer. That only works if the save data the run started from goes with
 * it. A sidecar directory cannot travel, and its absence fails SILENTLY -- the
 * replay still runs, just against different save data, so the cart's menus have
 * a different shape and the recorded presses land on different items.
 *
 * Appended after the frames:
 *   u32 file_count
 *   repeat: u32 name_len, name bytes, u32 data_len, data bytes
 *
 * A take with no saves writes file_count = 0, so the field is always present
 * and the reader never has to guess whether one exists. */
/* CRC32 over the frame block, stored in the header. Without it, bit rot or a
 * partial SD write landing inside an already-written region produces a file
 * that passes every guard and replays as a plausible-looking divergence --
 * silent, and indistinguishable from a game bug. One pass over data we are
 * already reading. */
static uint32_t p8_crc32(const void* buf, size_t len) {
    static uint32_t tab[256];
    static int built = 0;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        built = 1;
    }
    const unsigned char* p = (const unsigned char*)buf;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

struct P8SnapFile { std::string name; std::vector<unsigned char> data; };
static std::vector<P8SnapFile> g_rec_snap;   /* payload of the take being played */

/* Which save files this run actually opened. Called from vm::get_path_save and
 * vm::get_path_cstore (private.cpp) at the moment each is resolved, so a
 * multicart registers every sub-cart as it loads.
 *
 * Why this exists: the snapshot used to be seeded from the WHOLE saves folder,
 * so a shared take carried save data for every cart the user had ever played --
 * and, because cstore writes a complete .p8 into that same directory, entire
 * third-party carts in source form. Bloat, a privacy leak, and an unwitting
 * redistribution of other people's work, all inside a file the user believes is
 * a controller log.
 *
 * Scoping by the entry cart's NAME would have been simpler and wrong: sub-carts
 * call cartdata() with ids bearing no relation to it, so name-scoping silently
 * breaks every multicart recording. */
static std::vector<std::string> g_rec_used;

extern "C" void p8rec_note_save_file(const char *name)
{
    if (g_rec_mode != 1 || !name || !*name) return;   /* only while recording */
    for (size_t i = 0; i < g_rec_used.size(); i++)
        if (g_rec_used[i] == name) return;
    if (g_rec_used.size() < 512) g_rec_used.push_back(name);
}

/* ---- Content identity (step 19) -------------------------------------------
 *
 * A take is identified by the BYTES of every cart file the run opened, never by
 * a path. Hooked at vm::load_cart, which is the one choke point for the entry
 * cart, multicart siblings, reload() and cstore().
 *
 * Entry-only hashing would not be enough: a multicart loads siblings at runtime,
 * so verifying just the entry checks 1 of N files and the other N-1 can differ
 * freely -- which is precisely the silent divergence this format exists to
 * prevent. So: record every file, verify each one AS THE RUN OPENS IT, and name
 * the offending file when they disagree.
 *
 * Carts are tens to hundreds of KB, so these are always full-file hashes. No
 * cache, no size threshold -- both are complications the PICO-8 side does not
 * need, and a play-side cache can only ever cost correctness. */
struct P8RecIdent { std::string name; uint8_t hash[NSHA1_DIGEST_LEN]; };
static std::vector<P8RecIdent> g_rec_ident;   /* record: built; play: expected */
static bool g_rec_ident_ok = true;            /* cleared by a sibling mismatch */

/* Display name for a cart file: the path relative to Carts/, separators kept.
 * Used ONLY in messages and to pair a manifest entry with the file the run
 * opens. It never decides anything -- that is what the hash is for. */
static std::string p8rec_ident_name(std::string const &path)
{
    static const char *root = "/media/fat/games/PICO-8/Carts/";
    size_t rl = strlen(root);
    return (path.compare(0, rl, root) == 0) ? path.substr(rl) : path;
}

/* Fit a content name into a notice WITHOUT eating the instruction after it.
 *
 * The renderer is 21 cols x 3 lines and truncates past that (native_video_writer.c
 * NV_COLS / NV_NOTICE_MAX), and word-wrapping wastes some of each line. So a long
 * cart name in "Needs <name> - load that cart" pushes the only actionable part of
 * the sentence off the screen -- on the message a shared recording produces most
 * often, now that identity refusals are the common case.
 *
 * Keeps the TAIL, because the filename identifies the cart while the leading
 * folders are what a user can infer. */
static std::string p8rec_short_name(std::string const &n, size_t max)
{
    if (n.size() <= max) return n;
    return "..." + n.substr(n.size() - (max - 3));
}
#define P8REC_NAME_FIT 24   /* leaves room for the longest instruction we use */

extern "C" void p8rec_note_cart_file(const char *resolved_path)
{
    if (!g_rec_mode || !resolved_path || !*resolved_path) return;

    std::string nm = p8rec_ident_name(resolved_path);
    uint8_t h[NSHA1_DIGEST_LEN];
    bool hashed = (nsha1_file(resolved_path, h) == 0);

    if (g_rec_mode == 1)
    {
        /* RECORD: append each newly-seen file. An unreadable file is NOT
         * recorded as a zero hash -- that would make two unreadable carts
         * compare equal. Skip it; the take then simply does not vouch for it. */
        if (!hashed) { fprintf(stderr, "[REC] cannot hash %s -- not in the manifest\n", nm.c_str()); return; }
        for (size_t i = 0; i < g_rec_ident.size(); i++)
            if (g_rec_ident[i].name == nm) return;
        if (g_rec_ident.size() >= P8REC_IDENT_MAX) return;
        /* 🛑 Bound the NAME here, at the add site, for the same reason the count
         * above is bounded here: the writer emits `cnt` BEFORE its loop, so
         * anything the loop skips leaves the file declaring N entries and
         * containing N-1, and both readers loop exactly `cnt` times.
         *
         * A round-9 fix put this bound in the write loop as a `continue` and
         * made things strictly worse: before it, a >1024 name was written and
         * the reader refused cleanly; after it, the manifest desynchronised and
         * the reader walked into the frame block, so the take failed its CRC and
         * reported "damaged" instead. One refusal became a corrupt file.
         *
         * OpenBOR is the model -- it computes its entry count first, writes it,
         * then writes the entry under the same condition, so the two can never
         * disagree. Filtering at the source is the same shape.
         *
         * 1024 is what both read sites accept. A skipped entry costs the take
         * one integrity check, which is what "does not vouch for it" above
         * already means for an unhashable cart. */
        if (nm.size() > 1024) {
            fprintf(stderr, "[REC] cart path too long for the manifest (%u) -- not vouched for\n",
                    (unsigned)nm.size());
            return;
        }
        P8RecIdent e; e.name = nm; memcpy(e.hash, h, sizeof(h));
        g_rec_ident.push_back(e);
        return;
    }

    /* PLAY: verify against what the take recorded. A file the manifest does not
     * mention is not an error -- an older take, or a path the recorder could not
     * hash -- but a file it DOES mention and that differs is a guaranteed
     * divergence, so stop and say which file and what was expected. */
    for (size_t i = 0; i < g_rec_ident.size(); i++)
    {
        if (g_rec_ident[i].name != nm) continue;
        if (hashed && memcmp(g_rec_ident[i].hash, h, NSHA1_DIGEST_LEN) == 0) return;

        char want[41], got[41];
        nsha1_hex(g_rec_ident[i].hash, want);
        if (hashed) nsha1_hex(h, got); else snprintf(got, sizeof(got), "unreadable");
        fprintf(stderr, "[REC] %s does not match this recording (wants %s, yours is %s)"
                        " -- stopping, the replay would not match\n", nm.c_str(), want, got);
        {   /* Name the FILE, not just "content mismatch": on a multicart this is
             * a sibling the user may not even know is involved. */
            char msg[96];
            snprintf(msg, sizeof(msg), "%s version differs - stopped",
                     p8rec_short_name(nm, P8REC_NAME_FIT).c_str());
            NativeVideoWriter_Notice(msg, 6);
        }
        g_rec_ident_ok = false;
        return;
    }
}

static std::vector<P8SnapFile> p8snap_from_dir(std::string const &dir)
{
    std::vector<P8SnapFile> out;
    DIR *d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        /* Only what the run touched, and only cartdata.
         *
         * The .p8d.txt filter is load-bearing, not tidiness: the READER refuses
         * any payload entry that is not cartdata, because a <cart>.p8 entry is a
         * cstore overlay and vm::load_cart would splice it over the cart's ROM.
         * Without the same filter here, a cart that uses cstore would write a
         * take its own reader then rejects outright.
         *
         * Consequence, accepted with that security fix: a cstore overlay is NOT
         * carried, so a cart relying on one replays against un-overlaid ROM
         * elsewhere. Closing that needs the overlay verified some other way --
         * it is not reopened by putting it back in the payload. */
        {
            static const char *EXT = P8REC_SNAP_EXT;
            size_t el = strlen(EXT);
            if (n.size() <= el || n.compare(n.size() - el, el, EXT) != 0) continue;
            bool used = false;
            for (size_t i = 0; i < g_rec_used.size() && !used; i++)
                if (g_rec_used[i] == n) used = true;
            if (!used) continue;
        }
        std::string p = dir + "/" + n;
        struct stat st;
        if (stat(p.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) continue;
        FILE *f = fopen(p.c_str(), "rb");
        if (!f) continue;
        P8SnapFile sf;
        sf.name = n;
        sf.data.resize((size_t)st.st_size);
        bool ok = (st.st_size == 0) ||
                  fread(&sf.data[0], 1, (size_t)st.st_size, f) == (size_t)st.st_size;
        fclose(f);
        if (ok) out.push_back(sf);
    }
    closedir(d);
    return out;
}

/* 🛑 THE WRITER MUST NOT EMIT WHAT p8snap_read REFUSES.
 *
 * These four bounds mirror the reader's exactly (count 4096, name 512, entry
 * 8 MB, running total 8 MB). Without them this function wrote nl/dl/c straight
 * out of the vector with no limit, so a payload over any reader bound produced
 * a take THIS BUILD then refused -- permanently, with nothing pointing at the
 * cause. OpenBOR shipped precisely that bug twice (an unbounded strlen(name)
 * against a reader refusing >= 512, and a .scr the embed included and the
 * reader rejected), which is why its test_writer_reader_agree.py exists.
 *
 * Not reachable today -- p8snap_from_dir filters to .p8d.txt AND to what the
 * run touched, so a payload is a file or two of a few hundred bytes. That is an
 * argument about today's filter, not a property of this function, and the
 * filter is one edit from changing. Bounding here costs four comparisons.
 *
 * 🛑 Fix the WRITER, never the reader. Widening a reader bound to accept what
 * the writer emits is how a usability bug becomes an untrusted-input hole --
 * these takes are shared between strangers and the core runs as root.
 */
static bool p8snap_write(FILE *f, std::vector<P8SnapFile> const &v)
{
    uint32_t c = (uint32_t)v.size();
    if (c > 4096u) {
        fprintf(stderr, "[REC] payload has %u files -- over the 4096 the reader"
                        " accepts; not writing a take that cannot be replayed\n", c);
        return false;
    }
    {   /* aggregate first: the reader refuses on a running total, so a set of
         * individually-legal entries can still be rejected as a whole. */
        size_t total = 0;
        for (size_t i = 0; i < v.size(); i++) total += v[i].data.size();
        if (total > (8u << 20)) {
            fprintf(stderr, "[REC] payload totals %lu bytes -- over the 8 MB the"
                            " reader accepts; not writing it\n", (unsigned long)total);
            return false;
        }
    }
    if (fwrite(&c, sizeof(c), 1, f) != 1) return false;
    for (size_t i = 0; i < v.size(); i++) {
        uint32_t nl = (uint32_t)v[i].name.size();
        uint32_t dl = (uint32_t)v[i].data.size();
        if (nl > 512u || dl > (8u << 20)) {
            fprintf(stderr, "[REC] payload entry '%.64s' is out of range"
                            " (name %u, data %u) -- not writing it\n",
                    v[i].name.c_str(), nl, dl);
            return false;
        }
        if (fwrite(&nl, sizeof(nl), 1, f) != 1) return false;
        if (nl && fwrite(v[i].name.data(), 1, nl, f) != nl) return false;
        if (fwrite(&dl, sizeof(dl), 1, f) != 1) return false;
        if (dl && fwrite(&v[i].data[0], 1, dl, f) != dl) return false;
    }
    return true;
}

/* Absent payload (an older take) is NOT an error -- it plays with empty saves,
 * exactly as it did before. Only a MALFORMED payload fails. */
static bool p8snap_read(FILE *f, std::vector<P8SnapFile> &v)
{
    v.clear();
    uint32_t c = 0;
    /* NOT "an older take". P8REC3 always writes this field (0 when there are no
     * saves), and an older format carries an older magic and was already
     * rejected. So in a file that reached this line, a missing count is
     * truncation -- refuse. */
    if (fread(&c, sizeof(c), 1, f) != 1) return false;
    if (c > 4096u) return false;
    size_t total = 0;
    for (uint32_t i = 0; i < c; i++) {
        uint32_t nl = 0, dl = 0;
        if (fread(&nl, sizeof(nl), 1, f) != 1 || nl > 512u) return false;
        std::string nm(nl, '\0');
        if (nl && fread(&nm[0], 1, nl, f) != nl) return false;
        /* Per-entry cap AND a running total. Cartdata is a few hundred bytes per
         * file, so a legitimate payload is kilobytes; without the aggregate a
         * shared take could drive this to hundreds of MB on a 1 GB board that
         * shares its RAM with the FPGA. */
        /* 8 MB, not 16: the running total below refuses anything over 8 MB and
         * starts at zero, so a 16 MB per-entry cap could never bind -- the first
         * entry to exceed 8 MB failed on the very next line instead. A limit that
         * cannot fire reads as protection that is not there. */
        if (fread(&dl, sizeof(dl), 1, f) != 1 || dl > (8u << 20)) return false;
        total += dl;
        if (total > (8u << 20)) return false;
        P8SnapFile sf; sf.name = nm; sf.data.resize(dl);
        if (dl && fread(&sf.data[0], 1, dl, f) != dl) return false;
        /* These files now arrive from OTHER PEOPLE, so the name is untrusted.
         *
         * Bare filename only -- never a path -- so nothing can be written
         * outside the scratch.
         *
         * 🛑 AN EMBEDDED NUL IS REFUSED OUTRIGHT, and the reasoning that used to
         * stand here is why. It said: "nm is length-counted, so find() scans all
         * nl bytes, and the later c_str() truncation can only shorten a name,
         * never re-introduce a separator." Both halves are true and the
         * conclusion was still wrong, because it only considered the SEPARATOR
         * check. Truncation cannot add a '/', but it CAN cut away the required
         * extension -- which is the whole point of the whitelist below.
         *
         * The exploit, measured against this code: an entry named
         * "mygame.p8\0.p8d.txt" (nl = 18) has size() 18 and its last 8 BYTES are
         * ".p8d.txt", so every test below passes. p8snap_to_dir then builds the
         * path and calls .c_str(), which stops at the NUL, and the attacker's
         * blob lands as .scratch/mygame.p8 -- exactly the cstore overlay this
         * whitelist exists to stop. Z8_SAVES_DIR points at the scratch,
         * get_path_cstore resolves <basename>.p8 inside it, and load_cart
         * splices the ROM over the real cart before the first frame.
         *
         * The defect is a SEMANTIC MISMATCH: validated with length-counted
         * std::string semantics, used with NUL-terminated C semantics. Refusing
         * the NUL makes the two agree by construction, which is how OpenBOR's
         * extractor is immune -- it NUL-terminates first, then checks with
         * strrchr/strcasecmp, so check and use share one view of the string.
         *
         * AND the extension must be .p8d.txt. A snapshot carries CARTDATA. The
         * bare-name check alone accepted an entry called <cart>.p8 -- which is
         * where cstore overlays live, so vm::load_cart would splice a stranger's
         * bytes over the cart's ROM (spritesheet, map, sfx, music) before the
         * first frame. Contained to the data region, so not code execution, but
         * it renders arbitrary attacker content while the viewer believes they
         * are watching the real cart -- and this project has already seen a cart
         * pack geometry into the sfx region, so attacker "data" is attacker
         * control flow inside the cart.
         *
         * A malformed payload REFUSES rather than skipping the entry: playing on
         * with fewer save files than were recorded is a desync dressed up as a
         * warning. (User-confirmed 2026-08-02.) */
        static const char *SNAP_EXT = P8REC_SNAP_EXT;
        size_t extlen = strlen(SNAP_EXT);
        if (nm.empty() || nm.find('/') != std::string::npos
                       || nm.find('\\') != std::string::npos
                       || nm.find('\0') != std::string::npos   /* see above */
                       || nm == "." || nm == ".."
                       || nm.size() <= extlen
                       || nm.compare(nm.size() - extlen, extlen, SNAP_EXT) != 0)
        {
            fprintf(stderr, "[REC] snapshot payload contains an unsafe or "
                            "non-cartdata name -- not playing\n");
            return false;
        }
        v.push_back(sf);
    }
    return true;
}

/* Defined below, beside the other directory helpers. Declared here because
 * p8snap_to_dir is the only caller that precedes it. */
static void p8_wipe_dir(std::string const &dir);

static int p8snap_to_dir(std::vector<P8SnapFile> const &v, std::string const &dir)
{
    mkdir(dir.c_str(), 0777);
    p8_wipe_dir(dir);
    int n = 0;
    for (size_t i = 0; i < v.size(); i++) {
        FILE *f = fopen((dir + "/" + v[i].name).c_str(), "wb");
        if (!f) continue;
        bool ok = v[i].data.empty() ||
                  fwrite(&v[i].data[0], 1, v[i].data.size(), f) == v[i].data.size();
        if (fclose(f) == 0 && ok) n++;
    }
    return n;
}

static bool p8_copy_file(std::string const &src, std::string const &dst)
{
    FILE *s = fopen(src.c_str(), "rb");
    if (!s) return false;
    FILE *d = fopen(dst.c_str(), "wb");
    if (!d) { fclose(s); return false; }
    char buf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), s)) > 0)
        if (fwrite(buf, 1, n, d) != n) { ok = false; break; }
    if (fclose(d) != 0) ok = false;
    fclose(s);
    if (!ok) remove(dst.c_str());
    return ok;
}

/* Flat dirs only -- saves and snapshots have no subdirectories. */
static void p8_wipe_dir(std::string const &dir)
{
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        remove((dir + "/" + n).c_str());
    }
    closedir(d);
}

static int p8_copy_dir(std::string const &src, std::string const &dst)
{
    mkdir(dst.c_str(), 0777);
    p8_wipe_dir(dst);
    DIR *d = opendir(src.c_str());
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        std::string f = e->d_name;
        if (f == "." || f == "..") continue;
        /* Files only. The recorder tree no longer lives inside saves/, so the
         * self-copy that motivated this is gone -- but a user's own stray
         * subdirectory under saves/ would still be counted as a copied file
         * while silently failing to copy, which would misreport the seed. */
        struct stat st;
        if (stat((src + "/" + f).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) continue;
        if (p8_copy_file(src + "/" + f, dst + "/" + f)) n++;
    }
    closedir(d);
    return n;
}

/* Cart IDENTITY, used as BOTH the header's match field and the filename stem.
 *
 * This was the basename alone, which made two carts indistinguishable whenever
 * they shared a filename -- Carts/Puzzle/maze.p8.png and Carts/Action/maze.p8.png,
 * or a maze.p8 sitting beside its own maze.p8.png. The guard then compared
 * EQUAL and the wrong cart's recording PLAYED: the one case in the whole format
 * where a bad file is not refused.
 *
 * Keying on the path relative to Carts/ separates them. Relative (not absolute)
 * so moving the whole library does not invalidate every recording. Separators
 * are flattened so the same string also serves as the filename stem: a flat
 * Carts/ yields exactly the old basename -- existing layouts are untouched --
 * while a subfoldered one yields Puzzle_maze, which disambiguates the library
 * BY CONSTRUCTION instead of leaving the guard to refuse a collision later. */
static std::string p8rec_cart_base(std::string const &path)
{
    static const char *root = "/media/fat/games/PICO-8/Carts/";
    size_t rl = strlen(root);
    std::string b = (path.compare(0, rl, root) == 0) ? path.substr(rl) : path;

    /* strip .p8.png / .p8 so <cart>_3.inp pairs with <cart>.p8.png */
    static const char *exts[] = { ".p8.png", ".p8" };
    for (int i = 0; i < 2; i++)
    {
        size_t n = strlen(exts[i]);
        if (b.size() > n && b.compare(b.size() - n, n, exts[i]) == 0)
        { b = b.substr(0, b.size() - n); break; }
    }
    for (size_t i = 0; i < b.size(); i++)
        if (b[i] == '/' || b[i] == '\\') b[i] = '_';
    return b;
}

/* Path of one slot's take. The ONLY place the name is spelled, so the writer,
 * the reader and the used/empty probe cannot drift apart -- which the previous
 * unbounded design did: the scan accepted any <base>_<digits>.inp while the
 * loader rebuilt the name from the parsed integer, so a zero-padded copy
 * (maze_007) parsed to 7 and then opened a maze_7 that did not exist, and Play
 * broke permanently because it only ever opened the highest. Slots are
 * generated, never parsed, so there is nothing left to disagree about. */
static std::string p8rec_slot_path(std::string const &base, int slot)
{
    return std::string(P8REC_DIR) + "/" + base + "_" + std::to_string(slot) + ".inp";
}

static bool p8rec_slot_used(std::string const &base, int slot)
{
    return access(p8rec_slot_path(base, slot).c_str(), R_OK) == 0;
}

/* Highest OCCUPIED slot, or 0 if the cart has no takes at all.
 *
 * No longer a Play fallback -- p8rec_slot_now() resolves that. What survives
 * is the has-ANY-takes test, which is what separates "no recording for this
 * game" from "the slot you are looking at is empty". */
static int p8rec_highest(std::string const &base)
{
    for (int s = P8REC_SLOTS; s >= 1; s--)
        if (p8rec_slot_used(base, s)) return s;
    return 0;
}

/* THE slot: resolve the default ONCE and ASSIGN it, so the row that DISPLAYS a
 * slot and every consumer that ACTS on one are the same variable.
 *
 * Computing the default per-consumer is what produced two hardware bugs in a
 * row. f7a870e fixed Play by making it compute the same expression the row did;
 * the left/right handlers still fell back to 1, so the row read one slot while
 * LEFT adjusted another and jumped to slot 8. Matching expressions in N places
 * is not one variable -- OpenBOR has never had either bug because
 * pausemenu_patch.c assigns the default at the row and every later reader sees
 * that assignment. This is the same thing.
 *
 * The default is SLOT 1, matching OpenBOR (user's decision, 2026-08-07). It was
 * briefly first-free here, on the reasoning that a first-timer should not
 * clobber an existing take -- but that made the menu open on a different slot
 * depending on what was already recorded, which is unpredictable in exactly the
 * place a user wants predictability. Overwriting stays visible rather than
 * prevented: the row reads "used" before you commit, and the notice names the
 * slot afterwards. Do not reintroduce a first-free default on either core
 * without asking -- it is a decision, not an oversight. */
static int p8rec_slot_now()
{
    if (g_rec_slot < 1 || g_rec_slot > P8REC_SLOTS)
        g_rec_slot = 1;
    return g_rec_slot;
}

/* Push the pause menu's slot out to the OSD picker so the two show the same
 * number. The FPGA edge-detects this sequence and writes it back into the
 * OSD's status word (rtl/replay_slot_ui.sv).
 *
 * Call this ONLY from a change made in the pause menu. The OSD poll in the
 * hot-swap block also adopts the OSD's slot into g_rec_slot; publishing on
 * that adoption too would bounce the value back and forth. It converges
 * either way, but the traffic is pointless and it muddies anyone reading the
 * two in a log. */
/* Set when we publish; consumed by the ONE poll that follows. See the poll for
 * the race this closes -- it is not optional bookkeeping. */
static bool g_rec_pub_fresh = false;

/* Carry the chosen slot across the reset, and SAY whether it worked.
 *
 * Record, Play and Reset all _exit() and let the daemon respawn us, so a slot
 * held only in memory dies with the process. Every one of these writes used to
 * be fire-and-forget: with /tmp full or read-only, fopen returns NULL, the
 * recovery block's atoi() yields 0, p8rec_slot_now() clamps that to slot 1, and
 * Record then overwrites SLOT 1's take instead of the one the user picked --
 * silently. Callers that are about to arm must refuse instead. */
static bool p8rec_save_slot_marker()
{
    FILE *sf = fopen("/tmp/pico8_recslot", "w");
    if (!sf) return false;
    bool ok = fprintf(sf, "%d\n", p8rec_slot_now()) > 0;
    if (fclose(sf) != 0) ok = false;
    return ok;
}

static void p8rec_publish_slot()
{
    /* No sequence counter here on purpose -- see PublishReplaySlot. A
     * process-local static restarts at 0 while the wire survives the respawn,
     * so it could publish a value already there and the FPGA would see no
     * edge at all. The sequence is derived from the wire instead. */
    /* 🛑 Flag BEFORE the publish, matching OpenBOR. This set it AFTER, which is
     * safe here only because the poll that reads it lives in main() alongside
     * both publish sites -- PICO-8's only other threads are audio and keepalive,
     * and neither touches replay state.
     *
     * That is a real guarantee today and an invisible one: nothing in this file
     * said the ordering depended on it. The keepalive thread already loops at
     * 150 ms and is the obvious home if anyone moves this ~0.5 s poll off the
     * main loop, and at that moment the after-ordering becomes the race OpenBOR
     * documents at length -- a poll landing between publish and flag adopts the
     * echo of our own value and reverts the press the user just made.
     *
     * Ordering it the safe way costs at most one harmless extra skip (OpenBOR's
     * own comment says so), and removes the dependency on a fact stated nowhere.
     * If this ever does move off the main thread, g_rec_pub_fresh must also
     * become volatile, as OpenBOR's is. */
    g_rec_pub_fresh = true;
    NativeVideoWriter_PublishReplaySlot(p8rec_slot_now());
}

/* Returns true if the session can be torn down: the file was written, or there
 * was nothing to write. False means the take is STILL IN MEMORY and the caller
 * must leave the recorder armed so Stop can be retried. */
static bool p8rec_write(std::string const &cart_path)
{
    if (g_rec_frames.empty())
    {
        /* Never create an empty file -- with slots it would OVERWRITE a real
         * take with nothing, which is the one loss the whole design is built to
         * avoid. (Under the old numbered library it merely claimed the highest
         * index and shadowed every good take, which was bad enough.) */
        fprintf(stderr, "[REC] nothing captured -- not writing a file\n");
        /* Say so on screen. Stop Recording with nothing captured used to be
         * silent, which is indistinguishable from a save that worked -- and
         * the slot stays empty either way. OpenBOR_7533 has said this since
         * it shipped; same wording, per conformance item 20. */
        NativeVideoWriter_Notice("Nothing captured - no file. Saves off until reset.", 4);
        return true;   /* nothing to save, but the session is legitimately over */
    }
    std::string base = p8rec_cart_base(cart_path);

    /* Nothing picked -> slot 1, the same slot the row was showing. */
    p8rec_slot_now();
    bool overwriting = p8rec_slot_used(base, g_rec_slot);
    std::string out = p8rec_slot_path(base, g_rec_slot);

    mkdir(P8REC_DIR, 0777);   /* handler makes it, but never lose a take to a missing dir */

    /* Write to a temp name and rename into place only once every byte is
     * committed. Without this, a full SD leaves a SHORT file that still claims
     * the highest index -- and since playback can only ever open the highest,
     * one failed write permanently shadows every good take for this cart, with
     * no way back except deleting the file over the network. Every fwrite and
     * the fclose are checked, because the bulk payload is buffered and most of
     * a full-disk failure surfaces at close. */
    std::string tmp = out + ".part";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) { fprintf(stderr, "[REC] cannot write %s\n", tmp.c_str()); return false; }

    char cart[P8REC_CART_LEN];
    memset(cart, 0, sizeof(cart));
    snprintf(cart, sizeof(cart), "%s", base.c_str());
    uint32_t ver = P8REC_ENGINE_VER;
    uint32_t n   = (uint32_t)g_rec_frames.size();

    uint32_t container = P8REC_CONTAINER;
    uint32_t crc = p8_crc32(&g_rec_frames[0], (size_t)n * sizeof(uint32_t));
    bool ok = fwrite(P8REC_MAGIC, 1, P8REC_MAGIC_LEN, f) == (size_t)P8REC_MAGIC_LEN
           && fwrite(&container, sizeof(container), 1, f) == 1
           && fwrite(&ver, sizeof(ver), 1, f) == 1
           && fwrite(cart, 1, sizeof(cart), f) == sizeof(cart)
           && fwrite(&g_rec_seed, sizeof(g_rec_seed), 1, f) == 1
           && fwrite(&n, sizeof(n), 1, f) == 1
           && fwrite(&crc, sizeof(crc), 1, f) == 1;

    /* Identity section (v5). Sits between the fixed header and the frame block so
     * a probe can read it and stop, without seeking past a frame count the file
     * itself supplied -- probe() is called at four selection sites, before any
     * reset, and must not have to trust the thing it is validating. */
    if (ok)
    {
        uint16_t cnt = (uint16_t)g_rec_ident.size();
        ok = fwrite(&cnt, sizeof(cnt), 1, f) == 1;
        for (size_t i = 0; ok && i < g_rec_ident.size(); i++)
        {
            /* 🛑 NOTHING may be skipped in this loop. `cnt` was written above,
             * before it, and both readers loop exactly `cnt` times -- so a skipped
             * entry leaves the file declaring N and containing N-1, and the reader
             * walks into the frame block. A round-9 fix put a name-length
             * `continue` here and produced exactly that: a corrupt take that fails
             * its CRC and reports "damaged", where before it the reader had refused
             * cleanly. The bound now lives at the add site (p8rec_note_cart_file),
             * alongside the count bound that is there for this same reason.
             *
             * Both length and count are therefore guaranteed by construction here:
             * size() <= P8REC_IDENT_MAX (512) and every name <= 1024, so neither
             * uint16_t cast can truncate. */
            uint16_t nl = (uint16_t)g_rec_ident[i].name.size();
            ok = fwrite(&nl, sizeof(nl), 1, f) == 1
              && fwrite(g_rec_ident[i].name.data(), 1, nl, f) == nl
              && fwrite(g_rec_ident[i].hash, 1, NSHA1_DIGEST_LEN, f) == NSHA1_DIGEST_LEN;
        }
    }

    if (ok) ok = fwrite(&g_rec_frames[0], sizeof(uint32_t), n, f) == (size_t)n;
    /* The save state this run STARTED from, embedded so the take is one file. */
    std::vector<P8SnapFile> snap = p8snap_from_dir(P8REC_ARMSNAP);
    if (ok) ok = p8snap_write(f, snap);
    if (fclose(f) != 0) ok = false;          /* buffered payload lands here */

    if (!ok || rename(tmp.c_str(), out.c_str()) != 0)
    {
        remove(tmp.c_str());                 /* never leave a file holding the index */
        fprintf(stderr, "[REC] FAILED to write %s -- the recording is still in memory,"
                        " free some space and pick Stop Recording again\n", out.c_str());
                        NativeVideoWriter_Notice("Could not save the recording - free space and Stop again", 7);
        return false;
    }

    /* Pair the take with the save state it STARTED from. Without this the replay
     * would boot against whatever the saves happen to be later, and the cart's
     * load/continue menu would have a different shape -- the recorded D-pad
     * presses would land on a different slot and desync immediately. */
    fprintf(stderr, "[REC] wrote %s (%u frames, %u save file(s) embedded, seed %d)\n",
            out.c_str(), n, (unsigned)snap.size(), g_rec_seed);

    /* Say which slot, and say it LOUDER when a take was replaced. Bounded slots
     * mean Record destroys something; the row already warns beforehand, but the
     * user is watching the game by the time the write lands, so the outcome has
     * to be reported too. */
    {
        char msg[80];
        snprintf(msg, sizeof(msg),
                 /* Says the saves are still isolated: p8rec_reset() leaves
                  * Z8_SAVES_DIR pointing at the scratch on purpose, so
                  * everything written after this is wiped at the next arm.
                  * Reset is what clears it, so the instruction is actionable.
                  * The previous replace wording was 17 characters longer;
                  * with the suffix it wrapped to FOUR lines at 21 cols and
                  * was cut. Retired strings are DESCRIBED here, never quoted
                  * verbatim -- a grep cannot tell code from a comment, so a
                  * quoted one turns "is it gone?" into a permanent failure
                  * that gets waved away, which is how a check stops being
                  * believed. Same reasoning as the ASCII-only patch rule. */
                 overwriting ? "Replaced slot %d. Saves off until reset."
                             : "Saved to slot %d. Saves off until reset.",
                 g_rec_slot);
        NativeVideoWriter_Notice(msg, 6);
        if (overwriting)
            fprintf(stderr, "[REC] slot %d already held a take -- overwritten\n", g_rec_slot);
    }
    return true;
}

/* .s1 = the OSD "Load Replay" slot, the second way to start a playback (the
 * first is the pause menu). MiSTer writes the picked file's path there.
 *
 * A pick is detected by .s1's MTIME, never by its contents: MiSTer bumps the
 * mtime on EVERY pick, including re-picking the SAME file, where the path is
 * byte-identical and a content compare sees nothing at all. Baselined at
 * startup so a stale .s1 left behind by an earlier session never auto-replays,
 * and deliberately NOT cleared afterwards -- it persists as a "last replay"
 * marker, and a clear-then-restore would itself look like a fresh pick. */
static time_t p8_s1_mtime(void)
{
    struct stat st;
    if (stat("/media/fat/config/PICO-8.s1", &st) == 0) return st.st_mtime;
    return 0;   /* absent == never picked */
}
static time_t g_s1_seen = 0;

/* Load a recording for this cart. With no explicit path, opens the NEWEST take
 * for the cart (the pause-menu "Play Recording" path). With one, opens exactly
 * that file (the OSD "Load Replay" path, where the user picked it by hand).
 *
 * Returns false -- leaving the mode idle -- on any mismatch, so a wrong-cart or
 * corrupt file is REFUSED rather than played into a guaranteed desync. The
 * cart-match check below is what makes the OSD path safe: that picker will
 * happily hand us a take recorded against a completely different cart. */
/* Validate a take BEFORE anything irreversible happens (design note §7).
 *
 * Every "play a replay" entry point used to write its markers and _exit()
 * WITHOUT opening the file, so validation happened in the next process, after
 * the cart had already reloaded. A wrong pick therefore destroyed the user's
 * session and only then explained itself -- worst on the OSD .s1 path, which is
 * exactly where wrong picks come from.
 *
 * Probing here means the refusal happens while the pause menu is still open and
 * the run is untouched. Reads the header and the identity section only; it never
 * touches the frame block or the payload, so it stays cheap enough to run on
 * every selection.
 *
 * Returns true if the take would play. On false, `why` holds a user-facing
 * reason already sized for the 21x3 notice renderer. */
static bool p8rec_probe(std::string const &cart_path,
                        std::string const &explicit_path,
                        char *why, size_t whysz,
                        int slot_override = 0)
{
    std::string in = explicit_path;
    if (in.empty())
    {
        std::string base = p8rec_cart_base(cart_path);
        /* The SAME slot stat(221) displays -- p8rec_slot_now() is the only
         * thing that decides it.
         *
         * They used to differ, and the row names a slot, so the menu said one
         * thing and the button did another: the row read "slot 2" while Play
         * resolved the highest occupied slot and played slot 1's take rather
         * than saying slot 2 was empty. Reported on hardware 2026-08-07.
         *
         * A convenience default is fine while nothing on screen CLAIMS a slot.
         * This row claims one, so the default has to be the displayed one. */
        /* slot_override lets a caller ask about a slot WITHOUT committing to
         * it. The OSD Play path needs that: a refused Play must leave both
         * pickers exactly where they were, which is what OpenBOR does (it
         * commits the slot only once every check has passed). */
        int slot = slot_override ? slot_override : p8rec_slot_now();
        /* Nothing anywhere for this cart is a different fact from "the slot you
         * are looking at is empty", and only the first one tells the user there
         * is nothing to look for. OpenBOR has always split them this way
         * (pausemenu_patch.c: _pmx == 0 -> "No recording for this game"); before
         * this, PICO-8 could only reach that string when the resolved slot was
         * 0, which no default here ever produces -- so the notice was in the
         * binary and unreachable. */
        if (p8rec_highest(base) == 0)
        { snprintf(why, whysz, "No recording for this game"); return false; }
        if (!p8rec_slot_used(base, slot))
        {   /* An explicitly chosen but empty slot is not "no recordings" -- the
             * cart may well have takes in other slots, and saying otherwise
             * sends the user looking for a bug that is not there. */
            snprintf(why, whysz, "Slot %d is empty", slot);
            return false;
        }
        in = p8rec_slot_path(base, slot);
    }

    FILE *f = fopen(in.c_str(), "rb");
    if (!f) { snprintf(why, whysz, "That recording cannot be opened"); return false; }

    char magic[P8REC_MAGIC_LEN];
    uint32_t container = 0, ver = 0;
    char cart[P8REC_CART_LEN];
    bool ok = fread(magic, 1, P8REC_MAGIC_LEN, f) == (size_t)P8REC_MAGIC_LEN
           && memcmp(magic, P8REC_MAGIC, P8REC_MAGIC_LEN) == 0
           && fread(&container, sizeof(container), 1, f) == 1
           && fread(&ver, sizeof(ver), 1, f) == 1
           && fread(cart, 1, sizeof(cart), f) == sizeof(cart);
    if (!ok) { fclose(f); snprintf(why, whysz, "That file is not a valid recording"); return false; }
    if (container > P8REC_CONTAINER)
    { fclose(f); snprintf(why, whysz, "Made by a newer core - update to play it"); return false; }
    if (container < P8REC_CONTAINER)
    { fclose(f); snprintf(why, whysz, "Recorded by an older core - re-record it"); return false; }

    /* 🛑 And CHECK the engine version here, not only at load. This probe
     * exists so a bad pick is refused BEFORE the content reset -- arming a
     * damaged take costs the session and explains itself only in the next
     * process. `ver` was read and then never compared, so a version-mismatched
     * take passed, the cart reset, and only then did p8rec_load refuse. That is
     * exactly the cost the probe was added to avoid, and it lands on the
     * refusal a take received from someone ELSE triggers most predictably.
     * Same wording and direction as the container check above. */
    if (ver > P8REC_ENGINE_VER)
    { fclose(f); snprintf(why, whysz, "Made by a newer core - update to play it"); return false; }
    if (ver < P8REC_ENGINE_VER)
    { fclose(f); snprintf(why, whysz, "Recorded by an older core - re-record it"); return false; }

    /* skip seed + frame count + crc, then read the identity section */
    if (fseek(f, 4 + 4 + 4, SEEK_CUR) != 0)
    { fclose(f); snprintf(why, whysz, "Recording is truncated - not playing"); return false; }

    uint16_t cnt = 0;
    if (fread(&cnt, sizeof(cnt), 1, f) != 1 || cnt > P8REC_IDENT_MAX)
    { fclose(f); snprintf(why, whysz, "Recording is damaged - not playing"); return false; }
    if (cnt == 0) { fclose(f); return true; }   /* pre-v5 take: unverifiable, still plays */

    uint16_t nl = 0;
    uint8_t want[NSHA1_DIGEST_LEN];
    std::string nm;
    if (fread(&nl, sizeof(nl), 1, f) != 1 || nl == 0 || nl > 1024)
    { fclose(f); snprintf(why, whysz, "Recording is damaged - not playing"); return false; }
    nm.assign(nl, '\0');
    if (fread(&nm[0], 1, nl, f) != nl
     || fread(want, 1, NSHA1_DIGEST_LEN, f) != NSHA1_DIGEST_LEN)
    { fclose(f); snprintf(why, whysz, "Recording is truncated - not playing"); return false; }
    fclose(f);

    uint8_t have[NSHA1_DIGEST_LEN];
    if (nsha1_file(cart_path.c_str(), have) != 0
     || memcmp(have, want, NSHA1_DIGEST_LEN) != 0)
    {   /* Name what they need. Lead with the name so a long one loses trailing
         * words rather than the point of the sentence. */
        snprintf(why, whysz, "Needs %s - load it first",
                 p8rec_short_name(nm, P8REC_NAME_FIT).c_str());
        return false;
    }
    return true;
}

static bool p8rec_load(std::string const &cart_path,
                       std::string const &explicit_path = std::string())
{
    std::string base = p8rec_cart_base(cart_path);
    std::string in;
    if (!explicit_path.empty())
    {
        in = explicit_path;
    }
    else
    {
        /* The SAME slot stat(221) displays. p8rec_slot_now() is the one
         * place that decides it; see p8rec_probe for why that matters. */
        int slot = p8rec_slot_now();
        if (!p8rec_slot_used(base, slot)) {
            fprintf(stderr, "[REC] no recording for '%s' slot %d\n", base.c_str(), slot);
            /* Used to be silent: Play Recording reset the cart, nothing played,
             * and the user was left at the title with no explanation. */
            char msg[64];
            /* "nothing for this cart" vs "this slot is empty" -- see p8rec_probe. */
            if (p8rec_highest(base) == 0)
                 snprintf(msg, sizeof(msg), "No recording for this game");
            else snprintf(msg, sizeof(msg), "Slot %d is empty", slot);
            NativeVideoWriter_Notice(msg, 4);
            return false;
        }
        in = p8rec_slot_path(base, slot);
    }

    FILE *f = fopen(in.c_str(), "rb");
    if (!f) {
        /* Was SILENT. A take that vanishes between the probe and the arm --
         * deleted from another core, a failing card, a pulled SD -- produced
         * nothing on screen at all. OpenBOR has an engine-side backstop for
         * exactly this case; same string. */
        fprintf(stderr, "[REC] cannot read %s\n", in.c_str());
        NativeVideoWriter_Notice("That recording cannot be opened", 5);
        return false;
    }

    char magic[P8REC_MAGIC_LEN];
    char cart[P8REC_CART_LEN];
    uint32_t ver = 0, n = 0, container = 0, crc = 0;
    int32_t seed = 0;
    bool ok = fread(magic, 1, P8REC_MAGIC_LEN, f) == (size_t)P8REC_MAGIC_LEN
           && memcmp(magic, P8REC_MAGIC, P8REC_MAGIC_LEN) == 0
           && fread(&container, sizeof(container), 1, f) == 1
           && fread(&ver,  sizeof(ver),  1, f) == 1
           && fread(cart,  1, sizeof(cart), f) == sizeof(cart)
           && fread(&seed, sizeof(seed), 1, f) == 1
           && fread(&n,    sizeof(n),    1, f) == 1
           && fread(&crc,  sizeof(crc),  1, f) == 1
           && n > 0 && n <= P8REC_MAX_FRAMES;
    /* A NEWER container is not corruption -- say so, rather than calling the
     * user's perfectly good file damaged. This is the whole point of splitting
     * the version out of the magic. */
    if (ok && container > P8REC_CONTAINER)
    {
        fclose(f);
        fprintf(stderr, "[REC] %s was made by a newer core (container v%u, this build v%u)\n",
                in.c_str(), container, P8REC_CONTAINER);
        NativeVideoWriter_Notice("Made by a newer core - update to play it", 6);
        return false;
    }
    if (ok && container < P8REC_CONTAINER)
    {
        fclose(f);
        fprintf(stderr, "[REC] %s uses an older container (v%u)\n", in.c_str(), container);
        NativeVideoWriter_Notice("Recorded by an older core - re-record it", 6);
        return false;
    }
    if (!ok)
    {
        fclose(f);
        fprintf(stderr, "[REC] %s is not a valid recording\n", in.c_str());
        NativeVideoWriter_Notice("That file is not a valid recording", 5);
        return false;
    }

    cart[P8REC_CART_LEN - 1] = 0;

    /* Identity section. Read it BEFORE the frame block, and treat a short read as
     * truncation rather than "an older take" -- the container bump already
     * rejected every older magic, so anything reaching this line and running out
     * of bytes is damaged. */
    g_rec_ident.clear();
    {
        uint16_t cnt = 0;
        if (fread(&cnt, sizeof(cnt), 1, f) != 1 || cnt > P8REC_IDENT_MAX)
        {
            fclose(f);
            fprintf(stderr, "[REC] %s has a bad identity section\n", in.c_str());
            NativeVideoWriter_Notice("Recording is damaged - not playing", 5);
            return false;
        }
        for (uint16_t i = 0; i < cnt; i++)
        {
            uint16_t nl = 0;
            if (fread(&nl, sizeof(nl), 1, f) != 1 || nl == 0 || nl > 1024) { cnt = 0xFFFF; break; }
            std::string nm(nl, '\0');
            P8RecIdent e;
            if (fread(&nm[0], 1, nl, f) != nl
             || fread(e.hash, 1, NSHA1_DIGEST_LEN, f) != NSHA1_DIGEST_LEN) { cnt = 0xFFFF; break; }
            e.name = nm;
            g_rec_ident.push_back(e);
        }
        if (cnt == 0xFFFF)
        {
            fclose(f);
            g_rec_ident.clear();
            fprintf(stderr, "[REC] %s: identity section is truncated\n", in.c_str());
            NativeVideoWriter_Notice("Recording is truncated - not playing", 5);
            return false;
        }
    }

    /* Verify the ENTRY cart by content. The stored name is display-only from here
     * on: it is neither unique nor stable across machines, and comparing it was
     * wrong in both directions -- a renamed v2 matched and desynced, while the
     * same cart in a different folder was refused. Siblings are verified later,
     * as the run opens them (p8rec_note_cart_file).
     *
     * An older take with an empty manifest cannot be verified at all. It still
     * plays -- refusing would brick every take made before v5 -- but the engine
     * warning below is the honest signal for it. */
    if (!g_rec_ident.empty())
    {
        uint8_t have[NSHA1_DIGEST_LEN];
        std::string want_name = g_rec_ident[0].name;
        if (nsha1_file(cart_path.c_str(), have) != 0
         || memcmp(have, g_rec_ident[0].hash, NSHA1_DIGEST_LEN) != 0)
        {
            fclose(f);
            g_rec_ident.clear();
            fprintf(stderr, "[REC] loaded cart does not match this recording"
                            " (recorded from '%s') -- not playing\n", want_name.c_str());
            {   /* Name what they need. This is the message a shared take produces
                 * most often, and the renderer is 21 cols x 3 lines = 63 chars
                 * before it truncates -- so lead with the name, and keep the
                 * instruction short enough that a long name loses trailing words
                 * rather than the whole point of the sentence. */
                char msg[96];
                snprintf(msg, sizeof(msg), "Needs %s - load it first",
                         p8rec_short_name(want_name, P8REC_NAME_FIT).c_str());
                NativeVideoWriter_Notice(msg, 6);
            }
            return false;
        }
    }
    /* Braces are load-bearing: without them the notice sat outside the if and
     * fired on EVERY successful load, so a take recorded on this exact build
     * still warned "older build - may not match" for 6 seconds. */
    if (ver != P8REC_ENGINE_VER)
    {
        /* REFUSE, do not warn-and-play (policy set 2026-08-05: refuse on both
         * cores). P8REC_ENGINE_VER is bumped ONLY for a shipped game-LOGIC change
         * that would desync old takes, so a mismatch is a KNOWN desync, not a
         * risk of one -- and a replay that drifts into nonsense is indis-
         * tinguishable from a bug in the core.
         *
         * This site disagreed with p8rec_open's own version check, which already
         * refuses with the newer/older split. Same condition, two answers; now
         * both refuse, and this one names the direction too so the message says
         * something the player can act on. */
        fprintf(stderr, "[REC] recorded on engine v%u (this build is v%u)"
                        " -- not playing, it would desync\n", ver, P8REC_ENGINE_VER);
        if (ver > P8REC_ENGINE_VER)
            NativeVideoWriter_Notice("Made by a newer core - update to play it", 6);
        else
            NativeVideoWriter_Notice("Recorded by an older core - re-record it", 6);
        /* No p8rec_reset() here: it is defined further down, and the caller
         * owns that teardown.
         *
         * The FILE* is a different matter and this comment used to cover for
         * it: `f` is opened in this function and is not visible to the caller,
         * so nothing else can close it. The sibling cart-mismatch refusal above
         * closes it; this branch did not, leaking a descriptor on every
         * version-mismatched take the user tries. */
        fclose(f);
        return false;
    }

    g_rec_frames.assign(n, 0u);
    size_t got = fread(&g_rec_frames[0], sizeof(uint32_t), n, f);
    /* Snapshot rides in the same file. Absent = an older take, which simply
     * plays with empty saves; malformed = refuse, rather than replay against
     * half-written save data and desync for no visible reason. */
    /* Verify the frame block before anything trusts it. Bit rot inside an intact
     * block passed every other guard and replayed as a plausible divergence. */
    if (got == n && p8_crc32(&g_rec_frames[0], (size_t)n * sizeof(uint32_t)) != crc)
    {
        fclose(f);
        g_rec_frames.clear();
        fprintf(stderr, "[REC] %s failed its checksum -- damaged, not playing\n", in.c_str());
        NativeVideoWriter_Notice("Recording is damaged - not playing", 5);
        return false;
    }
    bool snap_ok = (got == n) && p8snap_read(f, g_rec_snap);
    fclose(f);
    if (got == n && !snap_ok)
    {
        g_rec_frames.clear();
        g_rec_snap.clear();
        fprintf(stderr, "[REC] %s has a corrupt save payload -- not playing\n", in.c_str());
        NativeVideoWriter_Notice("Recording is damaged - not playing", 5);
        return false;
    }
    if (got != n)
    {
        g_rec_frames.clear();
        g_rec_snap.clear();
        fprintf(stderr, "[REC] %s is truncated\n", in.c_str());
        NativeVideoWriter_Notice("Recording is truncated - not playing", 5);
        return false;
    }

    g_rec_seed = seed;
    g_rec_pos  = 0;
    g_rec_file = in;   /* the take actually opened (OSD pick or newest) */
    fprintf(stderr, "[REC] playing %s (%u frames, seed %d)\n", in.c_str(), n, seed);
    return true;
}

/* Single teardown for EVERY path that ends a recorder session, so none of them
 * can forget a piece. There are five: Stop, take-over, buffer exhausted, OSD
 * hot-swap, and Quit/cart-shutdown.
 *
 * The unsetenv is the part that is easy to miss and has the widest blast
 * radius. Z8_TEST_SEED does not merely seed the PRNG -- vm.cpp also keys
 * m_time_frame_stepped on it (t() switches from wall-clock to a fixed 1/60 per
 * step), makes the cart-facing reset() re-seed to a fixed value, and pins
 * stat(80..95) to a hardcoded date. Leaving it set after a session means every
 * later cart loaded IN THIS PROCESS -- via hot-swap or Quit-then-pick -- runs
 * with a frozen PRNG and a fake clock. A cart with "random" level generation
 * would produce the identical layout every launch, silently.
 *
 * -test mode sets it deliberately at startup, so never clear it there. */
static void p8rec_reset()
{
    g_rec_mode = 0;
    g_rec_cap_failed = false;
    g_rec_frames.clear();
    g_rec_frames.shrink_to_fit();   /* clear() alone keeps the capacity forever */
    g_rec_snap.clear();             /* same reason, and it can hold real bytes */
    g_rec_snap.shrink_to_fit();
    g_rec_used.clear();             /* the used-set belongs to one take only */
    g_rec_used.shrink_to_fit();
    g_rec_ident.clear();            /* manifest likewise: one take, one identity */
    g_rec_ident.shrink_to_fit();
    g_rec_ident_ok = true;
    g_rec_pos  = 0;
    if (!g_test_trace_enabled)
        unsetenv("Z8_TEST_SEED");
}


// ── Golden-master hash trace (-test flag) ─────────────────────────────
// When enabled, each gameplay frame emits FRAME:VIDEOCRC:AUDIOCRC to the
// trace file (see src/test_trace.h — same format + hash points as the x86
// z8headless --test oracle, so traces diff directly). Default-off; inert
// unless -test is passed. In test mode the free-running audio thread is
// NOT started — the main loop pulls the engine audio deterministically.
static FILE *g_test_trace = NULL;
static long long g_test_frame = 0;         // frames traced so far
static long long g_test_frames_limit = 0;  // -testframes N: exit(0) after N (0 = unlimited)
static std::unique_ptr<z8::pico8::vm> g_vm;
static bool g_joystick_connected = false;
static SDL_Joystick* g_sdl_joystick = NULL;  // SDL joystick for polling hat/axis state

// ── Timing ────────────────────────────────────────────────────────────

static uint64_t get_time_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// ══════════════════════════════════════════════════════════════════════
//  FPGA Native Audio — DDR3 ring buffer
//  ARM writes 48KHz stereo PCM to DDR3, FPGA reads at 48KHz.
//  Same audio path as NES, SNES, Genesis — I2S + SPDIF + DAC.
//  No ALSA, no Linux kernel, no dlopen.
// ══════════════════════════════════════════════════════════════════════

static const int SRC_RATE = 22050;   // zepto8 native sample rate
static const int DST_RATE = 48000;   // FPGA audio output rate

// ── Signal handler ────────────────────────────────────────────────────
static void signal_handler(int sig)
{
    (void)sig;
    g_running = false;
    // No ALSA cleanup needed — audio goes through DDR3 to FPGA.
    // When the process dies, the FPGA ring buffer drains to silence.
}

// Save-state request flags. Set by the FPGA save-state UI dispatch path
// (DDR3 control word, polled between frames). Main loop dispatches
// the actual save/load when one of these is set.
static volatile int g_savestate_save_request = -1;  // slot 0..3, or -1
static volatile int g_savestate_load_request = -1;

// ── Audio thread — DDR3 ring buffer writer ───────────────────────────
// Renders 22050Hz mono from zepto8, upsamples to 48KHz stereo,
// writes to DDR3 ring buffer. FPGA reads at 48KHz.
// No ALSA. No kernel. Same path as every MiSTer core.

static pthread_t g_audio_thread;
static volatile bool g_audio_running = false;

// Upsample 22050 Hz mono → 48000 Hz stereo using LINEAR INTERPOLATION.
//
// Engine-source-driven choice per the NON-NEGOTIABLE rule in
// feedback_audio_type_from_engine_source.md: zepto8's
// src/pico8/sfx.cpp::get_audio produces 22050 Hz output via linear
// interpolation at the engine level (PCM streaming linear-interp at
// line 446: s0 + (s1 - s0) * frac). The wrapper resampler matches
// the engine kernel character (linear).
//
// Per output sample: 1 multiply + 1 add. Cheap, matches engine
// character exactly. Mono input is duplicated to stereo on write.
//
// Returns number of stereo output samples written.

static int upsample_mono_to_stereo(const int16_t *mono_in, int in_samples,
                                    int16_t *stereo_out, int max_out)
{
    // Fixed-point step: (22050 << 16) / 48000 = 30106. uint64 intermediate
    // to avoid the int32 overflow trap (signed-shift UB on >= 32768).
    const uint32_t step = (uint32_t)(((uint64_t)SRC_RATE << 16) / DST_RATE);
    uint32_t accum = 0;
    int out_count = 0;

    while (out_count < max_out) {
        uint32_t src_idx = accum >> 16;
        if (src_idx >= (uint32_t)in_samples) break;

        uint32_t fr = accum & 0xFFFF;

        // Linear interpolation: s0 + (s1 - s0) * frac, with frac in 16.16.
        int32_t s0 = (int32_t)mono_in[src_idx];
        int32_t s1 = (src_idx + 1 < (uint32_t)in_samples)
                     ? (int32_t)mono_in[src_idx + 1]
                     : s0;  // Boundary: hold last sample
        int32_t sum = s0 + (((s1 - s0) * (int32_t)fr) >> 16);
        if (sum > 32767)  sum = 32767;
        if (sum < -32768) sum = -32768;
        int16_t s16 = (int16_t)sum;

        stereo_out[out_count * 2 + 0] = s16;  // Left
        stereo_out[out_count * 2 + 1] = s16;  // Right (mono duplicate)
        out_count++;
        accum += step;
    }
    return out_count;
}

static void *audio_thread_func(void *arg)
{
    (void)arg;

    // Pin audio thread to CPU core 1 (render/main thread runs on the memory-fast core 0).
    // 2026-06-13 affinity rule INVERTED (render->core0, audio->core1): mem_bench shows
    // core 0 has ~1.85x core 1's DDR3 read bandwidth; sprite render is memory-bound, so
    // render takes core 0 and audio (light) takes core 1. Validated +81% on OpenBOR_7533
    // He-Man. PICO-8 is vsync-locked (not memory-bound) so neutral here -- switched for
    // the uniform cross-core rule. See feedback_affinity_render_core0_audio_core1.md.
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    int16_t mono_buf[AUDIO_BUF_SAMPLES];
    // Max output: 512 * 48000/22050 + 2 ≈ 1117 stereo samples
    int16_t stereo_buf[2400];

    fprintf(stderr, "Audio: DDR3 ring buffer, %dHz mono → %dHz stereo\n", SRC_RATE, DST_RATE);
    fflush(stderr);

    while (g_audio_running)
    {
        if (!g_vm) break;

        // Render mono audio from zepto8 at 22050Hz
        g_vm->get_audio(mono_buf, AUDIO_BUF_SAMPLES * sizeof(int16_t));

        // Upsample to 48KHz stereo
        int out_samples = upsample_mono_to_stereo(mono_buf, AUDIO_BUF_SAMPLES,
                                                   stereo_buf, 1200);
        (void)out_samples;

        // Wait for space in the DDR3 ring buffer, then write
        while (g_audio_running) {
            uint32_t space = NativeVideoWriter_AudioSpace();
            if (space >= (uint32_t)out_samples) break;
            usleep(500);  // ~0.5ms — ring buffer provides 85ms of slack
        }
        if (!g_audio_running) break;

        NativeVideoWriter_WriteAudio(stereo_buf, out_samples);
    }

    return nullptr;
}

static bool audio_thread_start()
{
    g_audio_running = true;
    int err = pthread_create(&g_audio_thread, nullptr, audio_thread_func, nullptr);
    if (err != 0) {
        fprintf(stderr, "Audio thread creation failed\n");
        g_audio_running = false;
        return false;
    }
    fprintf(stderr, "Audio thread launched (DDR3 ring buffer)\n");
    return true;
}

static void audio_thread_stop()
{
    g_audio_running = false;
    pthread_join(g_audio_thread, nullptr);
}

// ── SDL dummy audio callback ──────────────────────────────────────────
// SDL_OpenAudio with a real callback is REQUIRED for SDL's internal
// timer/event system. Without it, video rendering has severe flicker.
// Leave SDL audio open with dummy callback — never use it for output.

static void DummyAudioCallback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    memset(stream, 0, len);
}

// ── Video helpers ─────────────────────────────────────────────────────

static inline uint16_t rgba_to_565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Precomputed scale lookup tables (initialized once)
static int scale_lut_x[320]; // maps dst_x → src_x
static int scale_lut_y[240]; // maps dst_y → src_y
static int scale_off_x = 0;
static int scale_w = 0;
static bool scale_lut_ready = false;

static void init_scale_luts()
{
    if (scale_lut_ready) return;
    scale_w = SCREEN_H; // 240×240 square
    scale_off_x = (SCREEN_W - scale_w) / 2;
    for (int dx = 0; dx < scale_w; ++dx)
        scale_lut_x[dx] = dx * PICO8_W / scale_w;
    for (int dy = 0; dy < SCREEN_H; ++dy)
        scale_lut_y[dy] = dy * PICO8_H / SCREEN_H;
    scale_lut_ready = true;
}

// Clear the border strips (call once after SDL_SetVideoMode)
static void clear_borders(SDL_Surface *surface)
{
    if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);
    memset(surface->pixels, 0, surface->pitch * surface->h);
    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
}

// Blit 128x128 RGBA8 buffer to 320x240 16bpp SDL surface with StretchToFit
// Uses precomputed lookup tables — no division in the inner loop
static void blit_stretched(SDL_Surface *surface, const lol::u8vec4 *src)
{
    init_scale_luts();

    if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);

    uint16_t *dst = (uint16_t *)surface->pixels;
    int pitch16 = surface->pitch / 2;

    for (int dy = 0; dy < SCREEN_H; ++dy) {
        int sy = scale_lut_y[dy];
        const lol::u8vec4 *src_row = src + sy * PICO8_W;
        uint16_t *dst_row = dst + dy * pitch16 + scale_off_x;
        for (int dx = 0; dx < scale_w; ++dx) {
            const lol::u8vec4 &p = src_row[scale_lut_x[dx]];
            dst_row[dx] = rgba_to_565(p.r, p.g, p.b);
        }
    }

    if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
}

// ── Joystick input ────────────────────────────────────────────────────
// Reads Linux joystick events from /dev/input/js0
// Button mapping (verified on hardware):
//
//   Xbox SDL#  PICO-8
//   A    0     O button (jump, in-game only — no menu function)
//   B    1     Nothing
//   X    2     X button (confirm menu, shoot in-game)
//   Y    3     Nothing
//   Back 6     Quit
//   Start 7    Pause
//   Guide 8    Quit

// ── Path resolution ───────────────────────────────────────────────────

static std::string get_exe_dir()
{
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return "./";
    buf[len] = '\0';
    char *last = strrchr(buf, '/');
    if (last) { *(last + 1) = '\0'; return std::string(buf); }
    return "./";
}

// ── Main ──────────────────────────────────────────────────────────────

static void print_usage(const char *prog)
{
    fprintf(stderr, "zepto8 (PICO-8 emulator) for MiSTer FPGA\n\n");
    fprintf(stderr, "Usage: %s [options] <cart.p8|cart.p8.png>\n\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -fps <N>    Target frame rate (default: %d)\n", DEFAULT_FPS);
    fprintf(stderr, "  -nosound    Disable audio\n");
    fprintf(stderr, "  -nojoy      Disable joystick\n");
    fprintf(stderr, "  -nativevideo Write video to DDR3 for FPGA native output (CRT)\n");
    fprintf(stderr, "  -data <dir> Set data directory (for pico8/bios.p8)\n");
    fprintf(stderr, "  -test <file>      Write golden-master hash trace (frame:videocrc:audiocrc)\n");
    fprintf(stderr, "  -testframes <N>   With -test: exit(0) after tracing N frames\n");
    fprintf(stderr, "  -h          Show this help\n");
}

int main(int argc, char **argv)
{
    // Line-buffer stderr so log output is flushed on every newline. Without
    // this, stderr (redirected to /media/fat/logs/PICO-8/pico8.log by
    // _handler.sh) is block-buffered (~4 KB) — diagnostic output from
    // savestate_save / savestate_load can stay buffered until process exit
    // or crash, making it impossible to debug crashes mid-restore.
    setvbuf(stderr, NULL, _IOLBF, 0);

    // ── Parse arguments ───────────────────────────────────────────────
    std::string cart_path;
    std::string data_dir;
    int target_fps = DEFAULT_FPS;
    bool enable_sound = true;
    bool enable_joy = true;
    bool enable_native_video = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-fps" && i + 1 < argc)  { target_fps = atoi(argv[++i]); }
        else if (arg == "-nosound")          { enable_sound = false; }
        else if (arg == "-nojoy")            { enable_joy = false; }
        else if (arg == "-nativevideo")      { enable_native_video = true; }
        else if (arg == "-data" && i + 1 < argc) { data_dir = argv[++i]; }
        else if (arg == "-test" && i + 1 < argc) {
            const char *tp = argv[++i];
            g_test_trace = fopen(tp, "w");
            if (!g_test_trace) { fprintf(stderr, "Cannot open trace file: %s\n", tp); return 1; }
            // Fully buffered — flushed at fclose, never per line (no
            // per-frame SD I/O; see feedback_logging_hotpath_perf.md).
            setvbuf(g_test_trace, NULL, _IOFBF, 65536);
        }
        else if (arg == "-testframes" && i + 1 < argc) { g_test_frames_limit = atoll(argv[++i]); }
        else if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return 0; }
        else if (arg[0] != '-')              { cart_path = arg; }
        else { fprintf(stderr, "Unknown option: %s\n", arg.c_str()); return 1; }
    }

    // Test-trace mode steps at exactly 60 fps so hardware traces are
    // directly comparable to the x86 z8headless oracle (which always
    // steps 1/60), and forces the deterministic PRNG seed (must be set
    // before the vm is constructed — private_init_ram seeds there).
    if (g_test_trace) {
        target_fps = DEFAULT_FPS;
        setenv("Z8_TEST_SEED", "1", 0);
        g_test_trace_enabled = true;   /* p8rec_reset must not unset it here */
    }

    if (cart_path.empty()) {
        // No cart specified — will show browser (SDL mode) or wait for OSD (native video)
    }
    if (target_fps < 10) target_fps = 10;
    if (target_fps > 60) target_fps = 60;

    // Data path: for bios.p8 resolution. Defaults to binary's directory.
    // In native video mode, default to MiSTer setname folder.
    if (data_dir.empty()) {
        if (enable_native_video)
            data_dir = "/media/fat/games/PICO8/";
        else
            data_dir = get_exe_dir();
    }
    lol::sys::set_data_path(data_dir);

    // Set ZEPTO8_BASE_DIR for cart path resolution.
    // With __MISTER__ defined, private.cpp uses this for:
    //   Carts:  $ZEPTO8_BASE_DIR/Carts/
    // Config: /media/fat/config/zepto8.cfg
    // Saves:  /media/fat/saves/PICO-8/
    setenv("ZEPTO8_BASE_DIR", data_dir.c_str(), 1);

    // Create standard MiSTer Organize folders
    std::string logs_dir = "/media/fat/logs/PICO-8";
    mkdir("/media/fat/logs", 0755);
    mkdir(logs_dir.c_str(), 0755);

    // NO stderr redirect here -- deliberately. _handler.sh already runs us as
    // `exec ... > "$LOGDIR/pico8.log" 2>&1`, so both streams arrive pointing at
    // that one file, sharing ONE file offset.
    //
    // This used to dup2 a freshly-opened pico8.log over fd 2, which broke the
    // logging in two ways at once. It stole stderr away from the handler's
    // redirect, so the handler's own pico8.log stayed permanently 0 bytes while
    // every real diagnostic went to a second file the handler never rotated --
    // and "the log is empty" is indistinguishable from "the code never ran",
    // which cost a real debugging session (see the add_stat int16_t incident).
    //
    // Re-opening the SAME path instead would be worse, not better: fd 1 would
    // keep the handler's offset-0 descriptor while fd 2 got an independent one,
    // so stdout and stderr would overwrite each other in the file. One redirect,
    // owned by the handler, is the only arrangement without a hazard.
    //
    // Launched by hand outside the handler, diagnostics now go to the terminal,
    // which is what you want when you are sitting in front of it.

    // Pin main/render thread to CPU core 0 (the memory-fast core; audio uses core 1).
    // 2026-06-13 affinity rule INVERTED: core 0 has ~1.85x core 1's DDR3 bandwidth
    // (mem_bench) and sprite render is memory-bound -> render on core 0 beats the old
    // IRQ-avoidance layout (validated +81% on OpenBOR_7533 He-Man). PICO-8 is vsync-
    // locked so neutral; switched for the uniform cross-core rule. Matches OpenBOR.
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // ── Init SDL ──────────────────────────────────────────────────────
    // In native video mode, use SDL's dummy video driver — this gives us
    // the event system for joystick input without touching /dev/fb0
    // (which doesn't work when a custom FPGA core is loaded).
    SDL_Surface *screen = NULL;

    if (enable_native_video) {
        setenv("SDL_VIDEODRIVER", "dummy", 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        // Create a dummy surface — SDL needs this for the event pump
        screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, SCREEN_BPP, SDL_SWSURFACE);
        // screen may be NULL with dummy driver — that's okay
    } else {
        // Normal mode: full SDL init with video and audio
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }

        // vmode is set by pico-8.sh launcher script (320x240 rgb16)
        // Redundant call here as fallback for direct invocation
        if (system("vmode -r 320 240 rgb16 > /dev/null 2>&1") != 0) {
            // vmode not available — script handles this, safe to ignore
        }

        screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, SCREEN_BPP, SDL_SWSURFACE);
        if (!screen) {
            fprintf(stderr, "SDL_SetVideoMode failed: %s\n", SDL_GetError());
            SDL_Quit(); return 1;
        }
        SDL_ShowCursor(SDL_DISABLE);
    }

    // Open SDL joystick for state polling (hat, axis, buttons)
    if (SDL_NumJoysticks() > 0) {
        g_sdl_joystick = SDL_JoystickOpen(0);
        if (g_sdl_joystick) {
            g_joystick_connected = true;
            fprintf(stderr, "Joystick: %s (%d buttons, %d axes, %d hats)\n",
                SDL_JoystickName(0),
                SDL_JoystickNumButtons(g_sdl_joystick),
                SDL_JoystickNumAxes(g_sdl_joystick),
                SDL_JoystickNumHats(g_sdl_joystick));
        }
    }

    // Startup screen clear (3 frames, proven pattern)
    if (screen) {
        for (int i = 0; i < 3; i++) {
            SDL_FillRect(screen, NULL, 0);
            SDL_Flip(screen);
        }
        clear_borders(screen);
    }

    // ── SDL audio init (for video stability) ───────────────────────────
    // SDL_OpenAudio with real callback required for internal timer/event
    // state. Leave open with dummy callback — audio output goes through DDR3.
    // Skip in native video mode — no SDL video means this isn't needed.
    if (screen) {
        SDL_AudioSpec desired;
        memset(&desired, 0, sizeof(desired));
        desired.freq = AUDIO_RATE;
        desired.format = AUDIO_S16LSB;
        desired.channels = AUDIO_CHANNELS;
        desired.samples = 512;
        desired.callback = DummyAudioCallback;
        if (SDL_OpenAudio(&desired, nullptr) == 0) {
            SDL_PauseAudio(0);
            // Don't close — leave open with dummy callback for SDL stability
        }
    }

    // ── Init native video DDR3 writer (for FPGA native output) ────────
    // Only when -nativevideo flag is passed (requires PICO-8 FPGA core loaded).
    bool have_native_video = false;
    if (enable_native_video) {
        have_native_video = NativeVideoWriter_Init();
        if (have_native_video)
            fprintf(stderr, "Native video: DDR3 writer active (128x128 RGB565)\n");
        else
            fprintf(stderr, "Native video: DDR3 init failed, falling back to SDL\n");
    }

    // ── Keepalive thread ──────────────────────────────────────────────
    // FPGA pico8_video_reader.sv has a 30-vblank (~500ms) staleness
    // timeout that blanks the screen when no fresh frame counter ticks
    // arrive. During cart hot-swap, save-state load, or .s0-wait loops,
    // the main thread doesn't write frames for hundreds of ms — long
    // enough to hit the timeout and produce a black flash.
    //
    // Solution: lightweight thread bumps the frame counter every 150ms
    // pointing at the LAST-written buffer. FPGA's stale detector resets
    // and keeps re-displaying the frozen previous frame instead of
    // blanking. Same pattern as OpenBOR uses (CLAUDE.md keepalive rule).
    std::thread keepalive_thread;
    std::atomic<bool> keepalive_run{true};
    if (have_native_video) {
        keepalive_thread = std::thread([&keepalive_run]() {
            while (keepalive_run.load()) {
                NativeVideoWriter_KeepaliveTick();
                usleep(150000);  /* 150ms — well under the 500ms timeout */
            }
        });
    }

    // ── Init audio ─────────────────────────────────────────────────────
    // In native video mode, audio goes through DDR3 ring buffer to FPGA.
    // No ALSA init needed — the ring buffer is part of the DDR3 writer.
    bool have_audio = false;
    if (enable_sound && have_native_video)
        have_audio = true;

    // Joystick is handled by SDL (opened after SDL_Init above)
    // Cart browser opens /dev/input/js0 directly when needed

    // ── Cart browser / game loop ──────────────────────────────────────
    // Normal mode: show visual cart browser
    // Native video mode: wait for cart from OSD file browser via DDR3

    std::string carts_dir = data_dir + "Carts";

    fprintf(stderr, "=== Process started, PID=%d ===\n", getpid());

    while (g_running)
    {
        // Get cart path
        if (cart_path.empty()) {
            if (have_native_video) {
                // SC0 mode: MiSTer writes the cart's source path to
                // /media/fat/config/PICO-8.s0 instantly when the user
                // picks from the OSD. We read the path and load the
                // cart from its real SD location — that way zepto8's
                // load("sibling.p8") for multicart games resolves to
                // the right directory (vs the old /tmp/ copy approach
                // which orphaned the cart from its siblings).
                // Clear DDR3 framebuffer so screen goes black during the
                // wait loop instead of showing the previous cart's last
                // frame (FPGA keepalive keeps reading the last-written
                // buffer). Cross-applied from OpenBOR's analogous fix per
                // `feedback_clear_framebuffer_on_wait.md`. PICO-8 native
                // is 128x128 RGBA8888 (4 bytes per pixel).
                {
                    /* Returning to wait-for-cart is a session boundary: the
                     * FPS overlay resets to OFF, exactly as it does on launch.
                     * Without this it survives a pause-menu Quit -- which goes
                     * through z8_app_requestexit -> request_exit(), an
                     * IN-PROCESS return to this loop rather than the _exit(0)
                     * that z8_cart_browser takes -- and the number would then
                     * be drawn straight onto the black wait frame. */
                    NativeVideoWriter_SetFpsOverlay(0);

                    /* ClearScreen rather than WriteFrame(black): it zeroes
                     * BOTH buffers, so the departing cart's last frame cannot
                     * linger in the one we are not about to publish, and it
                     * bypasses WriteFrame's overlay draw entirely. */
                    NativeVideoWriter_ClearScreen();
                }

                fprintf(stderr, "Waiting for OSD cart selection (.s0)...\n");
                int poll_count = 0;
                while (g_running) {
                    char s0_path[512] = {0};
                    FILE *f = fopen("/media/fat/config/PICO-8.s0", "r");
                    if (f) {
                        if (fgets(s0_path, sizeof(s0_path), f)) {
                            char *nl = strchr(s0_path, '\n'); if (nl) *nl = 0;
                            char *cr = strchr(s0_path, '\r'); if (cr) *cr = 0;
                            // MiSTer writes .s0 without truncating, so a
                            // shorter new path can leak trailing bytes from
                            // a previous longer one. Trim at the last cart
                            // extension and strip trailing whitespace.
                            char *cut = NULL;
                            char *p;
                            for (p = strstr(s0_path, ".p8.png"); p; p = strstr(p+1, ".p8.png"))
                                cut = p + 7;
                            if (!cut) for (p = strstr(s0_path, ".p8"); p; p = strstr(p+1, ".p8"))
                                cut = p + 3;
                            if (cut) *cut = 0;
                            int pl = (int)strlen(s0_path);
                            while (pl > 0 && (s0_path[pl-1] == ' ' || s0_path[pl-1] == '\t'))
                                s0_path[--pl] = 0;
                        }
                        fclose(f);
                        if (strlen(s0_path) > 0) {
                            char full[1024];
                            // OSD picks write relative paths (games/PICO-8/...);
                            // MGL writes absolute paths (/media/fat/...).
                            // Detect and don't double-prefix.
                            if (s0_path[0] == '/')
                                snprintf(full, sizeof(full), "%s", s0_path);
                            else
                                snprintf(full, sizeof(full), "/media/fat/%s", s0_path);
                            cart_path = std::string(full);
                            fprintf(stderr, "OSD selected: %s\n", cart_path.c_str());
                            break;
                        }
                    }
                    if (++poll_count >= 30) {
                        fprintf(stderr, "Still waiting for .s0...\n");
                        poll_count = 0;
                    }
                    usleep(200000); // poll every 200ms
                }
                if (cart_path.empty()) break; // quit was requested
            } else {
                // SDL cart browser (dummy driver — no fbcon)
                if (g_sdl_joystick) {
                    SDL_JoystickClose(g_sdl_joystick);
                    g_sdl_joystick = NULL;
                }

                int browser_joy_fd = open("/dev/input/js0", O_RDONLY | O_NONBLOCK);
                cart_path = run_cart_browser(screen, carts_dir, browser_joy_fd);
                if (browser_joy_fd >= 0) close(browser_joy_fd);

                if (cart_path.empty()) {
                    g_running = false;
                    break;
                }

                if (SDL_NumJoysticks() > 0) {
                    g_sdl_joystick = SDL_JoystickOpen(0);
                }
            }
        }

        // Create VM and load cart
        g_vm = std::make_unique<z8::pico8::vm>();

        // Register no-op stubs for the optional std::function callbacks the VM
        // uses for desktop features (mouse pointer lock, fullscreen toggle,
        // CRT filter selection). Default-constructed std::function objects
        // throw std::bad_function_call when invoked, and that exception
        // propagates out through the Lua bindings as a function-typed error
        // value — silently breaking any cart that touches mouse_flags.locked,
        // sets fullscreen, or tweaks filters (POOM did this on its first
        // _update_buttons() call; symptom = black screen, daemon log shows
        // "err_type=function" on coresume). Stubs make these no-ops.
        g_vm->registerPointerLockCallback([](bool){});
        g_vm->registerSetFullscreenCallback([](int){});
        g_vm->registerGetFullscreenCallback([]() -> std::string { return ""; });
        g_vm->registerSetFilterCallback([](int v){ return v; });
        g_vm->registerGetFilterNameCallback([](int) -> std::string { return ""; });

        // Register cart browser extcmd — when selected from pause menu,
        // delete .s0 and exit(0). Master_Daemon's child-respawn logic
        // re-execs _handler.sh which starts a fresh PICO-8 binary that
        // boots into the wait-for-OSD-cart-selection loop with cleanly
        // zeroed DDR3 (Init() does the memset). Same architecture as
        // OpenBOR's pause-menu Quit. Universal hybrid core rule: every
        // pause-menu Quit must exit(0), not return-to-loop, so the
        // post-Quit state is identical to a fresh handler spawn (no
        // stale DDR3, no stale VM state, no audio thread quirks).
        g_vm->add_extcmd("z8_cart_browser", [](std::string const &) {
            unlink("/media/fat/config/PICO-8.s0");
            fprintf(stderr, "Quit: cleared .s0, exit(0) — Master_Daemon will respawn\n");
            fflush(stderr);
            _exit(0);
        });

        // PICO-8 spec: extcmd("shutdown") = quit the program. Same
        // behavior as the system pause-menu Quit (z8_cart_browser above)
        // — unlink .s0 so Master_Daemon respawns the handler into a
        // fresh wait-for-OSD state (no auto-remount), then _exit(0).
        //
        // Without this handler, vm.cpp's built-in switch routes
        // shutdown to a silent no-op. Carts using the
        // `extcmd("shutdown"); extcmd("reset")` fallback pattern (e.g.,
        // oblivion_eve title-screen Quit) then fall through to reset,
        // which reloads the entry cart = back to title screen — user
        // perceives "Quit just resets the game". Registering shutdown
        // here as a user extcmd takes priority over vm.cpp's switch
        // (m_extcmds checked first per api_extcmd:1837) and routes
        // shutdown to the same exit path as the system menu Quit.
        /* FPS overlay toggle, driven from bios.p8's Options submenu.
         * add_extcmd is already how this file registers MiSTer-specific verbs,
         * so this needs no change to vm.cpp/vm.h. */
        /* Recorder, driven from bios.p8's Recording submenu. Record and Play
         * both title-anchor: write the marker, then take extcmd("reset")'s
         * exact path (marker + _exit(0), .s0 preserved) so the cart re-mounts
         * from scratch and the run begins from an identical state. Stop needs
         * no reset -- it just flushes and resumes. */
        /* stat(148) = recorder mode (0 idle / 1 recording / 2 playing) so the
         * bios can render a state-aware Recording submenu. add_stat is the
         * registration path vm.cpp already provides -- no vm change needed.
         *
         * The (int16_t) cast is LOAD-BEARING, not cosmetic. api_stat funnels the
         * std::any through any_to_variant<bool, int16_t, fix32, std::string,
         * std::nullptr_t>, which matches on `a.type() == typeid(T)` -- an EXACT
         * type test. A plain `int` matches none of those, so it silently returns
         * nullptr and stat(148) reads as NIL in Lua: `m == 1` and `m == 2` are
         * both false and the submenu is stuck on its idle branch forever. */
        g_vm->add_stat(148, []() -> std::any { return (int16_t)g_rec_mode; });

        /* Baseline the OSD replay slot ONCE per process, here rather than on the
         * first poll: a pick made while the cart was still loading would
         * otherwise become the baseline and be swallowed. Anything picked from
         * this moment on is a genuine new pick. */
        g_s1_seen = p8_s1_mtime();

        /* Override the built-in "reset" so Reset Cart RESTARTS a recording
         * instead of silently ending it. Registered handlers are looked up
         * before the built-in chain (vm.cpp api_extcmd), so this wins.
         *
         * Reset used to write only the reset marker, so the respawn found no
         * recmode marker and came up idle: the take was discarded with no
         * warning and no log line. The bad case is not losing the take -- it is
         * not NOTICING, and playing on for ten minutes believing you are still
         * recording. (User-found 2026-08-01; reviewers had flagged the same
         * path as "defensible policy, but silent".)
         *
         * Re-arming is the behaviour that matches the feature: a recording is
         * title-anchored, so it always begins at the cart's start, and Reset
         * returns you to precisely that point. "Reset while recording" is
         * therefore almost always "let me do that run again", and re-arming
         * gives a clean re-take from the anchor.
         *
         * Playback is deliberately NOT re-armed: Reset during a replay is a
         * manual intervention, and take-over already covers stopping one.
         *
         * Mirrors the built-in handler otherwise (marker + _exit(0), .s0 kept
         * so the respawn re-mounts the same cart). */
        g_vm->add_extcmd("reset", [](std::string const &) {
            if (g_rec_mode == 1) {
                /* 🛑 SLOT FIRST, AND CHECKED, THEN recmode.
                 *
                 * Carry the slot, exactly as Record and Play do. Without this
                 * the re-armed take fell back to the default, so Stop wrote a
                 * slot the user never chose -- destroying whatever was in it,
                 * and then Play read a third slot and replayed someone else's
                 * take. Both cores had it; reported on hardware 2026-08-07.
                 *
                 * The ORDER is the other half, and this core was left on the
                 * old one. Writing /tmp/pico8_recmode first meant a FAILED slot
                 * write still re-armed the recorder: the respawn then found no
                 * readable marker, p8rec_slot_now() clamped to 1, and Stop
                 * overwrote slot 1's take while the user believed they were on
                 * slot 5. Reset cannot refuse -- it is already committed -- but
                 * it must not re-arm against a slot nobody knows. Come up idle
                 * instead. (OpenBOR was fixed this way in 10deadc; this is the
                 * matching half.) */
                if (!p8rec_save_slot_marker()) {
                    fprintf(stderr, "[REC] could not carry the slot -- resetting"
                                    " WITHOUT re-arming\n");
                } else {
                    FILE *m = fopen("/tmp/pico8_recmode", "w");
                    if (m) { fputs("REC", m); fclose(m); }
                    fprintf(stderr, "[REC] reset while recording -- restarting the take"
                                    " in slot %d\n", g_rec_slot);
                    /* On screen too. This core _exit(0)s immediately below, so
                     * the message has to survive the respawn -- OpenBOR carries
                     * it in a recwarn marker and PICO-8 had no equivalent, so
                     * this adds the twin. Consumed beside the recmode read. */
                    {
                        FILE *wn = fopen("/tmp/pico8_recwarn", "w");
                        if (wn) {
                            fprintf(wn, "Reset - recording restarted in slot %d", g_rec_slot);
                            fclose(wn);
                        }
                    }
                }
            } else if (g_rec_mode == 2) {
                fprintf(stderr, "[REC] reset during playback -- playback ended\n");
                {
                    FILE *wn = fopen("/tmp/pico8_recwarn", "w");
                    if (wn) { fputs("Reset - replay stopped", wn); fclose(wn); }
                }
            }
            FILE *r = fopen("/tmp/pico8_reset_marker", "w");
            if (r) fclose(r);
            fprintf(stderr, "Reset: keeping .s0, _exit(0) -- Master_Daemon will respawn same cart\n");
            fflush(stderr);
            _exit(0);
        });

        g_vm->add_extcmd("z8_rec_record", [](std::string const &) {
            /* Carry the slot across the reset (see the recovery block at arm).
             * REFUSE if it cannot be carried: an unwritten marker resolves to
             * slot 1 in the next process, so Record would overwrite SLOT 1's
             * take rather than the one the user picked, and say nothing. */
            if (!p8rec_save_slot_marker())
            {
                fprintf(stderr, "[REC] cannot write the slot marker -- not arming\n");
                NativeVideoWriter_Notice("Could not start recording", 5);
                return;
            }
            FILE *m = fopen("/tmp/pico8_recmode", "w");
            if (m) { fputs("REC", m); fclose(m); }
            FILE *r = fopen("/tmp/pico8_reset_marker", "w");
            if (r) fclose(r);
            fprintf(stderr, "[REC] arming record -- resetting to the cart start\n");
            fflush(stderr);
            _exit(0);
        });

        g_vm->add_extcmd("z8_rec_play", [](std::string const &) {
            /* Validate BEFORE the reset. This used to arm and _exit()
             * unconditionally, so a take for the wrong cart -- or a damaged one --
             * cost the user their session and only explained itself in the next
             * process, after the cart had reloaded. Here the pause menu is still
             * on screen to carry the reason. */
            char why[96];
            if (!p8rec_probe(g_cart_path_for_rec, std::string(), why, sizeof(why)))
            {
                fprintf(stderr, "[REC] not arming playback: %s\n", why);
                NativeVideoWriter_Notice(why, 6);
                return;
            }
            /* Carry the slot across the reset (see the recovery block at arm).
             * REFUSE if it cannot be carried -- otherwise the next process
             * resolves to slot 1 and replays the wrong take. */
            if (!p8rec_save_slot_marker())
            {
                fprintf(stderr, "[REC] cannot write the slot marker -- not arming\n");
                NativeVideoWriter_Notice("Could not start playback", 5);
                return;
            }
            /* A SLOT play is defined by the slot marker alone. Clear any
             * playfile first: the PLAY branch in the next process consumes one
             * unconditionally and lets it OVERRIDE the slot, so a leftover from
             * an OSD pick that never completed would silently open a different
             * take than the one this row shows. */
            unlink("/tmp/pico8_playfile");
            FILE *m = fopen("/tmp/pico8_recmode", "w");
            if (m) { fputs("PLAY", m); fclose(m); }
            FILE *r = fopen("/tmp/pico8_reset_marker", "w");
            if (r) fclose(r);
            fprintf(stderr, "[REC] arming playback -- resetting to the cart start\n");
            fflush(stderr);
            _exit(0);
        });

        g_vm->add_extcmd("z8_rec_stop", [](std::string const &) {
            /* Keep the buffer when the write FAILS, so the user can fix the
             * problem (a full SD) and pick Stop Recording again instead of
             * silently losing the take. Only a successful write -- or stopping
             * a playback, which has nothing to save -- ends the session. */
            if (g_rec_mode == 1 && !p8rec_write(g_cart_path_for_rec))
                return;
            p8rec_reset();
        });

        /* Slot picker. Wraps rather than clamps, so eight slots are reachable in
         * at most four presses from either end. A slot of 0 ("not chosen") is
         * never produced here -- once the user touches the row they own the
         * choice, and the slot-1 default applies only to someone who never
         * did. */
        g_vm->add_extcmd("z8_rec_slot_prev", [](std::string const &) {
            /* Adjust the slot the row is SHOWING. These handlers used to
             * resolve their own default, so the row and the button disagreed:
             * with the row on slot 3, LEFT jumped to slot 8. Reported on
             * hardware 2026-08-07. */
            int cur = p8rec_slot_now();
            g_rec_slot = (cur <= 1) ? P8REC_SLOTS : cur - 1;
            p8rec_publish_slot();
        });
        g_vm->add_extcmd("z8_rec_slot_next", [](std::string const &) {
            int cur = p8rec_slot_now();
            g_rec_slot = (cur >= P8REC_SLOTS) ? 1 : cur + 1;
            p8rec_publish_slot();
        });

        /* 221/222 = the slot row's label and its used/empty value. Dynamic, so
         * they are registered stats rather than entries in vm.h's static UI-text
         * table -- and registered stats take priority over that table anyway.
         *
         * They MUST return std::string: api_stat converts through
         * any_to_variant<bool,int16_t,fix32,std::string,nullptr_t>, an EXACT
         * type match with no promotion, so anything else silently reads as nil
         * in Lua and the row renders blank with no error anywhere.
         *
         * Widths are load-bearing. The menu box gives 74px from bx at 4px/char;
         * __z8_draw_slot prints the value at x+48, so "slot 8 of 8" (11 chars,
         * ends x+44) clears it and "empty" (5 chars) ends at x+68 -- inside the
         * border. */
        g_vm->add_stat(221, []() -> std::any {
            char b[24];
            snprintf(b, sizeof(b), "slot %d of %d", p8rec_slot_now(), P8REC_SLOTS);
            return std::string(b);
        });
        g_vm->add_stat(222, []() -> std::any {
            std::string base = p8rec_cart_base(g_cart_path_for_rec);
            return std::string(p8rec_slot_used(base, p8rec_slot_now()) ? "used" : "empty");
        });

        g_vm->add_extcmd("z8_fps_overlay", [](std::string const &args) {
            NativeVideoWriter_SetFpsOverlay(args.empty() ? 0 : std::atoi(args.c_str()));
        });

        g_vm->add_extcmd("shutdown", [](std::string const &) {
            unlink("/media/fat/config/PICO-8.s0");
            fprintf(stderr, "Shutdown (cart-initiated): cleared .s0, _exit(0) — Master_Daemon will respawn\n");
            fflush(stderr);
            _exit(0);
        });

        /* Recorder arm-on-startup. extcmd("reset")'s _exit(0) respawned us and
         * .s0 re-mounted the same cart, so this is the title-anchor: whatever
         * mode the marker asks for begins from an identical starting state.
         *
         * The seed MUST be set before load() -- vm::private_init_ram() reads
         * Z8_TEST_SEED and feeds it to api_srand(). Recording picks a seed and
         * stores it; playback restores the one from the file. Either way the
         * RNG stream is identical across the two runs. */
        {
            /* Recover the slot the user picked in the pause menu. It has to
             * travel through a file because Record and Play both _exit(0) and
             * let the daemon respawn us -- a choice held only in memory dies
             * with that process, and the picker would appear to do nothing. */
            {
                FILE *sf = fopen("/tmp/pico8_recslot", "r");
                if (sf)
                {
                    char buf[16]; buf[0] = 0;
                    if (fgets(buf, sizeof(buf), sf))
                    {
                        int v = atoi(buf);
                        if (v >= 1 && v <= P8REC_SLOTS) g_rec_slot = v;
                    }
                    fclose(sf);
                    unlink("/tmp/pico8_recslot");
                }
            }

            /* Consume any message the PREVIOUS process left for us. OpenBOR
             * has had this since the handler needed to report save-scratch
             * failures the engine could not see; PICO-8 had no equivalent, so a
             * reset that restarted a take could only ever reach stderr.
             *
             * Read once and unlink, so a message cannot repeat on the next
             * launch -- a stale marker outliving its event is the same defect
             * the recmode guard below exists for. Bounded read; the writer's
             * strings are far shorter. */
            {
                FILE *wn = fopen("/tmp/pico8_recwarn", "rb");
                if (wn) {
                    char wb[96];
                    size_t n = fread(wb, 1, sizeof(wb) - 1, wn);
                    fclose(wn);
                    unlink("/tmp/pico8_recwarn");
                    if (n > 0) {
                        wb[n] = 0;
                        NativeVideoWriter_Notice(wb, 5);
                        fprintf(stderr, "[REC] %s\n", wb);
                    }
                }
            }

            FILE *mk = fopen("/tmp/pico8_recmode", "r");
            if (mk)
            {
                char want[16];
                memset(want, 0, sizeof(want));
                if (!fgets(want, sizeof(want), mk)) want[0] = 0;
                fclose(mk);
                unlink("/tmp/pico8_recmode");

                char *nl = strchr(want, '\n'); if (nl) *nl = 0;
                char *cr = strchr(want, '\r'); if (cr) *cr = 0;

                if (strcmp(want, "REC") == 0)
                {
                    /* From urandom, not the PID. The multiply is invertible mod
                     * 2^32, so a PID-derived seed was recoverable from any shared
                     * take -- a machine-state value in a file meant to travel,
                     * for no reason. The seed only has to be arbitrary. */
                    g_rec_seed = 0;
                    {
                        FILE *ur = fopen("/dev/urandom", "rb");
                        if (ur) {
                            if (fread(&g_rec_seed, sizeof(g_rec_seed), 1, ur) != 1)
                                g_rec_seed = 0;
                            fclose(ur);
                        }
                    }
                    if (g_rec_seed == 0)
                        g_rec_seed = (int32_t)((uint32_t)time(NULL) * 2654435761u);
                    g_rec_seed |= 1;
                    g_rec_frames.clear();
                    g_rec_pos  = 0;
                    g_rec_mode = 1;
                    fprintf(stderr, "[REC] recording armed (seed %d)\n", g_rec_seed);
                }
                else if (strcmp(want, "PLAY") == 0)
                {
                    /* An OSD "Load Replay" pick leaves the chosen file here;
                     * the pause-menu path leaves no file and gets the newest
                     * take. Consumed either way so a stale pick can never
                     * hijack a later pause-menu Play. */
                    std::string pick;
                    if (FILE *pf = fopen("/tmp/pico8_playfile", "r"))
                    {
                        char pbuf[512] = {0};
                        if (fgets(pbuf, sizeof(pbuf), pf))
                        {
                            char *e = strchr(pbuf, '\n'); if (e) *e = 0;
                            e = strchr(pbuf, '\r');       if (e) *e = 0;
                            pick = pbuf;
                        }
                        fclose(pf);
                        unlink("/tmp/pico8_playfile");
                    }
                    /* load() before the cart so the seed is known; it also
                     * validates cart + version and refuses on mismatch. */
                    if (p8rec_load(cart_path, pick)) g_rec_mode = 2;
                }
            }
        }
        if (g_rec_mode)
        {
            char sbuf[24];
            snprintf(sbuf, sizeof(sbuf), "%d", (int)g_rec_seed);
            setenv("Z8_TEST_SEED", sbuf, 1);

            /* Isolate persistent cart state. The title anchor restarts the
             * PROCESS, not the SD card -- so cartdata written during the record
             * run, and any cstore() (which OVERLAYS the cart ROM itself), are
             * still there when the playback run boots. The playback run would
             * start from a different world than the record run did, and desync.
             *
             * Z8_SAVES_DIR already redirects BOTH get_path_save (cartdata) and
             * get_path_cstore on MiSTer -- it exists for the golden-trace
             * harness, for exactly this reason. Point it at a scratch dir and
             * clear it at every arm, so record and playback both begin from an
             * identical (empty) state. The user's real saves under
             * /media/fat/saves/PICO-8/ are never touched.
             *
             * Starting with no save data is consistent with title-anchoring,
             * which already restarts the cart from its beginning -- and because
             * the RECORD run is isolated too, the replay shows exactly what the
             * user saw while recording. */
            mkdir(P8REC_DIR, 0777);
            /* No .snapshots dir. OpenBOR keeps per-take save snapshots in one
             * (and prunes it to 20); PICO-8 does not need it -- the snapshot
             * travels INSIDE the .inp as a payload section (p8snap_write /
             * p8snap_read), so there is nothing to store beside the take. The
             * directory was created on every launch and never written to, which
             * is worse than useless: it implied a store that a future writer
             * would have inherited with no prune on either side. */
            mkdir(P8REC_SCRATCH, 0777);    /* armsnap is created by p8_copy_dir */

            if (g_rec_mode == 1) {
                /* RECORD: seed the scratch from your REAL saves, and keep a
                 * pristine copy of exactly that seed. You therefore boot with
                 * your progress and load it through the cart's own menus --
                 * which is what gets recorded. The pristine copy is written
                 * beside the .inp on Stop so playback can reproduce it. */
                int c  = p8_copy_dir(P8_REAL_SAVES, P8REC_ARMSNAP);
                int c2 = p8_copy_dir(P8REC_ARMSNAP, P8REC_SCRATCH);
                /* The second copy's return used to be discarded, and it is the
                 * one that matters. If real->armsnap is partial, armsnap and
                 * scratch still AGREE and the take is self-consistent. If
                 * armsnap->scratch is partial, the run boots WITHOUT a file the
                 * embedded payload contains -- so playback restores it, the load
                 * menu differs, and it desyncs. Refuse to arm rather than hand
                 * over a take that cannot replay. */
                if (c2 != c) {
                    fprintf(stderr, "[REC] could not prepare the save scratch (%d of %d)"
                                    " -- not recording\n", c2, c);
                    NativeVideoWriter_Notice("Could not prepare saves - not recording", 6);
                    p8rec_reset();
                } else
                    fprintf(stderr, "[REC] seeded %d save file(s) from your real saves\n", c);
            } else {
                /* PLAYBACK: restore the snapshot this take was recorded against,
                 * NOT the current saves -- otherwise the cart's load menu has a
                 * different shape and the recorded navigation picks a different
                 * slot. Falls back to empty if the take predates snapshots. */
                int want = (int)g_rec_snap.size();
                int c = p8snap_to_dir(g_rec_snap, P8REC_SCRATCH);
                /* Compare against what the take CARRIED. p8snap_to_dir skips any
                 * file it cannot write, so a read-only FS or a full SD produced
                 * "this take carries no save data" -- a false statement about the
                 * user's file -- and a partial restore was reported as success.
                 * Both are guaranteed desyncs; refuse instead of playing on. */
                if (want > 0 && c != want) {
                    fprintf(stderr, "[REC] restored %d of %d save file(s) -- not playing,"
                                    " the replay would not match\n", c, want);
                    NativeVideoWriter_Notice("Could not restore the save data in this take", 6);
                    p8rec_reset();
                } else if (c > 0) {
                    fprintf(stderr, "[REC] restored %d save file(s) carried in this take\n", c);
                } else {
                    /* Braces are load-bearing. Without them the notice sat outside
                     * the else and fired on EVERY playback arm -- including the
                     * refusal branch above, where it overwrote "could not restore
                     * this take's save data" (accurate) with "this take carries no
                     * save data" (false), because Notice() writes one buffer and
                     * the last call in a frame wins. */
                    fprintf(stderr, "[REC] this take carries no save data -- starting empty\n");
                    NativeVideoWriter_Notice("This take carries no save data", 4);
                }
            }
            /* Only redirect saves if the session actually armed. Both refusal
             * branches above call p8rec_reset(), but this ran unconditionally,
             * so "Could not prepare saves - not recording" was followed by the
             * session running on the half-seeded scratch anyway -- progress
             * looking missing, and anything new wiped at the next arm. The
             * playback refusal was worse: the scratch there holds a partially
             * restored stranger's saves. OpenBOR avoids this by construction
             * (its handler removes recmode, so isolation never rises), which is
             * why its copy of that notice can say "not recording" and mean it. */
            if (g_rec_mode)
                setenv("Z8_SAVES_DIR", P8REC_SCRATCH, 1);

            /* KNOWN LIMITATION, deliberately not "fixed": music/sfx position is
             * not part of the deterministic state.
             *
             * get_audio() advances m_state.music.offset/pattern/count and the
             * per-channel sfx fields, and carts can read those back through
             * stat(16..26), stat(46..56) and stat(57). The audio thread drains
             * the DDR3 ring in real time, so how far the music has advanced at
             * game-frame N is wall-clock dependent -- meaning a cart that gates
             * logic on music position (rhythm timing, "wait until this sfx
             * ends") can branch differently on replay.
             *
             * Frame-locking the pull from the main loop -- what -test does --
             * was tried and REVERTED (2026-08-01): it made title-screen music
             * audibly slow. The thread paces on RING SPACE, a closed loop
             * against the FPGA's own 48 kHz clock; a frame-locked pull is OPEN
             * loop, producing audio at the main loop's wall-clock rate while
             * the FPGA consumes at its crystal rate, so the mismatch
             * accumulates with nothing to correct it. -test gets away with it
             * because a trace run is short and its audio is incidental.
             *
             * Do NOT reintroduce it without closing that loop. The right shape,
             * if this is ever worth doing, is to keep the thread and make the
             * cart-visible stat() values a function of the frame counter rather
             * than of the ring -- not to move the pull. */
        }

        g_cart_path_for_rec = cart_path;
        g_vm->load(cart_path);
        g_vm->run();
        fprintf(stderr, "=== Game started: %s (PID=%d) ===\n", cart_path.c_str(), getpid());

        // Start audio thread. In test-trace mode the thread stays OFF —
        // it free-runs get_audio() in 512-sample chunks, which would race
        // the main loop's deterministic per-frame pulls and desync the
        // audio hash. The trace block below pulls, hashes, and (when the
        // ring has space) still plays the audio from the main loop.
        bool audio_started = false;
        if (have_audio && !g_test_trace) {
            audio_started = audio_thread_start();
        }

        // ── Game loop ─────────────────────────────────────────────────

        lol::u8vec4 rgba_buf[PICO8_W * PICO8_H];
        const uint64_t frame_ns = 1000000000ULL / target_fps;
        uint64_t next_frame = get_time_ns();

        bool game_running = true;
        bool hot_swap_pending = false;
        while (g_running && game_running)
    {
        // ── Save-state request handling ──────────────────────────────
        // Source: FPGA savestate_ui via DDR3 control word from the OSD
        // pause-menu and F1-F4 keyboard shortcuts. Dispatched here
        // between frames so the VM and audio thread are at a clean
        // state boundary.
        {
            static uint8_t ss_last_seq    = 0;
            static bool    ss_seq_seeded  = false;
            uint32_t ss_word = NativeVideoWriter_ReadSavestate();
            uint8_t  cmd  = NV_SsCmd (ss_word);
            uint8_t  slot = NV_SsSlot(ss_word) & 0x3;
            uint8_t  seq  = NV_SsSeq (ss_word);
            if (!ss_seq_seeded) {
                // Capture FPGA's current seq as the baseline so a stale
                // counter from a previous run doesn't trigger spurious
                // save/load on first frame.
                ss_last_seq   = seq;
                ss_seq_seeded = true;
            }
            else if (seq != ss_last_seq) {
                ss_last_seq = seq;
                if      (cmd == 1) g_savestate_save_request = slot;
                else if (cmd == 2) g_savestate_load_request = slot;
            }
        }
        if (g_savestate_save_request >= 0) {
            int slot = g_savestate_save_request;
            g_savestate_save_request = -1;
            if (g_vm) g_vm->savestate_save(slot);
        }
        if (g_savestate_load_request >= 0) {
            int slot = g_savestate_load_request;
            g_savestate_load_request = -1;
            /* A savestate load is a total world warp -- it restores m_ram
             * (including the PRNG) and m_state behind the recorder's back, and
             * it is reachable from the OSD pause menu and F1-F4 at any time.
             * It is INVISIBLE to the input stream, so a replay that hits one
             * desyncs completely, and a recording that contains one can never
             * be replayed. Same out-of-band-state-change class as the hot-swap:
             * end the session rather than produce a recording that cannot work. */
            if (g_rec_mode) {
                fprintf(stderr, "[REC] savestate load -- %s discarded"
                                " (a savestate is not part of the input stream)\n",
                        g_rec_mode == 1 ? "recording" : "playback");
                /* Same as the hot-swap: mode-aware, because loading a save
                 * state during a replay reported a discarded recording. */
                NativeVideoWriter_Notice(g_rec_mode == 1
                    ? "Save state loaded - recording discarded"
                    : "Save state loaded - replay stopped", 5);
                p8rec_reset();
            }
            if (g_vm) g_vm->savestate_load(slot);
        }

        uint64_t now = get_time_ns();

        // Frame timing: sleep for most of the wait, then busy-wait for precision.
        // usleep on MiSTer's Linux can oversleep by 1-5ms, which at 60fps
        // (16.6ms budget) would drop us to ~50fps. Sleep until 2ms before
        // target, then spin for the remaining time.
        if (now < next_frame) {
            uint64_t wait = next_frame - now;
            if (wait > 2500000) // more than 2.5ms remaining
                usleep((unsigned int)((wait - 2000000) / 1000)); // sleep to within 2ms
            while (get_time_ns() < next_frame) {} // spin-wait the rest
        }
        next_frame += frame_ns;

        // Don't fall more than 2 frames behind
        uint64_t actual = get_time_ns();
        if (actual > next_frame + frame_ns * 2)
            next_frame = actual;

        // -- Input: read joysticks from DDR3 (FPGA writes hps_io data) --
        // Main_MiSTer has exclusive access to /dev/input/js*, so we read
        // joystick state directly from DDR3 where the FPGA puts it. PICO-8
        // supports up to 8 players via btn(b,p); MiSTer's hps_io provides
        // up to 4 USB joysticks, so we feed players 0..3.
        //
        // Standard PICO-8 input semantics: each player's controller maps
        // ONLY to their own player slot. Single-player carts read btn(b)
        // (= btn(b, 0)) and only see P1; multi-player carts read btn(b, p)
        // for each p. Don't OR across controllers here — that breaks
        // single-player carts (P2 would pause / move P1's character).
        // The bios.p8 pause-menu uses __z8_anybtnp() helpers so that once
        // P1 opens the menu, any player can navigate it.
        //
        // CONF_STR: "J1,O,X,Pause;" / "jn,B,Y,Start;" (SNES: B=Xbox A, Y=Xbox X)
        // joystick_N bits: 0=R 1=L 2=D 3=U 4=Xbox A(O) 5=Xbox X(X) 6=Start(Pause)
        if (have_native_video) {
            /* Pack the frame as 4 players x 7 buttons. This is the recorder's
             * choke point: everything the VM ever sees as input passes through
             * here exactly once per frame, so capturing here is complete by
             * construction and injecting here is indistinguishable from a
             * human playing. */
            uint32_t live = 0;
            for (int p = 0; p < 4; p++) {
                uint32_t joy = NativeVideoWriter_ReadJoystick(p);
                uint32_t b = ((joy >> 1) & 1)        /* Left  */
                           | (((joy >> 0) & 1) << 1) /* Right */
                           | (((joy >> 3) & 1) << 2) /* Up    */
                           | (((joy >> 2) & 1) << 3) /* Down  */
                           | (((joy >> 4) & 1) << 4) /* O     <- Xbox A */
                           | (((joy >> 5) & 1) << 5) /* X     <- Xbox X */
                           | (((joy >> 6) & 1) << 6);/* Pause <- Start  */
                live |= b << (p * 7);
            }

            uint32_t use = live;

            if (g_rec_mode == 1) {
                if (g_rec_frames.size() < P8REC_MAX_FRAMES) {
                    /* push_back THROWS on allocation failure -- it does not
                     * return an error the way OpenBOR's realloc does, and
                     * nothing in this binary catches, so an OOM here was
                     * std::terminate: the process dies, Master_Daemon respawns
                     * it, and the player loses the session with nothing on
                     * screen saying why. Catch it and stop the take the same
                     * way the other core does, with the same words.
                     *
                     * This is the per-FRAME allocation, not every one: the
                     * per-cart-file g_rec_used and g_rec_ident push_backs are
                     * still unguarded. Far rarer, and left alone rather than
                     * wrapped speculatively -- but the hole is not closed, so
                     * do not read this as "the recorder is OOM-safe".
                     *
                     * p8rec_reset() also shrink_to_fit()s, so the memory is
                     * genuinely returned rather than held as capacity. Worth
                     * doing on the path that just failed to allocate, though
                     * the exposure here is ~8 MB at the frame cap (~24 MB
                     * across a doubling), not the 144 MB the other core holds
                     * -- an earlier version of this comment borrowed those
                     * numbers and overstated it by roughly 20x. */
                    try {
                        g_rec_frames.push_back(live);
                    } catch (const std::bad_alloc &) {
                        fprintf(stderr, "[REC] out of memory at %u frames"
                                        " -- recording stopped\n",
                                (unsigned)g_rec_frames.size());
                        /* 🛑 FLUSH, do not discard. This called p8rec_reset(),
                         * which clears the frames -- so the take was GONE, while
                         * the frame-cap path directly below SAVES what it has.
                         * Two "cannot record any more" paths with opposite
                         * outcomes, and the one that destroys hours of capture is
                         * the one the user cannot see coming. Nothing about a
                         * failed allocation invalidates the frames already
                         * buffered; the notice even says "stopped", not
                         * "discarded". Write them, exactly as the cap does.
                         *
                         * Announced AFTER the write, for the reason the cap path
                         * below states about itself: a notice that claims an
                         * outcome before attempting it is wrong whenever the
                         * attempt fails, and this one has a REASON to fail --
                         * the allocator just said no. A user told "stopped" who
                         * then finds no take has been lied to twice.
                         *
                         * 🛑 The success string is BYTE-IDENTICAL to OpenBOR's,
                         * and must stay that way -- an earlier version of this
                         * edit reworded it to "stopped and saved" and silently
                         * broke notice parity with the other core.
                         *
                         * The failure string has no OpenBOR twin, and that is a
                         * MECHANISM difference rather than a divergence: PICO-8
                         * writes inline here and knows the result, while OpenBOR
                         * defers the flush to its stop path via the recstop
                         * marker, so it cannot know yet and reports a failed
                         * save from that path instead. */
                        bool saved = p8rec_write(g_cart_path_for_rec);
                        NativeVideoWriter_Notice(
                            saved ? "Out of memory - stopped. Saves off until reset."
                                  : "Out of memory - stopped. Could not save the take.", 6);
                        p8rec_reset();
                    }
                } else {
                    /* At the cap, STOP and flush rather than silently dropping
                     * frames while the menu still reads "Stop Recording" -- the
                     * player would carry on believing it was still capturing,
                     * and the file would just end mid-session with nothing
                     * recording that it had been truncated. */
                    fprintf(stderr, "[REC] frame cap reached (%u frames)"
                                    " -- stopping and saving\n",
                            (unsigned)g_rec_frames.size());
                    /* Announce AFTER the write, not before: this said
                     * "- saved" and only then tried. A failure did correct
                     * itself, because the next notice overwrites the buffer,
                     * but a message should not assert something not yet
                     * known.
                     *
                     * This comment used to end "Same ordering fixed on OpenBOR."
                     * It was not -- I wrote that while editing this file only.
                     * OpenBOR still announces its cap before the write. */
                    if (p8rec_write(g_cart_path_for_rec)) {
                        {   /* Fires AFTER p8rec_write, so it lands over that
                             * function's own Saved/Replaced notice in the same
                             * frame -- last call wins, and the cap wording is the
                             * more informative one. Same end state as OpenBOR,
                             * which folds the two together in its stop path. */
                            char m[96];
                            snprintf(m, sizeof(m),
                                     "Slot %d saved at the length limit."
                                     " Saves off until reset.", p8rec_slot_now());
                            NativeVideoWriter_Notice(m, 6);
                        }
                        p8rec_reset();
                    }
                    else if (!g_rec_cap_failed) {
                        /* The write failed AT THE CAP.
                         *
                         * Do NOT drop to mode 0: that left the frames
                         * allocated while the menu drew its IDLE list, so
                         * Stop Recording vanished and the take could be
                         * neither saved nor discarded -- the opposite of
                         * the normal Stop path, which deliberately keeps
                         * the buffer so the user can free space and retry.
                         *
                         * Do NOT retry every frame either: at the cap this
                         * block runs on every single frame, so a retry is a
                         * full file write per frame and the notice re-fires
                         * forever. That is exactly what OpenBOR does today.
                         *
                         * Stay armed, say it once, and let Stop Recording be
                         * the retry -- which is what the notice already
                         * tells the user to do. */
                        g_rec_cap_failed = true;
                        NativeVideoWriter_Notice("Could not save the recording - free space and Stop again", 7);
                    }
                }
            }
            else if (g_rec_mode == 2) {
                /* A sibling cart the run just opened did not match the manifest.
                 * p8rec_note_cart_file has already named the file on screen; it
                 * cannot stop playback from inside vm::load_cart, because that is
                 * called mid-load with the VM part-way through a cart swap. Stop
                 * here instead, at the next frame boundary, which is the same
                 * place take-over and end-of-take stop. Injecting even one more
                 * frame into content known to differ is the silent divergence the
                 * manifest exists to prevent. */
                if (!g_rec_ident_ok) {
                    fprintf(stderr, "[REC] stopping playback: content mismatch\n");
                    p8rec_reset();
                }
                else if (g_rec_pos < g_rec_frames.size()) {
                    /* Take-over: any button the human presses that the
                     * recording is not already holding hands control back.
                     * Deliberately edge-based, so resting on a button the
                     * recording also holds does not end playback. */
                    /* Seed the baseline from the FIRST replayed frame rather than
                     * from 0. With a 0 baseline, frame 0 computes
                     * pressed = live & ~0 == live, so ANY bit already held when
                     * the cart mounts reads as a fresh press and kills playback at
                     * frame 0. A resting finger, a second connected pad, or an
                     * off-centre analog stick mapped to a direction would make
                     * Play Recording fail EVERY time, after a full cart reload,
                     * with no visible cause. */
                    static uint32_t prev_live = 0;
                    if (g_rec_pos == 0) prev_live = live;
                    /* Pause (bit 6) is EXCLUDED. Opening the pause menu counted
                     * as a take-over, so p8rec_reset() ran and stat(148) was
                     * already 0 by the time the menu drew -- meaning the
                     * Recording submenu rendered its IDLE branch and
                     * "Stop Playback" could never be reached. The menu item
                     * exists to stop a replay; let it. */
                    /* 🛑 DO NOT mask the pause bits out of this test to make the
                     * menu's "Stop Playback" item reachable. Bit 6 was masked for
                     * exactly that reason and it is wrong -- reverted 2026-08-05
                     * after the same change on OpenBOR_7533 was traced through.
                     *
                     * During playback the VM's buttons come from `use`, which is
                     * the RECORDED frame (`use = g_rec_frames[g_rec_pos++]`
                     * below), so a live pause press that does not trip take-over
                     * never reaches g_vm->button(p,6,...) -- the menu simply does
                     * not open, and the pause button does nothing at all.
                     *
                     * Taking over IS how a live pause reaches the VM: it leaves
                     * `use` at `live`, so the same press opens the menu under
                     * live control. "Stop Playback" is then unreachable because
                     * it is unnecessary -- playback has already stopped. */
                    uint32_t pressed = live & ~prev_live;
                    prev_live = live;
                    if (pressed) {
                        fprintf(stderr, "[REC] take-over at frame %u -- playback stopped\n",
                                (unsigned)g_rec_pos);
                        /* NOT "Reset to play" -- reset does not replay. The recorder is
                         * already torn down by the time this draws, so the reset
                         * extcmd writes only the reset marker and the cart just
                         * restarts. What reset DOES undo is the save isolation,
                         * which is the part worth acting on. */
                        NativeVideoWriter_Notice("Took over - saves off until reset.", 6);
                        p8rec_reset();
                    } else {
                        use = g_rec_frames[g_rec_pos++];
                    }
                } else {
                    fprintf(stderr, "[REC] playback finished (%u frames)\n",
                            (unsigned)g_rec_frames.size());
                    NativeVideoWriter_Notice("Replay over - saves off until reset.", 6);
                    p8rec_reset();
                }
            }

            for (int p = 0; p < 4; p++) {
                uint32_t b = (use >> (p * 7)) & 0x7f;
                g_vm->button(p, 0, (b >> 0) & 1);  // Left
                g_vm->button(p, 1, (b >> 1) & 1);  // Right
                g_vm->button(p, 2, (b >> 2) & 1);  // Up
                g_vm->button(p, 3, (b >> 3) & 1);  // Down
                g_vm->button(p, 4, (b >> 4) & 1);  // O     ← Xbox A
                g_vm->button(p, 5, (b >> 5) & 1);  // X     ← Xbox X
                g_vm->button(p, 6, (b >> 6) & 1);  // Pause ← Start (per-player)
            }
        }

        // Check if VM requested exit or user pressed Back — return to browser
        if (!g_vm->is_running() || g_return_to_browser)
            game_running = false;

        // Check for OSD cart-swap during gameplay — poll .s0 for a
        // path different from the currently-loaded cart. SC0 mode: the
        // user picks a new cart from MiSTer's file browser, MiSTer
        // updates .s0 instantly. Throttle to ~2 Hz so we're not
        // hammering the SD card.
        if (have_native_video && game_running) {
            static int swap_poll = 0;
            if (++swap_poll >= 30) { // ~30 frames @ 60fps = 0.5s
                swap_poll = 0;

                /* OSD "Load Replay" (SC1). Checked before the cart poll because
                 * a replay pick ends this process outright. */
                time_t s1now = p8_s1_mtime();
                if (s1now != g_s1_seen) {
                    g_s1_seen = s1now;
                    char s1_path[512] = {0};
                    FILE *rf = fopen("/media/fat/config/PICO-8.s1", "r");
                    if (rf) {
                        if (fgets(s1_path, sizeof(s1_path), rf)) {
                            char *e = strchr(s1_path, '\n'); if (e) *e = 0;
                            e = strchr(s1_path, '\r');       if (e) *e = 0;
                            /* MiSTer writes .s1 WITHOUT truncating, so a shorter
                             * new path leaves trailing bytes from the previous
                             * longer one -- same trap as .s0. Cut at the ext. */
                            char *cut = NULL, *p;
                            for (p = strstr(s1_path, ".inp"); p; p = strstr(p+1, ".inp")) cut = p + 4;
                            if (cut) *cut = 0;
                            int pl = (int)strlen(s1_path);
                            while (pl > 0 && (s1_path[pl-1] == ' ' || s1_path[pl-1] == '\t')) s1_path[--pl] = 0;
                        }
                        fclose(rf);
                    }
                    if (strlen(s1_path) > 0) {
                        char full[1024];
                        if (s1_path[0] == '/')
                            snprintf(full, sizeof(full), "%s", s1_path);
                        else
                            snprintf(full, sizeof(full), "/media/fat/%s", s1_path);
                        fprintf(stderr, "[REC] OSD replay pick: %s\n", full);

                        /* Title-anchor it exactly like pause-menu Play: carry the
                         * chosen path, ask for PLAY, KEEP .s0 so the same cart
                         * re-mounts, and _exit so the respawn begins at the
                         * cart's first frame. Arming mid-run would replay the
                         * take against a world the recording never saw.
                         *
                         * A recording in progress dies with the process, which is
                         * the intended behaviour: picking a replay abandons the
                         * take, same as any other hot-swap. */
                        /* Probe before the reset. This is THE path wrong picks come
                         * from -- the OSD browser will happily hand us a take for
                         * any cart in the library -- and it used to arm and _exit()
                         * without opening the file, so the user lost their session
                         * and got the explanation only after the reload. Refusing
                         * here leaves the run untouched. */
                        char why[96];
                        if (g_rec_mode == 1)
                        {
                            /* Refuse rather than exit, exactly as the OSD slot
                             * Play path does. Exiting here would discard an
                             * unwritten take with no confirm and no message,
                             * while pause-Quit raises a LOSE RECORDING dialog
                             * for the same loss. Same wording on both cores. */
                            fprintf(stderr, "[REC] OSD replay refused -- recording in progress\n");
                            NativeVideoWriter_Notice("Stop the recording first", 5);
                        }
                        else if (!p8rec_probe(g_cart_path_for_rec, std::string(full), why, sizeof(why)))
                        {
                            fprintf(stderr, "[REC] OSD replay refused: %s\n", why);
                            NativeVideoWriter_Notice(why, 6);
                            /* Baseline is already advanced by the mtime check above,
                             * so a refused pick does not re-fire every frame. */
                        }
                        else
                        {
                        /* Carry the slot across the respawn. PICO-8 normally
                         * persists it at ADOPTION, which covers an OSD move --
                         * but a slot moved ONLY in the pause menu never triggers
                         * adoption (the pub-fresh poll is consumed, and the next
                         * one finds rslot already equal), so nothing had written
                         * the marker. After this respawn the recovery block
                         * found none, the slot clamped to 1, and adoption is
                         * frozen for the whole replay at mode 2.
                         *
                         * Unlike OpenBOR's equivalent this cannot clobber a
                         * pending marker: PICO-8's recovery read runs before
                         * this poll ever does, so nothing is left unconsumed. */
                        /* 🛑 REFUSE, don't warn-and-continue. Record and Play
                         * both return without arming on this identical failure,
                         * and the reset path was fixed to do the same. Unlike
                         * Reset this is NOT already committed -- nothing has
                         * _exit()ed yet -- so proceeding would reload the cart,
                         * find no marker, clamp to slot 1, and freeze adoption
                         * for the whole replay at mode 2: the pause picker shows
                         * 1 while the OSD shows the real slot, for the entire
                         * run. That is the two-pickers-disagree state this work
                         * exists to prevent, reached through the failure
                         * branch. The probe refusal directly above already
                         * demonstrates the pattern. */
                        if (!p8rec_save_slot_marker())
                        {
                            /* Fall through WITHOUT arming, exactly as the probe
                             * refusal above does -- the mtime baseline is already
                             * advanced, so this does not re-fire every frame. */
                            fprintf(stderr, "[REC] could not carry the slot -- not playing\n");
                            NativeVideoWriter_Notice("Could not start playback", 5);
                        }
                        else
                        {
                            if (FILE *pf = fopen("/tmp/pico8_playfile", "w")) { fprintf(pf, "%s\n", full); fclose(pf); }
                            if (FILE *mm = fopen("/tmp/pico8_recmode",  "w")) { fprintf(mm, "PLAY\n");     fclose(mm); }
                            if (FILE *rm = fopen("/tmp/pico8_reset_marker", "w")) fclose(rm);
                            _exit(0);
                        }
                        }
                    }
                }

                /* OSD "Replay Slot" 1-8 + "Play Replay". The FPGA publishes both
                 * in the spare upper half of the JOY0 qword (0x0C), which it
                 * already writes every frame, so reading it here costs nothing.
                 *
                 * Two jobs, and the first is the one that makes the feature
                 * honest: mirror the OSD's slot into g_rec_slot so the
                 * pause-menu row shows the SAME number. One slot value, two
                 * displays -- the rule f7a870e was written to enforce.
                 *
                 * Play reuses p8rec_probe() -- the same policy the pause menu's
                 * z8_rec_play calls -- so an empty slot refuses with the same
                 * words here as there instead of resetting the cart to find
                 * out. Refusals stay put; only a valid take resets. */
                {
                    /* 🛑 A Play pressed during the ~1-2 s respawn window is
                     * DISCARDED, deliberately and silently -- stated here
                     * because from the outside it looks like a dropped press.
                     *
                     * Record and Play both _exit() and let Master_Daemon
                     * respawn us, so briefly nothing reads 0x0C while the FPGA
                     * keeps counting presses. On restart rs_primed is false and
                     * we adopt whatever rs_seq reached as the baseline.
                     *
                     * Queuing would be worse: the press that armed THIS respawn
                     * is the one still echoing, so honouring it would restart
                     * the cart again as soon as it loaded, and a Play queued
                     * behind a Record arm would discard the take just started.
                     * Nothing survives the exit to report against either. */
                    static bool     rs_primed = false;
                    static unsigned rs_last_seq = 0;
                    uint32_t rw    = NativeVideoWriter_ReadReplay();
                    unsigned rseq  = (rw >> 16) & 0xFFu;
                    unsigned rcmd  = rw & 3u;
                    int      rslot = (int)((rw >> 8) & 7u) + 1;   /* wire is 0-based */

                    /* Seed the sequence from the FIRST real read, never from 0.
                     * DDR3 holds whatever the previous core left there, so an
                     * unseeded compare can fire a spurious Play the moment the
                     * core loads. */
                    if (!rs_primed) {
                        rs_primed = true;
                        /* Consume a publish that happened before priming, or the
                         * skip lands on the wrong poll -- harmless, but it would
                         * be spent on a poll it was not meant for. */
                        g_rec_pub_fresh = false;
                    } else {
                      /* The skip below covers ADOPTION ONLY. Play must still be
                       * evaluated on a skipped poll: `rs_last_seq` advances
                       * unconditionally at the bottom, so a Play pulse arriving
                       * during a skipped poll would be consumed and lost for
                       * good rather than merely delayed.
                       *
                       * `rslot` is the slot as of the once-per-frame DDR3 write,
                       * NOT a snapshot taken at the Play press. (An earlier
                       * version of this comment claimed the RTL latched cmd and
                       * slot together on the pulse. It does not -- rs_slot_lat
                       * tracks the current slot every cycle, which is exactly
                       * what lets an OSD move be mirrored without pressing
                       * Play.) That is still the right value to act on: it is
                       * what the OSD is displaying when the user presses. */
                      if (g_rec_pub_fresh) {
                        /* We published a slot since the last poll, and the echo
                         * we are holding may pre-date it: `rs_slot_lat` tracks
                         * every clk_sys cycle, but the reader only WRITES 0x0C
                         * once per frame, in ST_WRITE_JOY0. So for up to one
                         * frame after a pause-menu press, 0x0C still reports the
                         * OLD slot -- and adopting it would revert the press the
                         * user just made, then correct itself a poll later.
                         *
                         * Measured shape: the poll runs every 30 frames, the
                         * stale window is <= 1 frame, so this bit the user on
                         * roughly 1-3% of presses -- often enough to be reported
                         * as "the slot sometimes jumps back", and bad enough to
                         * matter if they hit Record inside that window.
                         *
                         * Skipping exactly ONE poll is sufficient and cannot
                         * stall: the next poll is ~0.5 s later, by which time
                         * the reader has written 0x0C many times over. A
                         * simultaneous OSD move is simply picked up then. */
                        g_rec_pub_fresh = false;
                      } else if (g_rec_mode == 0 && rslot != p8rec_slot_now()) {
                        /* 🛑 Adoption is FROZEN for the life of a take. The slot
                         * row is drawn on the idle branch only, so while
                         * recording there is nothing on screen showing which
                         * slot Stop will write to -- and the OSD picker is still
                         * reachable. Without this gate, moving it mid-take
                         * silently retargets Stop onto a DIFFERENT slot and
                         * destroys the take that was in it, with the only
                         * feedback arriving after the damage. Once Record has
                         * committed to a target, that target is fixed. */
                        g_rec_slot = rslot;
                        /* The slot has to survive the reset: Play and Record
                         * both respawn the process, so a choice held only in
                         * memory dies with it.
                         *
                         * This is the ONE of five call sites that does not
                         * refuse on failure, and that is deliberate rather than
                         * an oversight: the other four are ARMING, where an
                         * unwritten marker means the next process resolves to
                         * slot 1 and records or plays the WRONG take. Adoption
                         * arms nothing -- g_rec_slot above is already correct
                         * in memory, and whichever arm path runs next writes
                         * the marker itself and checks it there. Refusing here
                         * would throw away a correct adoption to prevent
                         * nothing.
                         *
                         * It must still be VISIBLE, though. Ignoring the result
                         * outright meant a failing /tmp produced no trace at
                         * all until something later behaved oddly. */
                        if (!p8rec_save_slot_marker())
                            fprintf(stderr, "[REC] could not persist the OSD slot"
                                            " (arming will retry and report)\n");
                      }

                        if (rcmd == 1u && rseq != rs_last_seq) {
                            fprintf(stderr, "[REC] OSD play replay, slot %d\n", rslot);
                            if (g_rec_mode == 1)
                            {
                                /* 🛑 Refuse rather than exit. This is the ONLY
                                 * route that can reach Play while recording --
                                 * the pause submenu hides Play at mode 1 -- and
                                 * an unguarded _exit() here would discard an
                                 * unwritten take with no confirm and no message,
                                 * while pause-Quit raises a LOSE RECORDING
                                 * dialog for exactly the same loss. A Notice()
                                 * placed just before _exit() would not help: the
                                 * process is gone before a frame publishes. */
                                fprintf(stderr, "[REC] OSD play refused -- recording in progress\n");
                                NativeVideoWriter_Notice("Stop the recording first", 5);
                            }
                            else
                            {
                            char why[96];
                            int  want = rslot;
                            if (!p8rec_probe(g_cart_path_for_rec, std::string(), why, sizeof(why), want))
                            {
                                /* Refused: it has already said why on screen.
                                 * Fall through and keep playing -- do NOT reset.
                                 *
                                 * This branch does not commit `want`, matching
                                 * OpenBOR, which only commits once every check
                                 * has passed.
                                 *
                                 * 🛑 That is NOT the same as "a refusal leaves
                                 * both pickers where they were", which is what
                                 * this comment used to claim. The adoption
                                 * branch above has usually ALREADY moved
                                 * g_rec_slot on this very poll -- whenever
                                 * mode==0 and the pub-fresh skip was not taken,
                                 * which is the ordinary case of moving the OSD
                                 * picker and then pressing Play. Adopting there
                                 * is correct (the OSD is the side that moved),
                                 * so the behaviour is right; only the claim was
                                 * wrong. The freeze this describes really only
                                 * bites at mode==2 or on a skipped poll. */
                                fprintf(stderr, "[REC] OSD replay refused: %s\n", why);
                                NativeVideoWriter_Notice(why, 6);
                            }
                            else
                            {
                                g_rec_slot = want;
                                if (!p8rec_save_slot_marker())
                                {
                                    /* Arming against a slot we could not record
                                     * would resolve to slot 1 in the next
                                     * process and replay the wrong take. */
                                    fprintf(stderr, "[REC] cannot write the slot marker -- not arming\n");
                                    NativeVideoWriter_Notice("Could not start playback", 5);
                                }
                                else
                                {
                                /* Same as the pause-menu arm: a SLOT play must
                                 * not inherit a stale playfile, which the PLAY
                                 * branch would honour over the slot. */
                                unlink("/tmp/pico8_playfile");
                                if (FILE *mm = fopen("/tmp/pico8_recmode", "w"))
                                { fprintf(mm, "PLAY\n"); fclose(mm); }
                                if (FILE *rm = fopen("/tmp/pico8_reset_marker", "w")) fclose(rm);
                                fprintf(stderr, "[REC] arming playback from OSD -- resetting to the cart start\n");
                                fflush(stderr);
                                _exit(0);
                                }
                            }
                            }
                        }
                    }
                    rs_last_seq = rseq;
                }

                char s0_path[512] = {0};
                FILE *f = fopen("/media/fat/config/PICO-8.s0", "r");
                if (f) {
                    if (fgets(s0_path, sizeof(s0_path), f)) {
                        char *nl = strchr(s0_path, '\n'); if (nl) *nl = 0;
                        char *cr = strchr(s0_path, '\r'); if (cr) *cr = 0;
                        char *cut = NULL; char *p;
                        for (p = strstr(s0_path, ".p8.png"); p; p = strstr(p+1, ".p8.png")) cut = p + 7;
                        if (!cut) for (p = strstr(s0_path, ".p8"); p; p = strstr(p+1, ".p8")) cut = p + 3;
                        if (cut) *cut = 0;
                        int pl = (int)strlen(s0_path);
                        while (pl > 0 && (s0_path[pl-1] == ' ' || s0_path[pl-1] == '\t')) s0_path[--pl] = 0;
                    }
                    fclose(f);
                    if (strlen(s0_path) > 0) {
                        char full[1024];
                        // Same absolute-path detection as the startup poll —
                        // MGL hot-swaps would otherwise double-prefix /media/fat/.
                        if (s0_path[0] == '/')
                            snprintf(full, sizeof(full), "%s", s0_path);
                        else
                            snprintf(full, sizeof(full), "/media/fat/%s", s0_path);
                        if (cart_path != full) {
                            fprintf(stderr, "Hot-swap: new OSD cart %s\n", full);

                            /* Stop the recorder. A hot-swap is an IN-PROCESS
                             * reload, so unlike Reset/Quit (which _exit and get
                             * a clean slate for free) every global survives it.
                             * Leaving the recorder running across a cart change
                             * is wrong in both directions:
                             *   playing   -> the old cart's input stream keeps
                             *                injecting into the NEW cart, which
                             *                desyncs immediately (user-reported
                             *                2026-07-31);
                             *   recording -> frames from two different carts land
                             *                in one buffer, and a later Stop
                             *                writes them under the NEW cart's
                             *                name -- a poisoned file that passes
                             *                the cart-name guard on playback.
                             * A hot-swap means this session was abandoned, so
                             * discard rather than flush; that also matches
                             * OpenBOR, whose _exit(1) swap discards implicitly.
                             * NOTE: scoped to the OSD .s0 path ONLY. A cart-driven
                             * load() (multicart sub-cart) must NOT reset -- the
                             * original run did the same load at the same frame, so
                             * it is part of the deterministic replay. */
                            if (g_rec_mode) {
                                fprintf(stderr, "[REC] cart hot-swap -- %s discarded\n",
                                        g_rec_mode == 1 ? "recording" : "playback");
                                /* Build the string from the MODE. The fprintf above already
                                 * distinguished the two; the notice did not, so hot-swapping
                                 * during a REPLAY announced a discarded recording that never
                                 * existed. */
                                NativeVideoWriter_Notice(g_rec_mode == 1
                                    ? "Cart changed - recording discarded"
                                    : "Cart changed - replay stopped", 4);
                                p8rec_reset();
                            }
                            /* Globals the cart swap must ALSO clear. This core reloads
                             * IN PROCESS, so anything not reset here survives into the
                             * next cart; OpenBOR is immune only because it _exit()s.
                             *
                             * 🛑 g_rec_slot is deliberately NOT cleared here, and this
                             * used to do the opposite. The old clear claimed to stop
                             * cart A's slot reaching cart B -- but once the slot also
                             * lives in the OSD it cannot: the picker keeps its value,
                             * the poll re-adopts it within half a second, and the
                             * recovery marker can restore it too. The clear only
                             * produced a transient window where the pause row read
                             * "slot 1" while the OSD read something else, which is the
                             * two-displays-disagreeing failure this design exists to
                             * prevent.
                             *
                             * Carrying it is also harmless: takes are per-content
                             * (<content>_<slot>.inp), so slot 5 on cart B is a
                             * different file from slot 5 on cart A -- nothing of cart
                             * A's can be overwritten by cart B -- and the row still
                             * reads used/empty before anything is committed. OpenBOR
                             * has always carried it; the two cores now agree.
                             *
                             * SetFpsOverlay(0) lives in the wait-for-OSD block, which a
                             * hot-swap skips -- while the bios reload resets the menu row
                             * to off. The row then read "off" over a visible overlay and
                             * the next press appeared to do nothing: the same
                             * displayed-value vs acted-on-value split as the slot bugs. */
                            NativeVideoWriter_SetFpsOverlay(0);
                            /* The cart is about to reload, so it is safe to hand
                             * saves back now -- and necessary, or the next cart
                             * would keep writing into the recorder scratch dir.
                             * Deliberately NOT inside p8rec_reset: get_path_save
                             * and get_path_cstore resolve at CALL time, so
                             * unsetting after a plain Stop Recording would move a
                             * still-running cart's save location under it. */
                            unsetenv("Z8_SAVES_DIR");

                            cart_path = std::string(full);
                            game_running = false;
                            hot_swap_pending = true;
                        }
                    }
                }
            }
        }

        g_vm->step(1.0f / target_fps);

        // Present ONLY completed frames. When a heavy scene exhausts the
        // instruction budget, the cart coroutine is suspended mid-_draw;
        // displaying that state ships a half-drawn frame (new sky + far
        // geometry over the previous frame's near geometry = the Virtua
        // Racing "shattered track", 2026-07-22). PICO-8 never shows a
        // partial draw — over-budget carts just present late — so on a
        // budget-yield tick we skip render+present and let the FPGA
        // keepalive hold the last completed frame. Trace mode still
        // hashes every step (determinism tool — goldens unchanged).
        bool presentable = g_vm->last_tick_presentable();

        // Render video
        if (presentable || g_test_trace)
            g_vm->render(rgba_buf, (size_t)128 * 128);

        // ── Golden-master hash trace (-test) ─────────────────────────
        // Hash points mirror tools/z8headless.cpp exactly: video = CRC32
        // over R,G,B of the native 128x128 render output (pre-upscale);
        // audio = CRC32 over this frame's 22050 Hz mono engine output
        // (367/368-sample pacing, hashed pre-upsample so the platform
        // upsampler/ring cannot affect the trace).
        if (g_test_trace) {
            uint32_t vh = 0;
            for (int i = 0; i < PICO8_W * PICO8_H; ++i) {
                uint8_t px[3] = { rgba_buf[i].r, rgba_buf[i].g, rgba_buf[i].b };
                vh = tt_crc32(vh, px, 3);
            }
            int ns = tt_audio_samples_for_frame(g_test_frame, SRC_RATE, DEFAULT_FPS);
            static int16_t tmono[512];
            g_vm->get_audio(tmono, (size_t)ns * sizeof(int16_t));
            uint32_t ah = tt_crc32(0, tmono, (size_t)ns * sizeof(int16_t));
            tt_emit(g_test_trace, g_test_frame, vh, ah);

            // Keep the run audible: upsample + write to the DDR3 ring
            // when there's space, drop when full — the hash was taken
            // pre-upsample, so playback can never affect the trace.
            if (have_native_video && have_audio) {
                static int16_t tstereo[2400];
                int out = upsample_mono_to_stereo(tmono, ns, tstereo, 1200);
                if (NativeVideoWriter_AudioSpace() >= (uint32_t)out)
                    NativeVideoWriter_WriteAudio(tstereo, out);
            }

            g_test_frame++;
            if (g_test_frames_limit && g_test_frame >= g_test_frames_limit) {
                fclose(g_test_trace);
                fprintf(stderr, "[test] trace complete (%lld frames) — exiting\n", g_test_frame);
                exit(0);
            }
        }

        if (!presentable) {
            // Mid-_draw budget yield: hold the last completed frame
            // (keepalive keeps the FPGA fed). Skipping render+present
            // also returns this tick's CPU to the cart.
        } else if (have_native_video) {
            // FPGA native path: write 128×128 RGBA8 → DDR3 as RGB565
            // The FPGA reader polls DDR3 and outputs scaled native video
            NativeVideoWriter_WriteFrame(rgba_buf, PICO8_W, PICO8_H);
        } else {
            // Fallback: SDL framebuffer → Linux /dev/fb0 → HDMI scaler
            blit_stretched(screen, rgba_buf);
            SDL_UpdateRect(screen, 0, 0, SCREEN_W, SCREEN_H);
        }

        // Audio is handled by the separate thread — no audio work here
    }

        // Game ended — clean up for next cart
        fprintf(stderr, "=== Game ended (PID=%d) ===\n", getpid());
        if (audio_started) {
            audio_thread_stop();
            audio_started = false;
        }
        g_vm.reset();
        g_return_to_browser = false;

        // Hot-swap: cart_path already set to new cart, outer loop reloads it.
        // Normal exit (user picked Quit from PICO-8 pause menu): clear
        // cart_path AND delete .s0 — otherwise the next outer-loop poll
        // would see the old .s0 path and immediately reload the same cart
        // (perceived as "Quit just resets the cartridge").
        // Reached by BOTH cart-driven shutdown (extcmd("shutdown")) AND the
        // pause-menu Quit. NOTE: Quit does NOT exit the process -- bios.p8's
        // Quit row calls extcmd("z8_app_requestexit") -> vm::request_exit(),
        // which only clears m_is_running, so we come back through here and the
        // outer loop re-enters the wait-for-OSD state IN THE SAME PROCESS.
        // (A previous version of this comment claimed Quit exit(0)s via
        // z8_cart_browser and never reached here. That is false, and it is why
        // the recorder reset below was missing.)
        if (!hot_swap_pending) {
            /* Session boundary -- same reasoning as the hot-swap reset above.
             * Every global survives an in-process return, so a recorder left
             * armed here carries into whatever cart the user picks next:
             *   playing   -> the old cart's stream injects into the new cart;
             *   recording -> both carts' frames land in one buffer and a later
             *                Stop writes them under the NEW cart's name, which
             *                PASSES the name guard on playback and only then
             *                desyncs.
             * Discard rather than flush: Quit means the session was abandoned. */
            if (g_rec_mode) {
                fprintf(stderr, "[REC] session ended -- %s discarded\n",
                        g_rec_mode == 1 ? "recording" : "playback");
                p8rec_reset();
            }
            unsetenv("Z8_SAVES_DIR");   /* next cart gets the real saves back */

            cart_path.clear();
            unlink("/media/fat/config/PICO-8.s0");
            fprintf(stderr, "Cart-driven shutdown: cleared .s0, will wait for OSD\n");
            // Cart-driven shutdown: explicit DDR3 clear, since binary stays
            // alive (vs Quit which exits and re-inits).
            if (have_native_video)
                NativeVideoWriter_ClearScreen();
        } else {
            fprintf(stderr, "Hot-swap: reloading %s\n", cart_path.c_str());
        }

        // Clear screen before returning to browser
        if (screen) {
            for (int i = 0; i < 3; i++) {
                SDL_FillRect(screen, NULL, 0);
                SDL_Flip(screen);
            }
        }

    } // end of browser/game while loop

    // ── Shutdown ──────────────────────────────────────────────────────
    if (g_sdl_joystick) {
        SDL_JoystickClose(g_sdl_joystick);
        g_sdl_joystick = NULL;
    }
    keepalive_run.store(false);
    if (keepalive_thread.joinable()) keepalive_thread.join();
    NativeVideoWriter_Shutdown();

    SDL_CloseAudio();
    SDL_Quit();

    return 0;
}
