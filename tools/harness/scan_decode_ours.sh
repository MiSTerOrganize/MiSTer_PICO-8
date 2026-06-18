#!/bin/sh
# z8headless (our engine) side of the decode-differential. Per cart: run
# --dumpcode (decompress the cart's code via our loader), record
# relpath|status|charlen. Joined against the shrinko8 side to flag carts where
# OUR decode diverges (truncation / garbage / load failure) = a bug in our cart
# loader/decompressor. Run inside WSL with z8headless+bios staged in /tmp/z8.
#   scan_decode_ours.sh <carts_root>
BIN=/tmp/z8/z8headless
ROOT="${1:?usage: scan_decode_ours.sh <carts_root>}"
RES=/tmp/z8/ours_codes.txt
cd /tmp/z8 || exit 1
: > "$RES"
i=0
find "$ROOT" \( -iname '*.p8' -o -iname '*.p8.png' \) | sort | while IFS= read -r cart; do
  i=$((i+1))
  rm -f /tmp/z8/cc
  timeout 20 "$BIN" --cart "$cart" --frames 1 --dumpcode /tmp/z8/cc --out /tmp/z8 >/dev/null 2>&1
  rc=$?
  if [ -s /tmp/z8/cc ]; then
    len=$(tr -d '\r' < /tmp/z8/cc | wc -c)
    st=OK
  else
    len=0
    [ $rc -eq 0 ] && st=EMPTY || st=FAIL$rc
  fi
  rel=${cart#"$ROOT/"}
  echo "$i|$st|$len|$rel" >> "$RES"
  [ $((i % 400)) -eq 0 ] && echo "...ours decoded $i"
done
echo "OURS DECODE DONE: $(wc -l < "$RES") -> $RES"
