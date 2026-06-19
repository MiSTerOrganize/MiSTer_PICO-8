#!/usr/bin/env python3
"""
seq_analyze.py -- Tier-2 trustworthiness classifier for the render-diff.

The Tier-1 fixed-checkpoint pass (compare_render.py) over-flags: title color-cycles
and idle sprite animations make the two engines momentarily out-of-phase at a sampled
checkpoint -> RENDER-DIVERGE false positive. Tier-2 re-runs each candidate in SEQ mode
(gen_wrapper.py --seq N: hash 0x6000 EVERY frame 1..N, NO input) on both engines and
classifies by TWO phase-robust signals:

  exact = same-frame exact-hash match rate   (catches in-phase animation: most frames
          agree, only transition frames differ -- e.g. Ape Clerk 71%)
  jacc  = Jaccard overlap of the two frame-hash SETS  (catches constant phase-shift:
          identical frames, shifted in time -- e.g. Alone in Pico jacc=0.99)

A real PERSISTENT render bug scores LOW on BOTH (every frame differs AND never appears
in the other engine's sequence -- e.g. a wrong palette/sprite drawn every frame).

  PACING-FP        exact >= 0.5  OR  jacc >= 0.5      (animation/phase, NOT a bug)
  REAL-CANDIDATE   exact < 0.5  AND  jacc < 0.2       (genuine persistent divergence)
  REVIEW-IMAGE     otherwise                          (eyeball the triptych)

Validated 2026-06-18: Jumping Jack (MATCH) exact=1.00/jacc=1.00; Ape Clerk exact=0.71;
Alone in Pico jacc=0.99 -- all three correctly NOT real bugs.

LIMITATION (state in report): no-input SEQ exercises the title/intro/attract only. A
render bug that needs gameplay input to reach (Virtua Racing track class) renders fine
at the title -> classified clean here. Those need per-cart deterministic input.

Usage: seq_analyze.py <z8_seq_dir> <off_seq_dir> [names.txt]
  each dir holds <id>.txt files of 'FBHASH f<N>=<hash>' lines (one per cart).
"""
import sys, os, re, glob

def load(path):
    d = {}
    for ln in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"FBHASH f(\d+)=(.*)", ln.strip())
        if m:
            d[int(m.group(1))] = m.group(2)
    return d

def classify(z, o):
    if not z or not o:
        return "NO-SEQ", 0.0, 0.0
    N = min(max(z), max(o))
    if N < 8:
        return "NO-SEQ", 0.0, 0.0
    exact = sum(1 for i in range(1, N + 1) if i in z and i in o and z[i] == o[i]) / N
    zs, os_ = set(z.values()), set(o.values())
    jacc = len(zs & os_) / len(zs | os_)
    if exact >= 0.5 or jacc >= 0.5:
        v = "PACING-FP"
    elif exact < 0.5 and jacc < 0.2:
        v = "REAL-CANDIDATE"
    else:
        v = "REVIEW-IMAGE"
    return v, exact, jacc

def main():
    zdir, odir = sys.argv[1], sys.argv[2]
    names = {}
    if len(sys.argv) > 3:
        for ln in open(sys.argv[3], encoding="utf-8", errors="replace"):
            if "|" in ln:
                k, v = ln.strip().split("|", 1); names[k] = v
    ids = sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(zdir, "*.txt")))
    counts = {}
    rows = []
    for cid in ids:
        zp = os.path.join(zdir, cid + ".txt"); op = os.path.join(odir, cid + ".txt")
        z = load(zp) if os.path.exists(zp) else {}
        o = load(op) if os.path.exists(op) else {}
        v, ex, ja = classify(z, o)
        counts[v] = counts.get(v, 0) + 1
        rows.append((v, cid, ex, ja, names.get(cid, cid)))
    order = {"REAL-CANDIDATE": 0, "REVIEW-IMAGE": 1, "PACING-FP": 2, "NO-SEQ": 3}
    rows.sort(key=lambda r: (order.get(r[0], 9), r[2]))
    for v, cid, ex, ja, nm in rows:
        print(f"{v:14s} {cid}  exact={ex:.2f} jacc={ja:.2f}  {nm}")
    print("\n=== " + " | ".join(f"{k}:{counts[k]}" for k in sorted(counts)) + " ===")
    print("REAL-CANDIDATE = trustworthy render-bug candidates -> eyeball triptych (fbdiff.py).")

if __name__ == "__main__":
    main()
