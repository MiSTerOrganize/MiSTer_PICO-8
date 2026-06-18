#!/usr/bin/env python3
# Join the shrinko8 (ground-truth) and z8headless (ours) decode scans and flag
# carts where OUR decode diverges = a bug in our cart loader/decompressor.
#   compare_decode.py shrinko_codes.txt ours_codes.txt
# shrinko line: i|status|len|md5|relpath     ours line: i|status|len|relpath
import sys
sf, of = sys.argv[1], sys.argv[2]

shr = {}
for ln in open(sf, encoding="utf-8", errors="replace"):
    p = ln.rstrip("\n").split("|", 4)
    if len(p) == 5:
        shr[p[4]] = (p[1], int(p[2]) if p[2].isdigit() else 0)
ours = {}
for ln in open(of, encoding="utf-8", errors="replace"):
    p = ln.rstrip("\n").split("|", 3)
    if len(p) == 4:
        ours[p[3]] = (p[1], int(p[2]) if p[2].isdigit() else 0)

# The differential is only meaningful for carts that actually go through
# DEcompression: .p8.png / ROM. Plain .p8 *text* carts have no compression
# (so no decode bug is possible), AND they may use #include directives that
# shrinko expands at read time while our --dumpcode emits the raw pre-#include
# source (the engine expands #includes later via preprocess_code at run time).
# So .p8 text divergences are preprocessor/tooling artifacts, not decode bugs.
# Split by extension and only treat .p8.png/ROM divergences as real candidates.
def is_png(rel): return rel.lower().endswith(".p8.png")

png_fail, png_len = [], []   # REAL decode-bug candidates (.p8.png)
txt_diverge = []             # .p8 text artifacts (#include / preprocessor) — informational
shr_only_fail, missing, agree_empty = [], [], 0

for rel, (ss, sl) in sorted(shr.items()):
    if rel not in ours:
        missing.append(rel); continue
    os_, ol = ours[rel]
    sok = (ss == "OK"); ook = (os_ == "OK")
    # Both empty (data-only sub-cart, no code section) = agreement, not a bug.
    if sl == 0 and ol == 0:
        agree_empty += 1; continue
    if (not sok) and ook:
        shr_only_fail.append((rel, ss)); continue
    diverged = False; detail = None
    if sok and not ook:
        diverged = True; detail = ("FAIL", os_, sl)
    elif sok and ook:
        tol = max(8, int(0.02 * max(sl, ol)))
        if abs(sl - ol) > tol:
            diverged = True; detail = ("LEN", ol, sl)
    if not diverged: continue
    if is_png(rel):
        (png_fail if detail[0] == "FAIL" else png_len).append((rel,) + detail[1:])
    else:
        txt_diverge.append((rel, detail[0]) + detail[1:])

print(f"=== DECODE DIFFERENTIAL ({len(shr)} shrinko / {len(ours)} ours) ===")
print(f"  .p8.png DECODE divergence (REAL bug signal): {len(png_fail)+len(png_len)}")
print(f"      - ours empty/failed, shrinko OK : {len(png_fail)}")
print(f"      - length divergence             : {len(png_len)}")
print(f"  .p8 text divergence (#include/preproc artifact, NOT decode): {len(txt_diverge)}")
print(f"  shrinko-side fail (not ours)        : {len(shr_only_fail)}")
print(f"  both-empty data carts (agree)       : {agree_empty}")
print(f"  missing from ours                   : {len(missing)}")

def dump(t, rows, fmt):
    if not rows: return
    print(f"\n=== {t} ({len(rows)}) ===")
    for r in rows[:120]: print("  " + fmt(r))
    if len(rows) > 120: print(f"  ... +{len(rows)-120} more")

dump("*** .p8.png DECODE FAIL (shrinko OK) — REAL BUG ***", png_fail,
     lambda r: f"ours={r[1]:<7} shrinko_len={r[2]:<7} {r[0]}")
dump("*** .p8.png LENGTH DIVERGENCE — REAL BUG ***", png_len,
     lambda r: f"ours_len={r[1]:<7} shrinko_len={r[2]:<7} delta={r[1]-r[2]:<+7} {r[0]}")
dump(".p8 text divergence (#include expansion — not a decode bug)", txt_diverge,
     lambda r: f"{r[1]} ours={r[2]} shrinko={r[3]} {r[0]}")
dump("shrinko-side failures (not our bug)", shr_only_fail,
     lambda r: f"shrinko={r[1]} {r[0]}")
