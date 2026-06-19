#!/usr/bin/env python3
"""
pixel_window_classify.py -- resolution-INDEPENDENT FP filter (replaces the lossy montage).

For each cart we dump a WINDOW of frames (e.g. 52..68) on both engines (no input). Each
FBDUMP row is 128 hex chars = 128 4-bit pixel indices, so a frame = 16384 chars. We take
the anchor frame (middle) on each engine and find its MINIMUM full-pixel Hamming distance
to ANY frame in the OTHER engine's window (both directions). residual = max(dir1, dir2).

  - residual ~ 0  : the anchor is reproduced by the other engine a few frames away
                    => pure animation/phase difference => FALSE POSITIVE.
  - residual large: the anchor frame exists on one engine and NOWHERE near it on the other
                    => the engines genuinely render different pixels => REAL divergence.

No downsampling, no human eye in the filter -- every pixel counts. Output is RANKED by
residual (descending); eyeball the top at full res, the residual~0 tail is provably phase.

  python pixel_window_classify.py <z8_win_dir> <off_win_dir> [names.txt] [anchor=60]
"""
import sys, os, glob

TOTAL = 16384  # 128x128 pixels

def load(path):
    """frame -> 16384-char pixel string (rows concatenated in order)."""
    rows = {}
    for ln in open(path, encoding="utf-8", errors="replace"):
        ln = ln.strip()
        if not ln.startswith("FBDUMP"):
            continue
        p = ln.split(None, 3)
        if len(p) < 4:
            continue
        f = int(p[1]); r = int(p[2]); rows.setdefault(f, {})[r] = p[3]
    out = {}
    for f, rr in rows.items():
        if len(rr) == 128 and all(len(rr[i]) == 128 for i in range(128)):
            out[f] = "".join(rr[i] for i in range(128))
    return out

def ham(a, b):
    # full-resolution per-pixel difference count
    d = 0
    for i in range(TOTAL):
        if a[i] != b[i]:
            d += 1
    return d

def min_to_window(anchor_str, window):
    best = TOTAL
    for s in window:
        d = ham(anchor_str, s)
        if d < best:
            best = d
            if best == 0:
                break
    return best

def main():
    zd, od = sys.argv[1], sys.argv[2]
    names = {}
    if len(sys.argv) > 3 and os.path.exists(sys.argv[3]):
        for ln in open(sys.argv[3], encoding="utf-8"):
            if "|" in ln:
                k, v = ln.strip().split("|", 1); names[k] = v
    # MULTI-ANCHOR: a persistent render bug fails to match the other engine at EVERY
    # anchor; an animation matches at >=1 anchor (some frame is reproduced). residual =
    # the BEST (min) anchor's max-direction distance -> low only if SOME anchor is
    # reproduced both ways. Anchors spread across the dumped window.
    ids = sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(zd, "*.txt")))
    rows = []
    for cid in ids:
        z = load(os.path.join(zd, cid + ".txt"))
        o = load(os.path.join(od, cid + ".txt"))
        if not z or not o:
            rows.append((-1, cid, "NO-DUMP", names.get(cid, cid))); continue
        zframes = sorted(z); oframes = sorted(o)
        lo = max(min(zframes), min(oframes)); hi = min(max(zframes), max(oframes))
        if hi <= lo:
            rows.append((-1, cid, "NO-OVERLAP", names.get(cid, cid))); continue
        anchors = [lo + (hi-lo)*k//6 for k in range(1, 6)]   # 5 anchors across the overlap
        zvals = list(z.values()); ovals = list(o.values())
        best = TOTAL
        for af in anchors:
            za = z.get(af); oa = o.get(af)
            if za is None or oa is None: continue
            r = max(min_to_window(za, ovals), min_to_window(oa, zvals))
            if r < best: best = r
        rows.append((best, cid, f"{best} ({100*best/TOTAL:.1f}%)", names.get(cid, cid)))
    rows.sort(reverse=True)
    print(f"=== pixel-window residual (5-anchor min, full-res Hamming; ranked most-real first) ===")
    print(f"{'resid':>6} {'pct':>6}  id     cart")
    for residual, cid, label, nm in rows:
        if residual < 0:
            print(f"{'  n/a':>6} {'':>6}  {cid}  {nm}  [no usable window dump]")
        else:
            print(f"{residual:6d} {100*residual/TOTAL:5.1f}%  {cid}  {nm}")
    # band summary
    real = sum(1 for r, *_ in rows if r >= 0 and r/TOTAL >= 0.05)
    review = sum(1 for r, *_ in rows if r >= 0 and 0.01 <= r/TOTAL < 0.05)
    fp = sum(1 for r, *_ in rows if r >= 0 and r/TOTAL < 0.01)
    nd = sum(1 for r, *_ in rows if r < 0)
    print(f"\n=== REAL(>=5%):{real} | REVIEW(1-5%):{review} | PHASE-FP(<1%):{fp} | NO-DUMP:{nd} ===")
    print("Eyeball REAL + REVIEW at full res; PHASE-FP = anchor reproduced by other engine (provably phase).")

if __name__ == "__main__":
    main()
