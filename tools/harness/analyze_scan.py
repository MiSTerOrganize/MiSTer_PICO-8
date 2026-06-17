#!/usr/bin/env python3
# Classify scan_library.sh output into Crash / Hang / No-render / Black / Stuck /
# OK and print a ranked watch-list.
# Usage: analyze_scan.py scan_results.txt
# Format: idx|exitcode|md5_early|md5_mid|md5_late|relpath
import sys, collections
RES = sys.argv[1] if len(sys.argv) > 1 else "scan_results.txt"
rows = []
for line in open(RES, encoding="utf-8", errors="replace"):
    p = line.rstrip("\n").split("|", 5)
    if len(p) == 6:
        rows.append((int(p[0]), int(p[1]), p[2], p[3], p[4], p[5]))

# static = all three captured frames identical (frozen even with advance-input)
static = [r for r in rows if r[1] == 0 and r[2] != "MISSING" and r[2]==r[3]==r[4]]
fp = collections.Counter(r[2] for r in static).most_common(1)
black_fp = fp[0][0] if fp else None

crash  = [r for r in rows if r[1] not in (0, 124)]
hang   = [r for r in rows if r[1] == 124]
norend = [r for r in rows if r[1] == 0 and r[2] == "MISSING"]
black  = [r for r in static if r[2] == black_fp]
stuck  = [r for r in static if r[2] != black_fp]
ok     = [r for r in rows if r[1] == 0 and r[2] != "MISSING" and not (r[2]==r[3]==r[4])]

print(f"=== SCAN SUMMARY ({len(rows)} carts) ===")
print(f"  OK (animates)        : {len(ok)}")
print(f"  BLACK (all 3 blank)  : {len(black)}  <- blank even with advance-input")
print(f"  STUCK (frozen 1 screen): {len(stuck)}  <- one screen despite input")
print(f"  NO RENDER            : {len(norend)}")
print(f"  HANG (timeout)       : {len(hang)}   <- reliable")
print(f"  CRASH (non-zero ec)  : {len(crash)}  <- reliable, highest value")

def dump(t, lst, lim=150):
    if not lst: return
    print(f"\n=== {t} ({len(lst)}) ===")
    for r in lst[:lim]: print(f"  ec={r[1]:>3}  {r[5]}")
    if len(lst) > lim: print(f"  ... +{len(lst)-lim} more")

dump("CRASH (HIGH PRIORITY)", crash)
dump("HANG (HIGH PRIORITY)", hang)
dump("NO RENDER", norend)
dump("BLACK even with input", black, 80)
dump("STUCK on one screen despite input", stuck, 80)
