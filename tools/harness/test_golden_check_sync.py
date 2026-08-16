#!/usr/bin/env python3
"""Assert golden_check.sh reproduces trace_worker.sh's run conditions EXACTLY.

A golden is only comparable to a run made the same way. The two workers hold
that invocation separately -- trace_worker.sh PRODUCES goldens, golden_check.sh
COMPARES against them -- so a condition changed in one and not the other makes
every cart drift at once, and the gate would blame the engine.

The failure this prevents is not hypothetical: the same-length trace-path rule
was learned when writing run A to a long path and run B to a short one made
layout-sensitive carts (Snak, CluePix Halloween) compare two different
deterministic modes -- a permanent false NONDET. If golden_check.sh ever stops
using the "/tmp/ta.XXXXXX" shape, those carts false-DIFF on every run.

Standard library only. Exit 0 = in sync, 1 = drifted, 2 = could not check.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PRODUCER = os.path.join(HERE, "trace_worker.sh")
CHECKER = os.path.join(HERE, "golden_check.sh")

# Each condition: a label and the regex that must appear in BOTH files.
CONDITIONS = [
    ("fixed-seed test mode",   r"--test"),
    ("120 boot frames",        r"--frames 120"),
    ("datadir at the bios",    r"--datadir /z8/"),
    ("ASLR off",               r'setarch "\$\(uname -m\)" -R'),
    ("20s timeout",            r"timeout 20"),
    ("isolated cartdata",      r'Z8_SAVES_DIR="\$h/saves"'),
    ("fresh HOME",             r'HOME="\$h"'),
    ("cwd = the cart's dir",   r'cd "\$\(dirname "\$cart"\)"'),
    ("same-length trace path", r'mktemp /tmp/ta\.XXXXXX'),
]


def read(p):
    if not os.path.isfile(p):
        print("could not check: missing %s" % p)
        sys.exit(2)
    with open(p, encoding="utf-8") as f:
        return f.read()


def main():
    prod, chk = read(PRODUCER), read(CHECKER)
    bad = 0
    print("comparing run conditions: trace_worker.sh (produces) vs golden_check.sh (checks)")
    for label, rx in CONDITIONS:
        in_p = re.search(rx, prod) is not None
        in_c = re.search(rx, chk) is not None
        if in_p and in_c:
            print("  OK    %s" % label)
        else:
            where = []
            if not in_p:
                where.append("trace_worker.sh")
            if not in_c:
                where.append("golden_check.sh")
            print("  DRIFT %-24s missing from: %s" % (label, ", ".join(where)))
            bad += 1

    # The checker must compare against a stored golden; the producer must not.
    if "/goldens/" not in chk:
        print("  DRIFT golden_check.sh never reads /goldens -- it is not comparing to anything")
        bad += 1

    print("\n%s" % ("IN SYNC" if bad == 0 else "DRIFTED: %d condition(s)" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
