"""The writer must not emit takes its own reader refuses.

WHY THIS EXISTS
---------------
test_snap_payload.py cuts the READER out of the shipped source and drives it
with malformed input. That is the right shape for a security boundary -- but it
can only ever observe the reader, so a WRITER emitting something illegal has no
case that can see it. OpenBOR shipped exactly that blind spot twice in one day
(an unbounded strlen(name) against a reader refusing >= 512, and a .scr the
embed included and the reader rejected), and each produced a take that build
then refused permanently with nothing anywhere pointing at the cause.

PICO-8 had the same asymmetry when this was written: p8snap_read refused
c > 4096, nl > 512, dl > 8 MB and a running total > 8 MB, while p8snap_write
emitted nl/dl/c straight out of the vector with no bound at all. Not reachable
-- p8snap_from_dir filters to .p8d.txt AND to what the run touched -- but that
is a property of today's FILTER, not of the writer.

🛑 Both sides were fixed on the WRITER. Never widen a reader bound to accept
what the writer emits: takes are shared between strangers and the core runs as
root, so that turns a usability bug into an untrusted-input hole.

WHAT THIS CHECKS
----------------
Not a round trip -- the writer is threaded through a 100-line function and is
not separable the way the reader is. Instead it extracts each side's constraint
from the SHIPPED source and asserts they agree. Extracting rather than
restating is the point: a checker holding its own copy of the numbers grades a
reimplementation and passes while the shipped code drifts.

    python3 tools/harness/test_writer_reader_agree.py
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "..", "src", "mister_main.cpp")

fails = []


def check(ok, label, detail=""):
    print(("PASS  " if ok else "FAIL  ") + label + (("  -- " + detail) if detail else ""))
    if not ok:
        fails.append(label)


def body(name, src):
    """The text of one function, brace-matched from its signature."""
    m = re.search(r"^static [\w:<>, ]+?\b%s\s*\(" % re.escape(name), src, re.M)
    if not m:
        return None
    i = src.index("{", m.start())
    depth, j = 0, i
    while j < len(src):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
        j += 1
    return None


src = open(SRC, encoding="utf-8", errors="replace").read()
w = body("p8snap_write", src)
r = body("p8snap_read", src)
check(w is not None, "found p8snap_write in the shipped source",
      "a rename makes every check below vacuous")
check(r is not None, "found p8snap_read in the shipped source")
if w is None or r is None:
    sys.exit(1)


def nums(text, pat):
    """Every distinct numeric literal captured by pat, normalised to int."""
    out = set()
    for g in re.findall(pat, text):
        t = g if isinstance(g, str) else g[0]
        t = t.strip()
        if t.endswith("u"):
            t = t[:-1]
        if "<<" in t:
            a, b = t.split("<<")
            out.add(int(a.strip().rstrip("u"), 0) << int(b.strip().rstrip("u"), 0))
        else:
            out.add(int(t, 0))
    return out


# ---- 1. entry-count cap -----------------------------------------------------
rc = nums(r, r"c\s*>\s*(\d+u?)")
wc = nums(w, r"c\s*>\s*(\d+u?)")
check(rc and wc and rc == wc, "count cap agrees",
      "reader=%s writer=%s -- a payload the writer emits must be one the "
      "reader accepts" % (sorted(rc) or "none", sorted(wc) or "none"))

# ---- 2. per-entry NAME length ----------------------------------------------
rn = nums(r, r"nl\s*>\s*(\d+u?)")
wn = nums(w, r"nl\s*>\s*(\d+u?)")
check(rn and wn and rn == wn, "entry name cap agrees",
      "reader=%s writer=%s" % (sorted(rn) or "none", sorted(wn) or "none"))

# ---- 3. per-entry DATA length ----------------------------------------------
rd = nums(r, r"dl\s*>\s*\(?\s*(\d+u?\s*<<\s*\d+u?)\s*\)?")
wd = nums(w, r"dl\s*>\s*\(?\s*(\d+u?\s*<<\s*\d+u?)\s*\)?")
check(rd and wd and rd == wd, "entry data cap agrees",
      "reader=%s writer=%s" % (sorted(rd) or "none", sorted(wd) or "none"))

# ---- 4. AGGREGATE cap -- the one a per-entry check cannot cover -------------
# The reader refuses on a running total, so entries that are individually legal
# can still be rejected as a set. A writer bounding only per-entry would still
# emit an unreadable take.
rt = nums(r, r"total\s*>\s*\(?\s*(\d+u?\s*<<\s*\d+u?)\s*\)?")
wt = nums(w, r"total\s*>\s*\(?\s*(\d+u?\s*<<\s*\d+u?)\s*\)?")
check(bool(rt), "reader has an aggregate cap", "reader=%s" % (sorted(rt) or "none"))
check(rt and wt and rt == wt, "aggregate cap agrees",
      "reader=%s writer=%s" % (sorted(rt) or "none", sorted(wt) or "none"))

# ---- 5. the writer must REFUSE, not truncate or skip ------------------------
# Silently dropping an over-size entry would produce a take that reads fine and
# replays against the wrong saves -- a desync dressed as success, which is worse
# than refusing to write.
check(w.count("return false") >= 3, "writer refuses (returns false) on a bound",
      "found %d return-false sites" % w.count("return false"))
check("continue" not in w, "writer never SKIPS an entry",
      "a skipped entry is a silent partial payload")

# ---- 6. container version is written by the same constant the reader gates on
wcv = re.search(r"container\s*=\s*(P8REC_CONTAINER)", src)
rcv = re.findall(r"container\s*[<>]\s*(P8REC_CONTAINER)", src)
check(bool(wcv) and len(rcv) >= 2, "container version uses one shared constant",
      "writer=%s reader-gates=%d" % (bool(wcv), len(rcv)))

print()
if fails:
    print("WRITER/READER DISAGREE: %d" % len(fails))
    print("The writer can emit a take this same build refuses. Fix the WRITER.")
    sys.exit(1)
print("WRITER AND READER AGREE on every bound")
