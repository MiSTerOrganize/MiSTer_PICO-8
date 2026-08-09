#!/usr/bin/env python3
"""Pause-menu mode-stability check. Same file on every hybrid core.

THE RULE: a replay injects presses BY POSITION, not by meaning. So the pause
menu's ROWS must not change with recorder mode. A label, a colour or a value
may vary; a row may not.

WHY THIS EXISTS (2026-08-08, user-found): Quit's confirm dialog only existed
while mrec_mode == 1. On playback the mode is 2, the confirm was ABSENT, and
the injected press that had originally chosen "Back" landed on "Quit" itself --
so the replay exited to a black screen at exactly that moment. The rule was
already written down, but scoped to the Recording submenu; this was an entire
menu STATE, one menu over. Scope was the mistake.

TWO CHECKS, because neither alone is enough:

  A. ROW COUNTS. The per-mode row counts must match the cross-core invariant
     (idle 4 / recording 2 / playing 1). Direct, exact, and cheap -- but blind
     to anything outside the Recording submenu, which is where the bug was.

  B. TRIPWIRE. Every mode-conditional site in the pause-menu source must appear
     in the reviewed baseline beside this script. A NEW one fails until a human
     answers: does this change what is DRAWN IN a row, or WHICH ROWS EXIST?
     This is the check that would have caught the quit confirm.

Baseline entries are keyed on the normalized source line, not the line number,
so ordinary edits above them do not churn it.

  python tools/harness/menu_mode_parity_check.py [--update-baseline]

Exit 0 clean, 1 on a finding, 2 on a usage/config error (distinct, so a driver
mistake can never be mistaken for a pass -- see feedback_parser_tests_cut_real_source).
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
BASELINE = os.path.join(HERE, "menu_mode_parity_baseline.txt")

# Row counts every core must agree on. Changing these is a DELIBERATE act:
# update the rule in feedback_hybrid_core_recording_replay.md in the same
# commit, and re-verify a replay that pauses, or the change is a desync.
WANT_ROWS = {"idle": 4, "recording": 2, "playing": 1}

CORES = {
    "openbor": {
        "file": "patches/pausemenu_patch.c",
        # recmode is the local copy that actually drives the layout; grepping
        # only mrec_mode would miss every branch that matters and give a check
        # that passes vacuously.
        "symbols": ["mrec_mode", "recmode"],
        # /tmp/openbor_recmode is a marker path, not a conditional.
        "ignore": ["openbor_recmode"],
        "comment": ("*", "/*", "//"),
    },
    "pico8": {
        "file": "src/pico8/bios.p8",
        "symbols": ["stat(148)"],
        "ignore": [],
        "comment": ("--",),
    },
}


def detect():
    for name, cfg in CORES.items():
        p = os.path.join(REPO, cfg["file"])
        if os.path.exists(p):
            return name, cfg, p
    return None, None, None


def conditional_sites(path, cfg):
    """Every non-comment line mentioning a mode symbol, normalized."""
    out = []
    for i, raw in enumerate(open(path, encoding="utf-8",
                                 errors="replace").read().splitlines(), 1):
        st = raw.strip()
        if not st or st.startswith(cfg["comment"]):
            continue
        if not any(sym in st for sym in cfg["symbols"]):
            continue
        if any(ig in st for ig in cfg["ignore"]):
            continue
        # drop a trailing comment so re-wording one does not churn the baseline
        st = re.split(r"/\*|//|--\s", st)[0].strip()
        if not st:
            continue
        out.append((i, re.sub(r"\s+", " ", st)))
    return out


def rows_openbor(src):
    """OpenBOR states the counts literally."""
    m = re.search(r"rec_items\s*=\s*\(recmode\s*==\s*0\)\s*\?\s*(\d+)\s*:\s*"
                  r"\(recmode\s*==\s*2\)\s*\?\s*(\d+)\s*:\s*(\d+)", src)
    if not m:
        return None
    return {"idle": int(m.group(1)), "playing": int(m.group(2)),
            "recording": int(m.group(3))}


def rows_pico8(src):
    """Count add(entries, ...) per branch, PLUS the rows common to all modes.

    "Back" is added after the `end` that closes the branches, so a naive split
    reads 3/1/0 -- off by one everywhere, the signature of a shared row. Count
    the common tail rather than adding 1, so that moving Back INTO a branch
    (which WOULD be a desync) is still caught.
    """
    i = src.find("local m = stat(148)")
    if i < 0:
        return None
    j = src.find("cursor = __z8_menu.reccursor", i)
    if j < 0:
        return None
    seg = src[i:j]

    marks = []
    for pat in ("\n        if m == 1 then", "\n        elseif m == 2 then",
                "\n        else\n", "\n        end\n"):
        k = seg.find(pat)
        if k < 0:
            return None
        marks.append((k, len(pat)))

    rec = seg[marks[0][0] + marks[0][1]:marks[1][0]]
    play = seg[marks[1][0] + marks[1][1]:marks[2][0]]
    idle = seg[marks[2][0] + marks[2][1]:marks[3][0]]
    common = seg[marks[3][0] + marks[3][1]:]

    c = common.count("add(entries,")
    return {"recording": rec.count("add(entries,") + c,
            "playing": play.count("add(entries,") + c,
            "idle": idle.count("add(entries,") + c}


def main():
    name, cfg, path = detect()
    if not name:
        print("ERROR: no known pause-menu source under %s" % REPO)
        return 2

    src = open(path, encoding="utf-8", errors="replace").read()
    fail = 0
    print("core: %s   source: %s" % (name, cfg["file"]))

    # ---- A. row counts -----------------------------------------------------
    rows = rows_openbor(src) if name == "openbor" else rows_pico8(src)
    if rows is None:
        print("FAIL  A  could not read the per-mode row counts -- the menu was")
        print("         restructured, so this check is no longer measuring")
        print("         anything. Re-point it before trusting a pass.")
        fail += 1
    else:
        for mode in ("idle", "recording", "playing"):
            ok = rows[mode] == WANT_ROWS[mode]
            print("%s  A  rows[%-9s] = %d (want %d)"
                  % ("PASS" if ok else "FAIL", mode, rows[mode], WANT_ROWS[mode]))
            if not ok:
                fail += 1

    # ---- B. tripwire -------------------------------------------------------
    sites = conditional_sites(path, cfg)
    key = lambda t: t[1]
    found = sorted({key(s) for s in sites})

    if "--update-baseline" in sys.argv:
        with open(BASELINE, "w", encoding="utf-8", newline="\n") as f:
            f.write("# Reviewed mode-conditional sites in the pause menu.\n"
                    "# A NEW line here means: does it change what is DRAWN IN a\n"
                    "# row, or WHICH ROWS EXIST? Only the first is allowed.\n"
                    "# Regenerate with --update-baseline, and say in the commit\n"
                    "# message which answer each new site got.\n")
            for s in found:
                f.write(s + "\n")
        print("baseline written: %d site(s)" % len(found))
        return 0

    if not os.path.exists(BASELINE):
        print("FAIL  B  no baseline; run with --update-baseline and review it")
        return 1

    base = {l.rstrip("\n") for l in open(BASELINE, encoding="utf-8")
            if l.strip() and not l.startswith("#")}
    new = [s for s in found if s not in base]
    gone = [s for s in sorted(base) if s not in found]

    for s in new:
        ln = next(i for i, t in sites if t == s)
        print("FAIL  B  NEW mode-conditional at line %d:" % ln)
        print("           %s" % s[:100])
        print("         Does this change a ROW, or only a LABEL? A row is a")
        print("         replay desync. If it is a label, --update-baseline.")
        fail += 1
    for s in gone:
        print("note  B  site gone (fine, refresh the baseline): %s" % s[:80])

    print("PASS  B  %d reviewed site(s), no new ones" % len(base)
          if not new else "")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
