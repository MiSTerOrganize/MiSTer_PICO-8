#!/bin/bash
# Run the golden-trace regression gate. ONE command, so nobody hand-rolls the
# docker line -- every mount and flag below is load-bearing.
#
#   bash tools/harness/run_golden_gate.sh            # subset gate (~1 min)
#   bash tools/harness/run_golden_gate.sh --full     # whole corpus (hours)
#   Z8=/path/to/z8headless bash ...run_golden_gate.sh
#
# z8headless: pass Z8=, or it downloads the artifact from the newest successful
# diff_harness run -- which is the binary built from that commit, so the gate
# tests what CI built rather than a stale local copy.
#
# 🛑 WHY THIS IS NOT A CI GATE (2026-08-16). It cannot be, today:
#   - the golden corpus is deliberately gitignored (.gitignore:73 in the
#     workspace repo) -- 3,737 files, and that repo has NO REMOTE at all
#   - the cart corpus is user content and is not in any repo
# GitHub CI can see neither, so the gate runs LOCALLY, next to the data. Making
# it a CI gate needs one of: a committed subset of goldens AND carts, or a
# self-hosted runner with the library mounted (Track C phase 2).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WS="$(cd "$REPO/.." && pwd)"

CARTS="$WS/PICO-8_Carts/Carts"
GOLDENS="$WS/#Golden_Traces/PICO-8/goldens"
WORK="${WORK:-$HERE/.golden_gate_work}"
STAGE="$WORK/z8"

FULL=0
[ "${1:-}" = "--full" ] && FULL=1

for d in "$CARTS" "$GOLDENS"; do
    [ -d "$d" ] || { echo "ERROR: not found: $d" >&2; exit 2; }
done
command -v docker >/dev/null || { echo "ERROR: docker not on PATH" >&2; exit 2; }

rm -rf "$WORK"; mkdir -p "$STAGE" "$WORK/out"

if [ -n "${Z8:-}" ]; then
    cp "$Z8" "$STAGE/z8headless"
    cp "$(dirname "$Z8")/bios.p8" "$STAGE/bios.p8" 2>/dev/null || cp "$REPO/games/PICO-8/bios.p8" "$STAGE/bios.p8"
else
    echo "Fetching z8headless from the newest successful diff_harness run..."
    RID="$(gh run list -R MiSTerOrganize/MiSTer_PICO-8 --workflow diff_harness.yml \
             --status success --limit 1 --json databaseId --jq '.[0].databaseId')"
    [ -n "$RID" ] || { echo "ERROR: no successful diff_harness run to pull from" >&2; exit 2; }
    gh run download "$RID" -R MiSTerOrganize/MiSTer_PICO-8 -n z8headless-linux -D "$STAGE"
    echo "  from run $RID"
fi
[ -f "$STAGE/z8headless" ] || { echo "ERROR: no z8headless staged" >&2; exit 2; }

cp "$HERE/golden_check.sh" "$HERE/golden_gate.sh" "$STAGE/"
chmod +x "$STAGE"/*.sh "$STAGE/z8headless"

SUBSET_ENV=()
if [ "$FULL" -eq 0 ]; then
    cp "$HERE/golden_gate_subset.txt" "$STAGE/gate_subset.txt"
    SUBSET_ENV=(-e SUBSET=/z8/gate_subset.txt)
fi

# --security-opt seccomp=unconfined is REQUIRED: the workers use setarch -R to
# disable ASLR, which needs personality(ADDR_NO_RANDOMIZE); the default seccomp
# profile blocks it, and without stable addresses z8lua's pointer-keyed pairs()
# order changes per run and every layout-sensitive cart false-DIFFs.
set +e
MSYS_NO_PATHCONV=1 docker run --rm --security-opt seccomp=unconfined \
    -v "$CARTS:/carts:ro" \
    -v "$GOLDENS:/goldens:ro" \
    -v "$STAGE:/z8:ro" \
    -v "$WORK/out:/work" \
    "${SUBSET_ENV[@]}" \
    ubuntu:24.04 bash /z8/golden_gate.sh
rc=$?
set -e

echo
echo "results : $WORK/out/results.txt"
echo "actual  : $WORK/out/actual/   (traces for anything that did not match)"
if [ $rc -ne 0 ]; then
    cat <<'EOF'

A DIFF is not automatically an engine bug. Before changing any golden, run the
determinism worker on the cart:

  docker run --rm --security-opt seccomp=unconfined \
    -v "<carts>:/carts:ro" -v "<stage>:/z8:ro" -v "<tmp>:/work" \
    ubuntu:24.04 bash /z8/trace_worker.sh "/carts/<rel>"

  DET    -> the engine is reproducible and the GOLDEN is stale. Expected after
            any change that shifts the Lua heap: z8lua hashes object keys by
            POINTER, so layout-sensitive carts legitimately change pairs() order.
            Re-baseline, and record WHY in the golden README.
  NONDET -> a real determinism regression. Do NOT re-baseline; fix the engine.
EOF
fi
exit $rc
