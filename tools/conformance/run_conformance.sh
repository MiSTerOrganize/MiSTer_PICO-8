#!/bin/sh
# z8conformance differential runner (zepto8 side).
# Runs each conformance cart through z8headless and diffs its CONFHASH/CONFVAL
# output against goldens.txt (ground truth from PICO-8). Any mismatch
# = a zepto8 conformance bug, pinned to the cart's API.
#
# Usage (in a glibc/musl container or WSL with z8headless + bios.p8 present):
#   Z8=/path/to/z8headless  DATADIR=/path/with/bios.p8  sh run_conformance.sh
# Defaults assume the cwd holds z8headless + bios.p8 + the carts + goldens.txt.
#
# Official-side goldens are regenerated LOCALLY (PICO-8 is never in CI):
#   for c in *.p8; do pico8 -x "$c" -home /tmp/p8h; done   # capture printh "CONF*"
# (see README.md). This runner needs only z8headless + the committed goldens.

Z8="${Z8:-./z8headless}"
DATADIR="${DATADIR:-.}"
GOLD="${GOLD:-goldens.txt}"
CARTS="mech_check fix32_math peek_extmem pal_secret tline_map rnd_seq"

fail=0; total=0
for cart in $CARTS; do
  got=$("$Z8" --cart "$cart.p8" --frames 1 --datadir "$DATADIR" --out /tmp 2>&1 \
        | grep -E '^CONF(HASH|VAL)' | sort -u)
  # extract this cart's golden block ([cart] .. next [ or EOF), drop comments/blanks
  want=$(awk -v c="[$cart]" '$0==c{f=1;next} /^\[/{f=0} f && /^CONF/{print}' "$GOLD" | sort -u)
  total=$((total+1))
  if [ "$got" = "$want" ]; then
    echo "PASS  $cart"
  else
    fail=$((fail+1))
    echo "FAIL  $cart  -- zepto8 diverges from PICO-8:"
    printf '%s\n' "$want" > /tmp/_want.$$ ; printf '%s\n' "$got" > /tmp/_got.$$
    diff /tmp/_want.$$ /tmp/_got.$$ | sed 's/^/      /'
    rm -f /tmp/_want.$$ /tmp/_got.$$
  fi
done
echo "===== z8conformance: $((total-fail))/$total carts conformant ====="
[ "$fail" -eq 0 ] || exit 1
