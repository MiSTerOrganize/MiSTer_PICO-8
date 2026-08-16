#!/bin/bash
# Golden-trace REGRESSION worker: one boot run of z8headless --test per cart,
# compared against the STORED golden. Container entry point.
#
# This is the sibling of trace_worker.sh and the difference matters:
#   trace_worker.sh  runs a cart TWICE and compares A vs B  -> is it DETERMINISTIC
#   golden_check.sh  runs it ONCE and compares to the golden -> did it CHANGE
# The first proves the engine is reproducible. It passes happily when every
# frame of every cart changes identically, which is exactly the regression the
# golden corpus exists to catch.
#
# Mounts: /carts (ro corpus), /z8 (ro: z8headless + bios.p8 + this),
#         /goldens (ro: the stored .trace tree), /work (rw: results)
#
# 🛑 THE RUN CONDITIONS BELOW ARE COPIED FROM trace_worker.sh AND MUST STAY
# IDENTICAL. A golden is only comparable to a run made the same way, and every
# one of these was learned the hard way:
#   - --test            fixed PRNG, fixed Lua string-hash seed, fixed stat date
#   - setarch -R        ASLR off; z8lua hashes object keys by POINTER, so
#                       pairs() order needs stable addresses
#   - Z8_SAVES_DIR      per-run cartdata; otherwise run A's save alters run B
#   - fresh HOME        same reason, across parallel workers
#   - cwd = cart dir    so multicart sibling load() resolves
#   - SAME-LENGTH out   the trace path's LENGTH changes heap layout (SSO at 15
#                       chars), which flips pairs() order on layout-sensitive
#                       carts. The goldens were produced writing to
#                       "/tmp/ta.XXXXXX", so THIS MUST TOO -- writing straight
#                       to a golden-shaped path would false-DIFF Snak and
#                       CluePix Halloween every run.
# tools/harness/test_golden_check_sync.py asserts this invocation still matches
# trace_worker.sh, so a change to one that is not mirrored fails loudly.
set -u
cart="$1"
ROOT=/carts
OUT=/work
rel="${cart#$ROOT/}"
golden="/goldens/$rel.trace"

t="$(mktemp /tmp/ta.XXXXXX)"          # same shape AND length as the golden run
h="$(mktemp -d)"
( cd "$(dirname "$cart")" && HOME="$h" Z8_SAVES_DIR="$h/saves" timeout 20 \
    setarch "$(uname -m)" -R /z8/z8headless \
    --cart "$cart" --frames 120 --datadir /z8/ --test "$t" >/dev/null 2>&1 )
ec=$?
rm -rf "$h"

if   [ $ec -eq 124 ];        then cls=HANG
elif [ $ec -ne 0 ];          then cls="CRASH$ec"
elif [ ! -f "$golden" ];     then cls=NOGOLDEN
elif cmp -s "$t" "$golden";  then cls=MATCH
else                              cls=DIFF
fi

# On a DIFF keep the first differing line -- enough to triage without storing
# the whole trace for thousands of carts.
detail=""
if [ "$cls" = DIFF ]; then
    detail="$(diff "$golden" "$t" 2>/dev/null | head -4 | tr '\n' ' ')"
fi
echo "$cls|$ec|$rel|$detail" >> "$OUT/results.txt"

# Keep the actual trace only for failures, so a re-baseline or a triage has it.
if [ "$cls" = DIFF ] || [ "$cls" = NOGOLDEN ]; then
    mkdir -p "$(dirname "$OUT/actual/$rel")"
    mv -f "$t" "$OUT/actual/$rel.trace"
else
    rm -f "$t"
fi
