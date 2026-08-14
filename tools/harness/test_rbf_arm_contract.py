"""The RBF and the ARM binary must ship together. Make that mechanical.

WHY THIS EXISTS
---------------
"The new RBF and the new ARM binary MUST ship together" is a rule this project
states in several places and enforces nowhere. CI builds and commits ONLY the
ARM half; nothing in build.yml references version.txt, _Other/, or the RTL. So a
source push that changes the DDR3 wire contract auto-ships a new ARM binary
against whatever RBF happens to be sitting in _Other/.

That is not hypothetical here. The replay-slot picker adds a NEW DDR3 word: the
ARM polls 0x0C for the OSD's slot. Under the OLD bitstream that offset is
untouched dead space holding whatever the previous core left in DDR3, so the
poll reads garbage and overwrites the pause-menu slot roughly once a second.
The feature looks broken for a reason that has nothing to do with the feature.

WHAT IS CHECKABLE, AND WHAT IS NOT
----------------------------------
An .rbf is a compiled bitstream; nothing in it can be compared to a source
constant. So this does NOT prove the shipped RBF implements the contract. What
it proves is the useful negative:

  A. version.txt names an RBF that actually exists in _Other/, and only one
     RBF is present -- a second, older one ships to users as well.
  B. The committed RBF is not OLDER than the newest commit touching the RTL.
     If the wire-contract RTL moved after the bitstream was committed, the
     bitstream cannot contain it, whatever anyone intended.

(B) is the load-bearing half and it is a git-history question, so it works
without Quartus and without a device.

🛑 It CANNOT catch: an RTL change committed together with a stale RBF in one
commit, or a Quartus compile that silently produced an old bitstream. Those need
the hardware check. This closes the accident, not the deliberate mistake.

    python3 test_rbf_arm_contract.py
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

fails = []


def check(ok, label, detail=""):
    print(("PASS  " if ok else "FAIL  ") + label + (("  -- " + detail) if detail else ""))
    if not ok:
        fails.append(label)


def git(*args):
    r = subprocess.run(["git", "-C", REPO] + list(args), capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


# ---- A. version.txt names exactly one RBF, and it is there -----------------
vt = os.path.join(REPO, "version.txt")
check(os.path.exists(vt), "version.txt exists")
named = open(vt, encoding="utf-8").read().strip() if os.path.exists(vt) else ""

other = os.path.join(REPO, "_Other")
rbfs = sorted(f for f in os.listdir(other) if f.endswith(".rbf")) if os.path.isdir(other) else []
check(len(rbfs) == 1,
      "exactly ONE rbf in _Other/",
      "found %s -- a second one ships to users alongside the intended build" % (rbfs or "none"))
check(named in rbfs, "version.txt names an rbf that exists",
      "version.txt=%r present=%s" % (named, rbfs))

# ---- B. the RBF is not older than the RTL it must implement ---------------
# 🛑 REFUSE ON A SHALLOW CLONE. This half is git-history-based, and
# actions/checkout@v4 defaults to depth 1, where every tracked file reports the
# shallow-boundary commit as its last change. The rbf and the RTL then tie on
# %ct, `rbf_when >= newest` is trivially true, and this printed a green PASS
# while being structurally unable to evaluate -- a check that confirms itself.
# Measured on one tip commit: FULL clone FAIL/exit 1, depth-1 clone PASS/exit 0.
# The workflow also sets fetch-depth: 0; this guard is what stops the check
# silently degrading again if that checkout is ever changed back.
shallow = git("rev-parse", "--is-shallow-repository")
check(shallow != "true", "the clone has real history (not shallow)",
      "a depth-1 clone makes every file's last-commit time identical, so the "
      "comparison below cannot fail -- set fetch-depth: 0 on the checkout")

# Tracked RTL, excluding the vendored framework (sys/) we do not author.
rtl = [f for f in git("ls-files", "fpga").split("\n")
       if f.endswith((".sv", ".v", ".qip")) and "/sys/" not in f and not f.startswith("fpga/sys/")]
check(bool(rtl), "found tracked RTL to compare against",
      "an empty list would make the comparison below vacuously pass")

if rbfs and rtl and shallow != "true":
    rbf_path = "_Other/" + (named if named in rbfs else rbfs[0])
    rbf_when = git("log", "-1", "--format=%ct", "--", rbf_path)
    newest, newest_f = 0, ""
    for f in rtl:
        t = git("log", "-1", "--format=%ct", "--", f)
        if t and int(t) > newest:
            newest, newest_f = int(t), f
    check(bool(rbf_when), "the rbf is committed (has git history)",
          "an untracked rbf cannot be compared and must not pass silently")
    if rbf_when and newest:
        ok = int(rbf_when) >= newest
        check(ok, "the committed rbf is NOT older than the newest RTL commit",
              "" if ok else
              "%s moved after %s was committed -- that bitstream cannot contain it"
              % (newest_f, rbf_path))

print()
if fails:
    print("RBF/ARM CONTRACT: %d problem(s)" % len(fails))
    print("A source push would ship an ARM binary against an RBF that does not match it.")
    sys.exit(1)
print("RBF/ARM CONTRACT OK (git-history check; hardware still proves the contract itself)")
