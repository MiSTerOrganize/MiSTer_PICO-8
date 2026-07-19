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
# Both runs write to SAME-LENGTH tmpfs paths (mv into the golden slot after).
# The --test path's LENGTH changes the process heap layout (std::string SSO
# boundary at 15 chars -> one extra allocation -> every later address shifts),
# and carts iterating object-keyed tables see a different pairs() order per
# layout (address-hashed keys; ASLR-off makes layout deterministic, not
# path-invariant). Writing run A to the long golden path and run B to a short
# mktemp path made such carts (Snak, CluePix Halloween) compare two different
# deterministic modes -> permanent false NONDET (found 2026-07-19).
ta="$(mktemp /tmp/ta.XXXXXX)"
tb="$(mktemp /tmp/tb.XXXXXX)"
run() {
  # cwd = the cart's own dir so multicart sibling load() resolves; fresh HOME
  # per run so cartdata written by run A can't leak into run B (or across
  # parallel workers). Corpus mount is ro — user carts are never written.
  local h; h="$(mktemp -d)"
  # Per-run isolated saves via Z8_SAVES_DIR: zepto8 otherwise writes
  # cartdata to the shared hardcoded /media/fat/saves/PICO-8/ path even
  # headless -- run A's save file changes run B's boot state, and carts
  # SHARING a cartdata id (CluePix / CluePix Halloween) interleave files
  # across parallel workers (2026-07-19: the last-NONDET root cause).
  # setarch -R disables ASLR: z8lua hashes table/function KEYS by pointer,
  # so pairs() order over object-keyed tables needs stable addresses to be
  # reproducible (string-keyed order is covered by Z8_TEST_SEED). Container
  # must run with a seccomp profile that allows personality(ADDR_NO_RANDOMIZE).
  ( cd "$(dirname "$cart")" && HOME="$h" Z8_SAVES_DIR="$h/saves" timeout 20 setarch "$(uname -m)" -R /z8/z8headless --cart "$cart" --frames 120 --datadir /z8/ --test "$1" >/dev/null 2>&1 )
  local ec=$?
  rm -rf "$h"
  return $ec
}
run "$ta"; eca=$?
run "$tb"; ecb=$?
if   [ $eca -eq 124 ] || [ $ecb -eq 124 ]; then cls=HANG
elif [ $eca -ne 0 ]  || [ $ecb -ne 0 ];  then cls="CRASH$eca"
elif cmp -s "$ta" "$tb"; then cls=DET
else cls=NONDET
fi
lines=0; [ -f "$ta" ] && lines=$(wc -l < "$ta")
echo "$cls|$eca|$ecb|$lines|$rel" >> "$OUT/results.txt"
rm -f "$tb"
case "$cls" in DET|NONDET) mv -f "$ta" "$g" ;; *) rm -f "$ta" ;; esac
