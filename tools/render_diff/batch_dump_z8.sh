#!/bin/bash
# eyeball-pass step 2: z8headless FBDUMP over candidate dump wrappers -> /d/z8_dump/<id>.txt
set -u
WRAP=/d/dump_wrap; OUT=/d/z8_dump
cp /z/z8headless /tmp/ && cp /z/bios.p8 /tmp/ && chmod +x /tmp/z8headless && cd /tmp
mkdir -p "$OUT"
for f in "$WRAP"/*.p8; do
  id=$(basename "$f" .p8)
  [ -s "$OUT/$id.txt" ] && continue
  timeout 25 ./z8headless --cart "$f" --frames 200 --datadir /tmp --out /tmp 2>&1 \
    | grep "^FBDUMP" | sort -u > "$OUT/$id.txt"
done
echo "z8-dump complete: $(ls "$OUT"/*.txt 2>/dev/null | wc -l) files" >&2
