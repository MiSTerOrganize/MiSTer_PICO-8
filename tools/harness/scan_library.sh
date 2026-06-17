#!/bin/sh
# Mass-scan a PICO-8 cart library through the headless engine (z8headless) to
# flag broken carts WITHOUT playing them. Run inside a Linux env (e.g. WSL) with
# a fully-static z8headless + bios.p8 staged in /tmp/z8.
#
#   scan_library.sh <carts_root> [hold_mask | input_script.txt]
#
# Arg 2 (optional):
#   - a number  -> held P1 button mask every frame (bits: 1=L 2=R 4=U 8=D 16=O 32=X)
#   - a file    -> per-frame input script ("FRAME MASK" lines) fed via --input,
#                  used to tap past title/menu screens so the black/stuck signal
#                  is reliable (a no-input scan can't pass carts that wait for a
#                  button, e.g. Zokorimoro). CRASH/HANG are reliable either way.
#
# Per cart: run 120 frames under a timeout, record exit code + fingerprints of an
# early / mid / late frame. Output: /tmp/z8/scan_results.txt
#   index|exitcode|md5_early|md5_mid|md5_late|relpath
#   exit 124 = HANG ; exit 139/!=0 = CRASH ; all-3 md5 equal = STATIC/stuck.
BIN=/tmp/z8/z8headless
ROOT="${1:?usage: scan_library.sh <carts_root> [hold_mask|input.txt]}"
ARG2="${2:-0}"
RES=/tmp/z8/scan_results.txt
if [ -f "$ARG2" ]; then INPUT="--input $ARG2"; HOLD=""; else INPUT=""; HOLD="--hold $ARG2"; fi
cd /tmp/z8 || exit 1
: > "$RES"
i=0
find "$ROOT" \( -iname '*.p8' -o -iname '*.p8.png' \) | sort | while IFS= read -r cart; do
  i=$((i+1))
  rm -f frame_00005.png frame_00060.png frame_00119.png
  timeout 20 "$BIN" --cart "$cart" --frames 120 --dump 5,60,119 $HOLD $INPUT --out . >/dev/null 2>&1
  ec=$?
  m5=MISSING;  [ -f frame_00005.png ]  && m5=$(md5sum frame_00005.png  | cut -c1-12)
  m60=MISSING; [ -f frame_00060.png ]  && m60=$(md5sum frame_00060.png | cut -c1-12)
  m119=MISSING;[ -f frame_00119.png ]  && m119=$(md5sum frame_00119.png| cut -c1-12)
  rm -f frame_00005.png frame_00060.png frame_00119.png
  rel=${cart#"$ROOT/"}
  echo "$i|$ec|$m5|$m60|$m119|$rel" >> "$RES"
  [ $((i % 200)) -eq 0 ] && echo "...scanned $i"
done
echo "SCAN DONE: $(wc -l < "$RES") carts -> $RES"
