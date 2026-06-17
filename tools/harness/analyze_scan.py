#!/usr/bin/env python3
# Classify scan_library.sh output into Crash / Hang / No-render / Black / Static
# / OK and print a ranked watch-list. Usage: analyze_scan.py scan_results.txt
import sys, collections
RES = sys.argv[1] if len(sys.argv) > 1 else "scan_results.txt"
rows = []
for line in open(RES, encoding="utf-8", errors="replace"):
    p = line.rstrip("\n").split("|", 4)
    if len(p) == 5:
        rows.append((int(p[0]), int(p[1]), p[2], p[3], p[4]))

static = [r for r in rows if r[1] == 0 and r[2] != "MISSING" and r[2] == r[3]]
fp = collections.Counter(r[2] for r in static).most_common(1)
black_fp = fp[0][0] if fp else None

crash  = [r for r in rows if r[1] not in (0, 124)]
hang   = [r for r in rows if r[1] == 124]
norend = [r for r in rows if r[1] == 0 and r[2] == "MISSING"]
black  = [r for r in static if r[2] == black_fp]
stat2  = [r for r in static if r[2] != black_fp]
ok     = [r for r in rows if r[1] == 0 and r[2] != "MISSING" and r[2] != r[3]]

print(f"=== SCAN SUMMARY ({len(rows)} carts) ===")
print(f"  OK (animating)      : {len(ok)}")
print(f"  STATIC (title/idle) : {len(stat2)}  (noisy without --hold input)")
print(f"  BLACK/blank         : {len(black)}  (noisy: includes carts waiting for input)")
print(f"  NO RENDER           : {len(norend)}")
print(f"  HANG (timeout)      : {len(hang)}   <- reliable")
print(f"  CRASH (non-zero ec) : {len(crash)}   <- reliable, highest value")

def dump(t, lst, lim=300):
    if not lst: return
    print(f"\n=== {t} ({len(lst)}) ===")
    for r in lst[:lim]: print(f"  ec={r[1]:>3}  {r[4]}")
    if len(lst) > lim: print(f"  ... +{len(lst)-lim} more")

dump("CRASH — engine faulted (HIGH PRIORITY)", crash)
dump("HANG — engine stuck (HIGH PRIORITY)", hang)
dump("NO RENDER", norend)
