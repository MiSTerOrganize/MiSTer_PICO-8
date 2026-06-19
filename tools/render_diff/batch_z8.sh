#!/bin/bash
# Phase B -- z8headless (zepto8, what we ship) over every wrapper. Runs INSIDE the
# ubuntu:24.04 container: /z = z8headless+bios.p8 (ro), /d = the rd_run work dir.
# Resumable via /d/z8_done.txt. --frames bounds z8 (self-stops at 2000 engine frames);
# timeout is just a hang safety net. Output format = compare_render.py's parser:
#   ## <id>
#   FBHASH f1=...   (etc.)
set -u
WRAP=/d/wrap; OUT=/d/z8_results.txt; DONE=/d/z8_done.txt
cp /z/z8headless /tmp/ && cp /z/bios.p8 /tmp/ && chmod +x /tmp/z8headless && cd /tmp
touch "$OUT" "$DONE"
declare -A seen; while read -r d; do seen[$d]=1; done < "$DONE"
n=0; tot=$(ls "$WRAP"/*.p8 2>/dev/null | wc -l)
for f in "$WRAP"/*.p8; do
  id=$(basename "$f" .p8)
  [ -n "${seen[$id]:-}" ] && continue
  { echo "## $id"
    timeout 20 ./z8headless --cart "$f" --frames 2000 --datadir /tmp --out /tmp 2>&1 \
      | grep -E "^(FBHASH|AUDHASH)" | sort -u
  } >> "$OUT"
  echo "$id" >> "$DONE"
  n=$((n+1)); [ $((n%200)) -eq 0 ] && echo "z8 $n done (of ~$tot remaining-at-start)" >&2
done
echo "z8 phase complete: $(wc -l < "$DONE") carts in done-list" >&2
