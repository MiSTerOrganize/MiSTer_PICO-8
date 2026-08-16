#!/bin/bash
# Golden-trace regression gate for the REPO-OWNED carts -- the half that CAN run
# in CI.
#
#   bash tools/harness/repo_golden_gate.sh              # gate: compare, exit 1 on drift
#   bash tools/harness/repo_golden_gate.sh --rebaseline # regenerate the goldens
#   Z8=/path/to/z8headless bash tools/harness/repo_golden_gate.sh
#
# WHY THIS EXISTS, when tools/harness/golden_gate.sh already gates the corpus:
# the corpus gate cannot run in CI. Its 3,737 goldens are gitignored in a repo
# with no remote, and the 3,302 carts are third-party content in no repo at all
# -- committing strangers' games to a public repo is redistribution, not a
# tooling problem. So CI could never compare against ANY golden. It ran two
# traces of one cart against EACH OTHER, which proves the engine is
# deterministic and is blind to it being deterministically WRONG -- exactly the
# class the golden corpus exists to catch, and the class that shipped 5c89107.
#
# These 28 carts are OURS (conformance + test carts), tracked, and CI can see
# them. Narrow, but it closes that hole for everything CI can legitimately read.
#
# 🛑 NO setarch -R HERE, and that is deliberate -- do not "restore" it to match
# trace_worker.sh. The corpus goldens need ASLR off because z8lua hashes object
# keys by POINTER, so layout-sensitive carts need stable addresses. CI cannot
# rely on that: it builds its own binary with its own flags, so the heap layout
# differs from ours by construction, and personality(ADDR_NO_RANDOMIZE) may be
# blocked by the runner's seccomp profile anyway. These 28 were MEASURED instead
# (2026-08-16): 3 runs each with ASLR ON were byte-identical, and a CI-style
# cmake build reproduced all 28 traces exactly. They are build-invariant, so
# they need no address pinning. A cart that is NOT build-invariant must never be
# added here -- verify before adding, or the gate false-fails on arrival and
# everyone learns to ignore it.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
GOLDENS="$HERE/repo_goldens"

REBASE=0
[ "${1:-}" = "--rebaseline" ] && REBASE=1

Z8="${Z8:-$REPO/build/z8headless}"
# 🛑 MUST be absolute: the run loop cd's into each cart's directory, so a
# relative Z8 (CI passes ./build/z8headless) stops resolving and every cart
# comes back rc=127. Verified locally only with an absolute path first time
# round, which is exactly why CI caught what the local run could not.
case "$Z8" in
    /*) ;;
    *)  Z8="$(cd "$(dirname "$Z8")" 2>/dev/null && pwd)/$(basename "$Z8")" ;;
esac
[ -x "$Z8" ] || { echo "ERROR: no z8headless at $Z8 (set Z8=, or build it first)" >&2; exit 2; }

# bios.p8 must sit in the datadir the engine is pointed at.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp "$REPO/src/pico8/bios.p8" "$STAGE/bios.p8" || exit 2

mkdir -p "$GOLDENS"

# The cart set is derived from git, not from a hand-kept list, so a cart added
# to the repo cannot silently sit outside the gate.
# bios.p8 is excluded: it is the BIOS, not a cart, so tracing it 'as a cart'
# exercises a path no user takes -- and every real cart below already runs
# through it, so a BIOS regression still shows up here, 27 times over.
mapfile -t CARTS < <(cd "$REPO" && git ls-files | grep -iE '\.p8$' | grep -vE '^src/' | grep -vE '(^|/)bios\.p8$' | sort)
[ "${#CARTS[@]}" -gt 0 ] || { echo "ERROR: no repo-owned carts found" >&2; exit 2; }

pass=0; fail=0; new=0; runfail=0
FAILED=()

for rel in "${CARTS[@]}"; do
    cart="$REPO/$rel"
    golden="$GOLDENS/$rel.trace"
    # same-length trace path, matching the corpus workers' discipline
    t="$(mktemp /tmp/ta.XXXXXX)"
    h="$(mktemp -d)"
    ( cd "$(dirname "$cart")" && HOME="$h" Z8_SAVES_DIR="$h/saves" timeout 20 \
        "$Z8" --cart "$cart" --frames 120 --datadir "$STAGE/" --test "$t" \
        >/dev/null 2>&1 )
    rc=$?
    rm -rf "$h"

    if [ $rc -ne 0 ] || [ ! -s "$t" ]; then
        # Counted separately from DIFF on purpose. Reporting a binary that
        # never ran as "drifted" sends the reader hunting a behaviour change
        # that did not happen -- rc=127 is a missing binary, not a regression.
        echo "RUNFAIL  $rel (rc=$rc)"
        FAILED+=("$rel (run failed, rc=$rc)")
        runfail=$((runfail + 1)); rm -f "$t"; continue
    fi

    if [ "$REBASE" -eq 1 ]; then
        mkdir -p "$(dirname "$golden")"
        cp "$t" "$golden"
        echo "baselined $rel"
        pass=$((pass + 1))
    elif [ ! -f "$golden" ]; then
        # Loud, not skipped: a gate that quietly covers less than it claims is
        # worse than one that fails.
        echo "NEW      $rel -- no golden. Run with --rebaseline and commit it."
        FAILED+=("$rel (no golden)")
        new=$((new + 1))
    elif cmp -s "$t" "$golden"; then
        pass=$((pass + 1))
    else
        echo "DIFF     $rel"
        diff "$golden" "$t" | head -4 | sed 's/^/           /'
        FAILED+=("$rel")
        fail=$((fail + 1))
    fi
    rm -f "$t"
done

# A golden with no cart behind it means the gate is measuring nothing.
while IFS= read -r g; do
    [ -z "$g" ] && continue
    rel="${g#"$GOLDENS/"}"; rel="${rel%.trace}"
    [ -f "$REPO/$rel" ] || { echo "ORPHAN   $rel.trace -- golden with no cart"; FAILED+=("$rel (orphan golden)"); fail=$((fail + 1)); }
done < <(find "$GOLDENS" -name '*.trace' 2>/dev/null)

echo
if [ "$REBASE" -eq 1 ]; then
    echo "re-baselined $pass cart(s). Commit tools/harness/repo_goldens/ and say WHY."
    exit 0
fi
echo "repo golden gate: $pass matched, $fail drifted, $new without a golden, $runfail did not run"
if [ "$runfail" -gt 0 ]; then
    echo "  🛑 $runfail cart(s) did not RUN. That is not drift -- rc=127 means the"
    echo "     binary was not found (a relative Z8 breaks once we cd into a cart dir)."
fi
if [ "${#FAILED[@]}" -gt 0 ]; then
    echo
    echo "NOT MATCHING:"; printf '  %s\n' "${FAILED[@]}"
    cat <<'EOF'

A DIFF is not automatically a bug -- an intentional render or logic change
invalidates these traces legitimately. But unlike the corpus carts, these 28 are
NOT address-sensitive (measured), so "the heap moved" is NOT an explanation here.
A DIFF means behaviour changed. Establish which change and whether it was
intended, then:

  bash tools/harness/repo_golden_gate.sh --rebaseline

and commit the goldens WITH the reason in the message.
EOF
    exit 1
fi
echo "GATE PASS"
