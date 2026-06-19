#!/usr/bin/env python3
"""
rank_triptychs.py -- eyeball-pass step 4: build + RANK triptychs for candidates.

For each id with both z8_dump/<id>.txt and off_dump/<id>.txt, runs fbdiff.py to write
z8/official/diff PNGs into rd_run/triptych/ and parses its metrics. Ranks candidates
MOST-BUG-LIKE FIRST: a real render bug = large contiguous diff with LOW bg_swap_ratio
(real colour swapped for real colour); animation/particle FP = high bg_swap + scatter.
Rank score = largest_cc * (1 - bg_swap)   (high cc + low bg = top). Eyeball top-down;
stop when you hit the obvious animation-FP tail.

  python rank_triptychs.py   (reads rd_run/z8_dump + rd_run/off_dump + names.txt)
"""
import os, re, glob, subprocess, sys
WORK = "C:/Users/miste/AppData/Local/Temp/rd_run"
ZD = os.path.join(WORK, "z8_dump"); OD = os.path.join(WORK, "off_dump")
TRI = os.path.join(WORK, "triptych")
HERE = os.path.dirname(os.path.abspath(__file__))
FBDIFF = os.path.join(HERE, "fbdiff.py")

def main():
    os.makedirs(TRI, exist_ok=True)
    names = {}
    nf = os.path.join(WORK, "names.txt")
    if os.path.exists(nf):
        for ln in open(nf, encoding="utf-8"):
            if "|" in ln:
                k, v = ln.strip().split("|", 1); names[k] = v
    ids = sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(ZD, "*.txt")))
    rows = []
    for cid in ids:
        zp = os.path.join(ZD, cid + ".txt"); op = os.path.join(OD, cid + ".txt")
        if not (os.path.exists(zp) and os.path.exists(op) and os.path.getsize(zp) and os.path.getsize(op)):
            continue
        r = subprocess.run([sys.executable, FBDIFF, zp, op,
                            os.path.join(TRI, cid + "_z8.png"),
                            os.path.join(TRI, cid + "_off.png"),
                            os.path.join(TRI, cid + "_diff.png")],
                           capture_output=True, text=True)
        out = r.stdout
        if "total differing pixels: 0" in out:
            continue
        m_t = re.search(r"total differing pixels: (\d+)", out)
        m_c = re.search(r"largest_cc=(\d+)", out)
        m_b = re.search(r"bg_swap_ratio=([0-9.]+)", out)
        total = int(m_t.group(1)) if m_t else 0
        cc = int(m_c.group(1)) if m_c else 0
        bg = float(m_b.group(1)) if m_b else 1.0
        score = cc * (1.0 - bg)
        rows.append((score, total, cc, bg, cid, names.get(cid, cid)))
    rows.sort(reverse=True)
    print(f"=== {len(rows)} candidates with a nonzero no-input diff (ranked most-bug-like first) ===")
    print(f"{'score':>7} {'total':>6} {'cc':>5} {'bg':>5}  id     cart")
    for score, total, cc, bg, cid, nm in rows:
        print(f"{score:7.1f} {total:6d} {cc:5d} {bg:5.2f}  {cid}  {nm}")
    print(f"\ntriptych PNGs in: {TRI}  (read <id>_diff.png; top of list = most likely real)")

if __name__ == "__main__":
    main()
