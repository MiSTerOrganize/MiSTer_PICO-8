#!/bin/bash
# z8 window dumps -> /d/play_z8/<id>.txt. --frames 600 so flip-count reaches frame 90
# (carts flip < once per engine-frame). Resumable.
set -u
WRAP=/d/play_wrap; OUT=/d/play_z8
cp /z/z8headless /tmp/ && cp /z/bios.p8 /tmp/ && chmod +x /tmp/z8headless && cd /tmp
mkdir -p "$OUT"
n=0
for f in "$WRAP"/*.p8; do
  id=$(basename "$f" .p8)
  [ -s "$OUT/$id.txt" ] && continue
  timeout 40 ./z8headless --cart "$f" --frames 600 --datadir /tmp --out /tmp 2>&1 \
    | grep "^FBDUMP" | sort -u > "$OUT/$id.txt"
  n=$((n+1)); [ $((n%200)) -eq 0 ] && echo "z8-play $n" >&2
done
echo "z8-play complete: $(ls "$OUT"/*.txt 2>/dev/null | wc -l)" >&2
