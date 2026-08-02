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
#define P8REC_CONTAINER  4u        /* framing version; bump on any layout change */
// Bump ONLY on a shipped game-LOGIC change that would desync existing
// recordings (VM semantics, RNG, timestep, input mapping). NOT for
// render/audio/UI/perf changes -- the fixed timestep makes replay independent
// of frame cost, so those can never desync a recording.
#define P8REC_ENGINE_VER 1u
#define P8REC_MAX_FRAMES 2000000u   /* ~9.2 h at 60 fps; caps a runaway file */
#define P8REC_CART_LEN   256   /* holds a relative path now, not just a basename */

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

static const char *P8REC_DIR = "/media/fat/games/PICO-8/Replays";

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
/* All of it lives under saves/, NOT under Replays/ and NOT under savestates/.
 *
 * Not under Replays/ because the OSD "Load Replay" picker lists directories as
 * well as files: a snapshot sitting beside each take turned that picker into a
 * list of decoy folders which appear EMPTY when opened, since they hold .p8d.txt
 * saves and the picker filters to .inp. Replays/ now contains nothing but takes.
 *
 * Not under savestates/ because none of this is an emulator save state -- it is
 * cartdata the game itself wrote. (Same reasoning applies to OpenBOR, whose .sNN
 * files are script-saves despite the directory we named "savestates".) */
static const char *P8REC_STATE   = "/media/fat/saves/PICO-8/.replays";
static const char *P8REC_SCRATCH = "/media/fat/saves/PICO-8/.replays/.scratch";
static const char *P8REC_ARMSNAP = "/media/fat/saves/PICO-8/.replays/.armsnap";
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
            static const char *EXT = ".p8d.txt";
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

static bool p8snap_write(FILE *f, std::vector<P8SnapFile> const &v)
{
    uint32_t c = (uint32_t)v.size();
    if (fwrite(&c, sizeof(c), 1, f) != 1) return false;
    for (size_t i = 0; i < v.size(); i++) {
        uint32_t nl = (uint32_t)v[i].name.size();
        uint32_t dl = (uint32_t)v[i].data.size();
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
        if (fread(&dl, sizeof(dl), 1, f) != 1 || dl > (16u << 20)) return false;
        total += dl;
        if (total > (8u << 20)) return false;
        P8SnapFile sf; sf.name = nm; sf.data.resize(dl);
        if (dl && fread(&sf.data[0], 1, dl, f) != dl) return false;
        /* These files now arrive from OTHER PEOPLE, so the name is untrusted.
         *
         * Bare filename only -- never a path -- so nothing can be written
         * outside the scratch. Embedded NULs are handled: nm is length-counted,
         * so find() scans all nl bytes, and the later c_str() truncation can
         * only shorten a name, never re-introduce a separator.
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
        static const char *SNAP_EXT = ".p8d.txt";
        size_t extlen = strlen(SNAP_EXT);
        if (nm.empty() || nm.find('/') != std::string::npos
                       || nm.find('\\') != std::string::npos
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
        /* Files only. The snapshot store (.replays) lives INSIDE the directory
         * being snapshotted, so seeding would otherwise try to copy it into
         * every new snapshot -- harmless in that copying a directory as a file
         * just fails, but it would skew the count and is plainly wrong. Any
         * other stray subdirectory under saves/ was mishandled the same way. */
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

/* Highest existing index for <base>_N.inp, or 0 if there are none. */
static int p8rec_highest(std::string const &base)
{
    int best = 0;
    DIR *d = opendir(P8REC_DIR);
    if (!d) return 0;
    std::string pre = base + "_";
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        std::string n = e->d_name;
        if (n.size() <= pre.size() + 4) continue;
        if (n.compare(0, pre.size(), pre) != 0) continue;
        if (n.compare(n.size() - 4, 4, ".inp") != 0) continue;

        /* The middle must be ALL digits, and the index is then accepted only if
         * the name we would RECONSTRUCT from it matches the one on disk.
         *
         * The scan accepts any <base>_<middle>.inp but p8rec_load rebuilds the
         * name as base + "_" + to_string(hi) + ".inp" -- so anything the two
         * disagree about breaks Play permanently, because load only ever opens
         * the highest. A zero-padded copy (maze_007) parsed to 7 and then
         * opened a maze_7 that does not exist. Worse, a sibling cart whose stem
         * is <base>_<digits> aliased in: cart "maze_12" writing maze_12_1.inp
         * made p8rec_highest("maze") parse "12_1" as 12, so Play opened a
         * maze_12 that never existed -- and every later take for "maze" was
         * pushed to _13. Requiring an exact round-trip rejects both. */
        std::string mid = n.substr(pre.size(), n.size() - pre.size() - 4);
        if (mid.empty() || mid.find_first_not_of("0123456789") != std::string::npos)
            continue;
        long k = strtol(mid.c_str(), NULL, 10);
        if (k <= 0 || k > 999999) continue;                 /* also kills atoi overflow */
        if (pre + std::to_string(k) + ".inp" != n) continue; /* e.g. zero-padded */
        if ((int)k > best) best = (int)k;
    }
    closedir(d);
    return best;
}

/* Numbered library, like OpenBOR's: each Stop writes <cart>_<N+1>.inp, so a
 * new recording never overwrites an older one. */
/* Returns true if the session can be torn down: the file was written, or there
 * was nothing to write. False means the take is STILL IN MEMORY and the caller
 * must leave the recorder armed so Stop can be retried. */
static bool p8rec_write(std::string const &cart_path)
{
    if (g_rec_frames.empty())
    {
        /* Never create an empty file: it would take the highest index and then
         * shadow every real take, since playback only ever opens the highest. */
        fprintf(stderr, "[REC] nothing captured -- not writing a file\n");
        return true;   /* nothing to save, but the session is legitimately over */
    }
    std::string base = p8rec_cart_base(cart_path);
    std::string out  = std::string(P8REC_DIR) + "/" + base + "_"
                     + std::to_string(p8rec_highest(base) + 1) + ".inp";

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
           && fwrite(&crc, sizeof(crc), 1, f) == 1
           && fwrite(&g_rec_frames[0], sizeof(uint32_t), n, f) == (size_t)n;
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
        int hi = p8rec_highest(base);
        if (!hi) {
            fprintf(stderr, "[REC] no recording for '%s'\n", base.c_str());
            /* Used to be silent: Play Recording reset the cart, nothing played,
             * and the user was left at the title with no explanation. */
            NativeVideoWriter_Notice("No recording for this cart", 4);
            return false;
        }
        in = std::string(P8REC_DIR) + "/" + base + "_" + std::to_string(hi) + ".inp";
    }

    FILE *f = fopen(in.c_str(), "rb");
    if (!f) { fprintf(stderr, "[REC] cannot read %s\n", in.c_str()); return false; }

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
    if (base != cart)
    {
        fclose(f);
        fprintf(stderr, "[REC] this recording is for cart '%s' but '%s' is loaded"
                        " -- not playing\n", cart, base.c_str());
        {   /* Name the cart they need, not just "refused" -- this is the one
             * message a shared recording will produce most often. */
            char msg[96];
            snprintf(msg, sizeof(msg), "Recording is for %s - load that cart", cart);
            NativeVideoWriter_Notice(msg, 6);
        }
        return false;
    }
    /* Braces are load-bearing: without them the notice sat outside the if and
     * fired on EVERY successful load, so a take recorded on this exact build
     * still warned "older build - may not match" for 6 seconds. */
    if (ver != P8REC_ENGINE_VER)
    {
        fprintf(stderr, "[REC] recorded on engine v%u (this build is v%u) -- may"
                        " desync; press any button to take over\n", ver, P8REC_ENGINE_VER);
        NativeVideoWriter_Notice("Recorded on an older build - may not match", 6);
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
        NativeVideoWriter_Notice("Recording damaged - not playing", 5);
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
    g_rec_frames.clear();
    g_rec_frames.shrink_to_fit();   /* clear() alone keeps the capacity forever */
    g_rec_snap.clear();             /* same reason, and it can hold real bytes */
    g_rec_snap.shrink_to_fit();
    g_rec_used.clear();             /* the used-set belongs to one take only */
    g_rec_used.shrink_to_fit();
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
    // this, stderr (redirected to /media/fat/logs/PICO-8/pico8.log by the
    // daemon) is block-buffered (~4 KB) — diagnostic output from
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

    // Redirect stderr to log file for diagnostics
    // Captures: startup info, cart printh() output, errors, hot-swap events
    {
        std::string log_path = logs_dir + "/pico8.log";
        FILE *logf = fopen(log_path.c_str(), "w");
        if (logf) {
            dup2(fileno(logf), STDERR_FILENO);
            fclose(logf);
        }
    }

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
                FILE *m = fopen("/tmp/pico8_recmode", "w");
                if (m) { fputs("REC", m); fclose(m); }
                fprintf(stderr, "[REC] reset while recording -- restarting the take\n");
            } else if (g_rec_mode == 2) {
                fprintf(stderr, "[REC] reset during playback -- playback ended\n");
            }
            FILE *r = fopen("/tmp/pico8_reset_marker", "w");
            if (r) fclose(r);
            fprintf(stderr, "Reset: keeping .s0, _exit(0) -- Master_Daemon will respawn same cart\n");
            fflush(stderr);
            _exit(0);
        });

        g_vm->add_extcmd("z8_rec_record", [](std::string const &) {
            FILE *m = fopen("/tmp/pico8_recmode", "w");
            if (m) { fputs("REC", m); fclose(m); }
            FILE *r = fopen("/tmp/pico8_reset_marker", "w");
            if (r) fclose(r);
            fprintf(stderr, "[REC] arming record -- resetting to the cart start\n");
            fflush(stderr);
            _exit(0);
        });

        g_vm->add_extcmd("z8_rec_play", [](std::string const &) {
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
            mkdir(P8REC_STATE, 0777);      /* parent of scratch + armsnap */
            mkdir(P8REC_SCRATCH, 0777);

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
                    NativeVideoWriter_Notice("Could not restore this take's save data", 6);
                    p8rec_reset();
                } else if (c > 0)
                    fprintf(stderr, "[REC] restored %d save file(s) carried in this take\n", c);
                else
                    fprintf(stderr, "[REC] this take carries no save data -- starting empty\n");
                    NativeVideoWriter_Notice("This take carries no save data", 4);
            }
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
                        NativeVideoWriter_Notice("Save state loaded - recording discarded", 5);
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
                    g_rec_frames.push_back(live);
                } else {
                    /* At the cap, STOP and flush rather than silently dropping
                     * frames while the menu still reads "Stop Recording" -- the
                     * player would carry on believing it was still capturing,
                     * and the file would just end mid-session with nothing
                     * recording that it had been truncated. */
                    fprintf(stderr, "[REC] frame cap reached (%u frames)"
                                    " -- stopping and saving\n",
                            (unsigned)g_rec_frames.size());
                            NativeVideoWriter_Notice("Recording hit its length limit - saved", 5);
                    if (p8rec_write(g_cart_path_for_rec))
                        p8rec_reset();
                    else
                        g_rec_mode = 0;   /* write failed; do not spin on the cap */
                }
            }
            else if (g_rec_mode == 2) {
                if (g_rec_pos < g_rec_frames.size()) {
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
                    uint32_t pressed = live & ~prev_live & ~(1u << 6);
                    prev_live = live;
                    if (pressed) {
                        fprintf(stderr, "[REC] take-over at frame %u -- playback stopped\n",
                                (unsigned)g_rec_pos);
                        NativeVideoWriter_Notice("You took over - replay stopped", 4);
                        p8rec_reset();
                    } else {
                        use = g_rec_frames[g_rec_pos++];
                    }
                } else {
                    fprintf(stderr, "[REC] playback finished (%u frames)\n",
                            (unsigned)g_rec_frames.size());
                    NativeVideoWriter_Notice("Replay finished - you have control", 4);
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
                        if (FILE *pf = fopen("/tmp/pico8_playfile", "w")) { fprintf(pf, "%s\n", full); fclose(pf); }
                        if (FILE *mm = fopen("/tmp/pico8_recmode",  "w")) { fprintf(mm, "PLAY\n");     fclose(mm); }
                        if (FILE *rm = fopen("/tmp/pico8_reset_marker", "w")) fclose(rm);
                        _exit(0);
                    }
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
                                        NativeVideoWriter_Notice("Cart changed - recording discarded", 4);
                                p8rec_reset();
                            }
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
