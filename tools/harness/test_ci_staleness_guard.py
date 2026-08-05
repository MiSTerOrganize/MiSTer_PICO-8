#!/usr/bin/env python3
"""Regression test for the CI commit-back staleness guard.

WHY THIS EXISTS
---------------
The commit-back loop in .github/workflows/build.yml re-applies THIS run's binary
onto whatever main's tip is, retrying on rejection. That is right about never
LOSING a build, but on its own it never asks whether the tip already carries a
NEWER binary.

Builds overlap whenever pushes land close together, and they do NOT finish in
start order. Measured 2026-08-04 on MiSTer_OpenBOR_7533:

    0ea791c build   00:46 -> 01:15
    d4815ef build   00:52 -> 01:07

The OLDER source finished LAST, re-applied its binary over the newer one, and
main shipped a stale (unstripped) binary until an unrelated build happened to
heal it. The guard closes that window.

WHAT IT TESTS
-------------
It cuts the REAL guard block out of the shipped build.yml and drives it with a
stubbed `gh`, rather than testing a reimplementation -- a copy would keep passing
while the shipped workflow rotted. Same principle as
feedback_parser_tests_cut_real_source.md.

The load-bearing property is asymmetric, so both directions are tested:
skipping a push it should have made loses a build SILENTLY, which is worse than
the clobber it prevents. So the guard may only skip when it is CERTAIN the tip
is newer, and must fail OPEN on every uncertainty.

USAGE
-----
    python tools/harness/test_ci_staleness_guard.py [build.yml ...]

With no argument it tests this repo's own .github/workflows/build.yml.
Exits non-zero on any failure. Leaves nothing behind.
"""
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

try:
    import yaml
except ImportError:
    sys.exit("PyYAML required:  pip install pyyaml")

SELF = "a" * 40    # this run's github.sha
NEWER = "b" * 40   # a source commit ahead of SELF
OLDER = "c" * 40   # a source commit behind SELF

# (description, tip binary commit message, compare status, expected verdict)
CASES = [
    ("tip AHEAD (newer build already on main)", f"CI: rebuild ARM binary for {NEWER}", "ahead",     "STALE"),
    ("tip BEHIND (we are newer)",               f"CI: rebuild ARM binary for {OLDER}", "behind",    "PROCEED"),
    ("tip IS this run",                         f"CI: rebuild ARM binary for {SELF}",  "identical", "PROCEED"),
    ("histories diverged",                      f"CI: rebuild ARM binary for {NEWER}", "diverged",  "PROCEED"),
    ("tip is a human commit (no marker)",       "handler: fix log rotation ordering",  "ahead",     "PROCEED"),
    ("short sha in marker (unparseable)",       "CI: rebuild ARM binary for bbbbbbb",  "ahead",     "PROCEED"),
    ("gh api errors",                           "__API_FAIL__",                        "ahead",     "PROCEED"),
    ("no commits touch the binary path",        "__EMPTY__",                           "ahead",     "PROCEED"),
]


def _bash():
    """Prefer git-bash on Windows: a bare `bash` there resolves to WSL's, which
    cannot see Windows paths and dies with execvpe(/bin/bash)."""
    if os.name == "nt":
        for c in (r"C:\Program Files\Git\usr\bin\bash.exe",
                  r"C:\Program Files\Git\bin\bash.exe"):
            if os.path.exists(c):
                return c
    return shutil.which("bash") or "bash"


def extract_guard(path):
    """Pull the guard out of the shipped workflow, verbatim."""
    doc = yaml.safe_load(io.open(path, encoding="utf-8"))
    steps = doc["jobs"]["build-arm"]["steps"]
    step = [s for s in steps if "Commit fresh ARM binary" in str(s.get("name", ""))]
    if not step:
        raise SystemExit(f"{path}: no 'Commit fresh ARM binary' step")
    body = step[0]["run"]
    if (step[0].get("env") or {}).get("GH_TOKEN") is None:
        raise SystemExit(f"{path}: step is missing env.GH_TOKEN -- `gh api` would fail at runtime")
    try:
        start = body.index("# --- Staleness guard")
        end = body.index("cp /tmp/ci-binary/", start)
    except ValueError:
        raise SystemExit(f"{path}: staleness guard block not found")
    guard = body[start:end]
    for token in ("gh api", "STALE=1", "compare/"):
        if token not in guard:
            raise SystemExit(f"{path}: guard missing {token!r}")
    # The guard is useless if the loop's exit condition still treats a skip as a
    # failure, so assert the wiring too.
    if '"$STALE" != "1"' not in body:
        raise SystemExit(f"{path}: STALE is set but never consulted by the final condition")
    return guard


def run_case(guard, tip_msg, cmp_status, workdir):
    g = (guard.replace("${{ github.repository }}", "TEST/REPO")
              .replace("${{ github.sha }}", SELF))
    stub = f'''#!/bin/bash
case "$2" in
  *commits*)
    case "{tip_msg}" in
      __API_FAIL__) exit 1 ;;
      __EMPTY__)    echo "null" ; exit 0 ;;
      *)            echo "{tip_msg}" ; exit 0 ;;
    esac ;;
  *compare*) echo "{cmp_status}" ;;
esac
'''
    gh = os.path.join(workdir, "gh")
    io.open(gh, "w", newline="\n").write(stub)
    os.chmod(gh, 0o755)
    script = "STALE=0\nfor i in 1; do\n" + g + "\n:\ndone\necho VERDICT=$STALE\n"
    runner = os.path.join(workdir, "run.sh")
    io.open(runner, "w", newline="\n").write(script)
    r = subprocess.run([_bash(), runner], capture_output=True, text=True,
                       env={**os.environ, "PATH": workdir + os.pathsep + os.environ["PATH"]})
    out = r.stdout + r.stderr
    m = re.search(r"VERDICT=(\d)", out)
    if not m:
        return "ERROR", out
    return ("STALE" if m.group(1) == "1" else "PROCEED"), out


def main():
    targets = sys.argv[1:]
    if not targets:
        here = os.path.dirname(os.path.abspath(__file__))
        targets = [os.path.join(here, "..", "..", ".github", "workflows", "build.yml")]

    fails = 0
    workdir = tempfile.mkdtemp(prefix="staleguard-")
    try:
        for path in targets:
            path = os.path.normpath(path)
            guard = extract_guard(path)
            print(f"\n=== {path} ({len(guard)} bytes of real guard text) ===")
            for desc, msg, status, want in CASES:
                got, out = run_case(guard, msg, status, workdir)
                ok = got == want
                fails += (not ok)
                print(f"  [{'PASS' if ok else 'FAIL'}] {desc:<42} want={want:<7} got={got}")
                if not ok:
                    for line in out.strip().splitlines():
                        print("        " + line)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print(f"\n{'ALL PASS' if fails == 0 else str(fails) + ' FAILURES'}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
