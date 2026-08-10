#!/usr/bin/env python3
"""Slot-selection conformance for the PICO-8 recorder.

Covers the checklist items that are pure logic and therefore do NOT need a
controller in front of a MiSTer -- items 1 (8 bounded slots), 3 (slot paths
GENERATED, never parsed) and 11 (nothing picked: the row opens on slot 1, Play
takes the highest OCCUPIED one). Every other item needs button presses in-core
and belongs to the 10-step hardware procedure.

🛑 The default is slot 1, NOT the first free slot. It was briefly first-free,
on the reasoning that a first-timer should not clobber an existing take; that
made the menu open somewhere different depending on what had already been
recorded, which is unpredictable exactly where a user wants predictability, so
4110894 removed it and matched OpenBOR. This file asserted the OLD contract and
kept naming a function that commit deleted, so it exited 2 ("could not cut")
from Aug 7 until 2026-08-10 -- and because diff_harness.yml was
workflow_dispatch-only, nothing said so. A gate nobody runs protects nothing.
The regression guard is therefore the opposite of what it used to be: the
default must NOT move when the library fills up.

Like test_snap_payload.py, this CUTS THE REAL FUNCTIONS out of
src/mister_main.cpp and compiles them natively. It never reimplements them: a
reimplementation passes while the shipped code rots, which is the whole reason
that rule exists.

Exit codes are DISTINCT so a refusal can never be confused with a driver error:
  0 = all checks passed
  1 = a check failed
  2 = harness/driver error (could not cut, could not compile)
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "..", "src", "mister_main.cpp"))

WANT = ["p8rec_slot_path", "p8rec_slot_used", "p8rec_highest", "p8rec_slot_now"]


def cut_source():
    """Lift the four slot helpers verbatim out of the shipped source."""
    src = open(SRC, encoding="utf-8", errors="replace").read()

    m = re.search(r'^#define\s+P8REC_SLOTS\s+(\d+)', src, re.M)
    if not m:
        print("could not find P8REC_SLOTS in mister_main.cpp", file=sys.stderr)
        sys.exit(2)
    slots = int(m.group(1))

    m = re.search(r'^static const char \*P8REC_DIR\s*=\s*"([^"]+)"', src, re.M)
    if not m:
        print("could not find P8REC_DIR", file=sys.stderr)
        sys.exit(2)

    bodies = []
    for fn in WANT:
        # from the function's return type through its closing brace at col 0
        m = re.search(r'^[A-Za-z_][\w:<>* ]*\b%s\s*\([^)]*\)\s*\n\{.*?^\}' % re.escape(fn),
                      src, re.M | re.S)
        if not m:
            print("could not cut %s out of mister_main.cpp" % fn, file=sys.stderr)
            sys.exit(2)
        bodies.append(m.group(0))

    return slots, "\n\n".join(bodies)


HARNESS = r"""
#include <string>
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>

static const char *P8REC_DIR = "%(dir)s";
#define P8REC_SLOTS %(slots)d
static int g_rec_slot = 0;

%(bodies)s

static int fails = 0;
static void ck(bool ok, const char *what) {
    printf("  %%-58s %%s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static void touch(const std::string &p) { FILE *f = fopen(p.c_str(), "wb"); if (f) fclose(f); }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %%s <tmpdir>\n", argv[0]); return 2; }
    std::string base = "maze";

    /* item 3 -- paths are GENERATED. The old scan accepted any <stem>_<digits>
     * while the loader rebuilt the name from the parsed integer, so a
     * zero-padded copy opened a file that did not exist and Play broke
     * permanently. Generated paths cannot disagree with themselves. */
    ck(p8rec_slot_path(base, 1) == std::string(P8REC_DIR) + "/maze_1.inp", "slot 1 path is <dir>/maze_1.inp");
    ck(p8rec_slot_path(base, 8) == std::string(P8REC_DIR) + "/maze_8.inp", "slot 8 path is <dir>/maze_8.inp");
    ck(p8rec_slot_path(base, 8).find("maze_08") == std::string::npos,       "never zero-pads");

    /* item 1 -- bounded at 8 */
    ck(P8REC_SLOTS == 8, "P8REC_SLOTS == 8");

    /* item 11 -- nothing picked yet. The row opens on slot 1 and Play finds
     * nothing. g_rec_slot == 0 is the "not yet chosen" sentinel. */
    g_rec_slot = 0;
    ck(p8rec_slot_now() == 1,    "unset slot resolves to 1");
    ck(p8rec_highest(base) == 0, "empty library: highest occupied is 0");

    /* the clamp is what makes the row survive junk from any source -- a stale
     * /tmp marker, a hand-edited value, an off-by-one in a caller */
    g_rec_slot = 0;   ck(p8rec_slot_now() == 1, "slot 0 clamps to 1");
    g_rec_slot = -3;  ck(p8rec_slot_now() == 1, "negative slot clamps to 1");
    g_rec_slot = P8REC_SLOTS + 1;
    ck(p8rec_slot_now() == 1, "slot past the end clamps to 1");
    g_rec_slot = 3;   ck(p8rec_slot_now() == 3, "an in-range choice is preserved");
    ck(g_rec_slot == 3, "resolving does not disturb an in-range choice");

    /* occupy 1 and 2 */
    touch(p8rec_slot_path(base, 1));
    touch(p8rec_slot_path(base, 2));
    ck(p8rec_slot_used(base, 1),  "slot 1 reads used once written");
    ck(!p8rec_slot_used(base, 3), "slot 3 still reads empty");
    ck(p8rec_highest(base) == 2,  "with 1+2 taken, highest occupied is 2");

    /* 🛑 THE REGRESSION GUARD. The default must NOT depend on the library:
     * that is precisely the first-free behaviour 4110894 removed. If someone
     * reintroduces it, this is the check that fails. */
    g_rec_slot = 0;
    ck(p8rec_slot_now() == 1, "default stays 1 even though slot 1 is taken");

    /* a GAP must not confuse Play: occupy 5, leave 3 and 4 empty. Play must
     * still find the newest-numbered take, not the first hole. */
    touch(p8rec_slot_path(base, 5));
    ck(p8rec_highest(base) == 5, "gap: highest occupied is 5");
    g_rec_slot = 0;
    ck(p8rec_slot_now() == 1,    "gap: default still 1, not the hole at 3");

    /* a full library is where a first-free default used to have no
     * non-destructive answer. There is nothing to decide now -- the row still
     * opens on 1, reads "used", and the overwrite notice names the slot. */
    for (int s = 1; s <= P8REC_SLOTS; s++) touch(p8rec_slot_path(base, s));
    ck(p8rec_highest(base) == P8REC_SLOTS, "full library: highest is 8");
    g_rec_slot = 0;
    ck(p8rec_slot_now() == 1,              "full library: default still 1");
    ck(p8rec_slot_used(base, 1),           "full library: slot 1 reads used, so the row warns");

    /* a sibling cart whose stem ends in _<digits> must not alias in. This is
     * the exact shape that broke the old parsing scan: cart "maze_12" writing
     * maze_12_1.inp made highest("maze") parse "12_1" as 12. */
    touch(p8rec_slot_path("maze_12", 1));
    ck(p8rec_highest(base) == P8REC_SLOTS, "sibling cart maze_12 does not alias into maze");
    ck(p8rec_highest("maze_12") == 1,      "sibling cart keeps its own library");

    /* out-of-range probes must simply read empty, never crash or wrap */
    ck(!p8rec_slot_used(base, 0),  "slot 0 reads empty (never valid)");
    ck(!p8rec_slot_used(base, 99), "slot 99 reads empty (never valid)");

    printf("\n%%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
"""


def main():
    slots, bodies = cut_source()
    print("cut %s from src/mister_main.cpp (P8REC_SLOTS=%d)\n" % (", ".join(WANT), slots))

    with tempfile.TemporaryDirectory() as work:
        rec = os.path.join(work, "replays")
        os.makedirs(rec)
        csrc = os.path.join(work, "probe.cpp")
        with open(csrc, "w", encoding="utf-8") as f:
            f.write(HARNESS % {"dir": rec.replace("\\", "/"), "slots": slots, "bodies": bodies})

        exe = os.path.join(work, "probe")
        r = subprocess.run(["g++", "-O1", "-g", "-std=c++17", "-o", exe, csrc],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr)
            print("the extracted slot helpers did not compile", file=sys.stderr)
            return 2

        r = subprocess.run([exe, work], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        if r.stderr:
            sys.stderr.write(r.stderr)
        # 0 pass / 1 fail / 2 driver error -- assert the exact code, never "!= 0"
        if r.returncode not in (0, 1):
            print("probe exited %d (driver error, not a verdict)" % r.returncode, file=sys.stderr)
            return 2
        return r.returncode


if __name__ == "__main__":
    sys.exit(main())
