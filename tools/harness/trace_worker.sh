#!/bin/bash
# Per-cart golden-trace worker: two boot runs of z8headless --test, diff,
# classify, keep run A as the golden. Invoked by p8_trace_scan.sh via xargs -P.
# Mounts: /carts (ro corpus), /z8 (ro: z8headless + bios.p8 + this), /work (rw).
set -u
cart="$1"
ROOT=/carts
OUT=/work
rel="${cart#$ROOT/}"
g="$OUT/goldens/$rel.trace"
mkdir -p "$(dirname "$g")"
tb="$(mktemp /tmp/tb.XXXXXX)"
run() {
  # cwd = the cart's own dir so multicart sibling load() resolves; fresh HOME
  # per run so cartdata written by run A can't leak into run B (or across
  # parallel workers). Corpus mount is ro — user carts are never written.
  local h; h="$(mktemp -d)"
  # setarch -R disables ASLR: z8lua hashes table/function KEYS by pointer,
  # so pairs() order over object-keyed tables needs stable addresses to be
  # reproducible (string-keyed order is covered by Z8_TEST_SEED). Container
  # must run with a seccomp profile that allows personality(ADDR_NO_RANDOMIZE).
  ( cd "$(dirname "$cart")" && HOME="$h" timeout 20 setarch "$(uname -m)" -R /z8/z8headless --cart "$cart" --frames 120 --datadir /z8/ --test "$1" >/dev/null 2>&1 )
  local ec=$?
  rm -rf "$h"
  return $ec
}
run "$g";  eca=$?
run "$tb"; ecb=$?
if   [ $eca -eq 124 ] || [ $ecb -eq 124 ]; then cls=HANG
elif [ $eca -ne 0 ]  || [ $ecb -ne 0 ];  then cls="CRASH$eca"
elif cmp -s "$g" "$tb"; then cls=DET
else cls=NONDET
fi
lines=0; [ -f "$g" ] && lines=$(wc -l < "$g")
echo "$cls|$eca|$ecb|$lines|$rel" >> "$OUT/results.txt"
rm -f "$tb"
case "$cls" in DET|NONDET) ;; *) rm -f "$g" ;; esac
