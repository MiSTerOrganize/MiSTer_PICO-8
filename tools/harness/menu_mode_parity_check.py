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

THE ONE SANCTIONED EXCEPTION (2026-08-10). A site MAY change which rows exist
if the differing shape is UNREACHABLE INSIDE A TAKE. Exactly one shape
qualifies: gated on mode NON-ZERO. Frames are only ever captured while
recording (mode 1) and replayed at mode 2, so both sides of a replay see the
same menu; idle (0) is never inside a take. Quit's confirm is that case -- it
asks only when there is a recording or replay to lose, because Reset Pak does
not ask either and a second press to leave a finished game is friction for no
gain. This trades safety-by-construction for safety-by-argument, so the
argument is enforced by check C rather than left in a comment.

THREE CHECKS, because none alone is enough:

  A. ROW COUNTS. The per-mode row counts must match the cross-core invariant
     (idle 4 / recording 2 / playing 1). Direct, exact, and cheap -- but blind
     to anything outside the Recording submenu, which is where the bug was.

  B. TRIPWIRE. Every mode-conditional site in the pause-menu source must appear
     in the reviewed baseline beside this script. A NEW one fails until a human
     answers: does this change what is DRAWN IN a row, or WHICH ROWS EXIST?
     This is the check that would have caught the quit confirm.

  C. QUIT-CONFIRM GATE. The exception above is only sound while the gate is a
     non-zero test. `== 1` is the ORIGINAL SHIPPED BUG and `== 2` is its mirror;
     either distinguishes recording from playback and desyncs a replay. B would
     not catch the regression, because the site is already in the baseline -- so
     C reads the gate itself and fails on any equality against a literal mode.

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
        # The line that RAISES the confirm. The gate is the nearest
        # mode-mentioning line at or above it.
        "confirm_anchor": "in_quitconfirm = 1",
        "confirm_sym": "mrec_mode",
    },
    "pico8": {
        "file": "src/pico8/bios.p8",
        # 🛑 The GLOBAL alone is not the symbol set. OpenBOR says so above --
        # grepping only mrec_mode "would miss every branch that matters and
        # give a check that passes vacuously" -- and this core had precisely
        # that miss: bios.p8 does `local m = stat(148)` and then branches on
        # `m`, so check B reviewed 3 sites here against OpenBOR's 12.
        #
        # `m` cannot be listed literally (one letter matches everything), so
        # alias_of names the assignment to resolve; the local is then matched
        # on word boundaries only.
        "symbols": ["stat(148)"],
        "alias_of": r"local\s+(\w+)\s*=\s*stat\(148\)",
        "ignore": [],
        "comment": ("--",),
        "confirm_anchor": "cur.askrec",
        "confirm_sym": "stat(148)",
    },
}


def detect():
    for name, cfg in CORES.items():
        p = os.path.join(REPO, cfg["file"])
        if os.path.exists(p):
            return name, cfg, p
    return None, None, None


def conditional_sites(path, cfg):
    """Every non-comment line mentioning a mode symbol, normalized.

    Resolves a LOCAL ALIAS first (cfg["alias_of"]). A core that copies the
    mode into a local and then branches on the local is invisible to a
    global-only symbol list -- which is not hypothetical: PICO-8 does
    `local m = stat(148)` and reviewed 3 sites where OpenBOR reviewed 12.
    """
    src = open(path, encoding="utf-8", errors="replace").read()
    syms = list(cfg["symbols"])
    alias_re = cfg.get("alias_of")
    aliases = sorted(set(re.findall(alias_re, src))) if alias_re else []
    out = []
    for i, raw in enumerate(src.splitlines(), 1):
        st = raw.strip()
        if not st or st.startswith(cfg["comment"]):
            continue
        hit = any(sym in st for sym in syms)
        if not hit and aliases:
            # word boundaries: a one-letter local would otherwise match
            # every line in the file
            hit = any(re.search(r"\b" + re.escape(a) + r"\b", st)
                      for a in aliases)
        if not hit:
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

    # 🛑 This count is TEXT, not structure, and that is a known limit worth
    # stating rather than hiding. It cannot see a row added by any other
    # form, so refuse outright if one appears instead of silently
    # undercounting -- an undercount reads as "shape unchanged".
    for _alt in ("entries[#entries", "insert(entries", "addall(entries"):
        if _alt in seg:
            raise SystemExit(
                "menu_mode_parity_check: rows are added by %r as well as\n"
                "  add(entries, ...), so the per-branch count is not the row\n"
                "  count and check A is measuring the wrong thing. Teach this\n"
                "  function the new form before trusting the result." % _alt)
    # A row wrapped in a mode conditional keeps this count while changing the
    # SHAPE -- that is F8, and it is caught by check B, which now sees the
    # local alias (F7) and flags the new conditional as an unreviewed site.
    c = common.count("add(entries,")
    return {"recording": rec.count("add(entries,") + c,
            "playing": play.count("add(entries,") + c,
            "idle": idle.count("add(entries,") + c}


def quit_gate(path, cfg):
    """The condition guarding Quit's confirm, or None if it cannot be found.

    Returns None rather than guessing: an anchor that has moved means this
    check is measuring nothing, and a check that cannot fail is not a check.
    """
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    anchor = None
    for i, raw in enumerate(lines):
        st = raw.strip()
        if st.startswith(cfg["comment"]):
            continue
        if cfg["confirm_anchor"] in st:
            anchor = i
            break
    if anchor is None:
        return None
    # nearest mode-mentioning non-comment line at or above the anchor
    for i in range(anchor, max(-1, anchor - 12), -1):
        st = lines[i].strip()
        if st.startswith(cfg["comment"]):
            continue
        if cfg["confirm_sym"] in st:
            return re.sub(r"\s+", " ", re.split(r"/\*|//|--\s", st)[0].strip())
    return None


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
                    "# row, or WHICH ROWS EXIST? Only the first is allowed --\n"
                    "# EXCEPT a gate on mode NON-ZERO, whose differing shape is\n"
                    "# unreachable inside a take (captured at 1, replayed at 2).\n"
                    "# That exception is enforced by check C, not by trust.\n"
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

    if not new:
        print("PASS  B  %d reviewed site(s), no new ones" % len(base))

    # ---- C. quit-confirm gate ---------------------------------------------
    gate = quit_gate(path, cfg)
    if gate is None:
        print("FAIL  C  could not find the quit-confirm gate (anchor %r moved)"
              % cfg["confirm_anchor"])
        print("         Re-point this before trusting a pass -- as written it")
        print("         is measuring nothing.")
        fail += 1
    else:
        # 🛑 EVALUATE the predicate; do not pattern-match its spelling. The
        # denylist here knew ==, != and ~= only, so `mrec_mode > 1` sailed
        # through -- playback-only, the exact MIRROR of the defect that
        # shipped. So did >= 2, < 2 and <= 1.
        #
        # The real rule is one sentence: the gate must not be able to tell
        # mode 1 from mode 2. A take is CAPTURED at 1 and REPLAYED at 2, so
        # any predicate that separates them shows a different menu on each
        # side of a replay, and injected navigation lands on a different row.
        # Comparing against 0 is fine and is the sanctioned form.
        sym = re.escape(cfg["confirm_sym"])
        bad = None
        for _m in re.finditer(sym + r"\s*(==|!=|~=|>=|<=|>|<)\s*(-?\d+)", gate):
            _op, _n = _m.group(1), int(_m.group(2))
            _f = {"==": lambda v: v == _n, "!=": lambda v: v != _n,
                  "~=": lambda v: v != _n, ">=": lambda v: v >= _n,
                  "<=": lambda v: v <= _n, ">": lambda v: v > _n,
                  "<": lambda v: v < _n}[_op]
            if _f(1) != _f(2):        # separates recording from playback
                bad = _m
                break
        if bad:
            print("FAIL  C  quit confirm is gated on a SPECIFIC mode: %s"
                  % bad.group(0))
            print("           %s" % gate[:100])
            print("         Recording is mode 1 and playback is mode 2. A take")
            print("         is captured at 1 and replayed at 2, so a gate that")
            print("         tells them apart makes the confirm appear on one")
            print("         side and not the other -- the recorded Back lands")
            print("         on Quit and the replay exits to a black screen.")
            print("         Gate on NON-ZERO instead.")
            fail += 1
        else:
            print("PASS  C  quit confirm gated on non-zero mode: %s" % gate[:60])

    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
