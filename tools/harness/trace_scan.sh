#!/bin/bash
# Full-corpus PICO-8 golden-trace scan (container entry point).
set -u
OUT=/work
mkdir -p "$OUT/goldens"
: > "$OUT/results.txt"
find /carts \( -iname '*.p8' -o -iname '*.p8.png' \) -print0 | sort -z > /tmp/cartlist
N=$(tr -cd '\0' < /tmp/cartlist | wc -c)
echo "scanning $N carts with $(nproc) workers"
xargs -0 -P "$(nproc)" -I{} bash /z8/trace_worker.sh {} < /tmp/cartlist
echo "SCAN DONE: $(wc -l < "$OUT/results.txt") carts"
echo "=== class histogram ==="
cut -d'|' -f1 "$OUT/results.txt" | sort | uniq -c | sort -rn
echo "=== non-DET carts (first 40) ==="
grep -v '^DET|' "$OUT/results.txt" | head -40
