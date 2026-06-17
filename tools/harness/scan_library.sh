#!/bin/sh
# Mass-scan a PICO-8 cart library through the headless engine (z8headless) to
# flag broken carts WITHOUT playing them. Run inside a Linux env (e.g. WSL) with
# a fully-static z8headless + bios.p8 staged in /tmp/z8.
#
# Per cart: run N frames under a timeout, record exit code + fingerprints of an
# early and a late frame:
#   exit 0           = ran clean
#   exit 124         = HANG (timeout) -> engine stuck
#   exit 139 / != 0  = CRASH (segfault etc.)  <- highest-value signal
#   m5 == m59        = STATIC (frame unchanged; title screen OR stuck)
#   m5 == MISSING    = no render
#
# Output: /tmp/z8/scan_results.txt  ("index|exitcode|md5_early|md5_late|relpath")
# Then analyze with tools/harness/analyze_scan.py on the host.
#
# NOTE: a no-input scan can't pass title screens (many carts sit on a black/
# static screen until a button is pressed) -- so STATIC/BLACK are noisy. Re-run
# with --hold to advance past intros for a cleaner black/stuck signal. CRASH and
# HANG are reliable regardless of input.
BIN=/tmp/z8/z8headless
ROOT="${1:?usage: scan_library.sh <carts_root> [hold_mask]}"
HOLD="${2:-0}"
RES=/tmp/z8/scan_results.txt
cd /tmp/z8 || exit 1
: > "$RES"
i=0
find "$ROOT" \( -iname '*.p8' -o -iname '*.p8.png' \) | sort | while IFS= read -r cart; do
  i=$((i+1))
  rm -f frame_00005.png frame_00059.png
  timeout 15 "$BIN" --cart "$cart" --frames 60 --dump 5,59 --hold "$HOLD" --out . >/dev/null 2>&1
  ec=$?
  if [ -f frame_00005.png ]; then m5=$(md5sum frame_00005.png | cut -c1-12); else m5=MISSING; fi
  if [ -f frame_00059.png ]; then m59=$(md5sum frame_00059.png | cut -c1-12); else m59=MISSING; fi
  rm -f frame_00005.png frame_00059.png
  rel=${cart#"$ROOT/"}
  echo "$i|$ec|$m5|$m59|$rel" >> "$RES"
  [ $((i % 200)) -eq 0 ] && echo "...scanned $i"
done
echo "SCAN DONE: $(wc -l < "$RES") carts -> $RES"
