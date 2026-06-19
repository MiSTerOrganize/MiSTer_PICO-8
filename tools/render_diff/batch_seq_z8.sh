#!/bin/bash
# Tier-2 step 2: z8headless over candidate SEQ wrappers -> per-id /d/z8_seq/<id>.txt.
# Run in ubuntu container (/z=z8headless+bios ro, /d=rd_run rw). Resumable (skips done).
set -u
WRAP=/d/cand_wrap; OUT=/d/z8_seq
cp /z/z8headless /tmp/ && cp /z/bios.p8 /tmp/ && chmod +x /tmp/z8headless && cd /tmp
mkdir -p "$OUT"
n=0
for f in "$WRAP"/*.p8; do
  id=$(basename "$f" .p8)
  [ -s "$OUT/$id.txt" ] && continue
  timeout 25 ./z8headless --cart "$f" --frames 300 --datadir /tmp --out /tmp 2>&1 \
    | grep "^FBHASH" | sort -u > "$OUT/$id.txt"
  n=$((n+1)); [ $((n%100)) -eq 0 ] && echo "z8-seq $n done" >&2
done
echo "z8-seq complete: $(ls "$OUT"/*.txt 2>/dev/null | wc -l) files" >&2
