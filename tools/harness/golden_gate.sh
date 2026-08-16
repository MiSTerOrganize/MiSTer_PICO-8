#!/bin/bash
# Golden-trace regression GATE (container entry point). Runs golden_check.sh
# over a cart list and exits non-zero if anything drifted.
#
#   SUBSET=/z8/gate_subset.txt  bash /z8/golden_gate.sh    # the fast gate
#   bash /z8/golden_gate.sh                                # whole corpus
#
# Mounts: /carts (ro), /goldens (ro), /z8 (ro), /work (rw)
set -u
OUT=/work
mkdir -p "$OUT/actual"
: > "$OUT/results.txt"

if [ -n "${SUBSET:-}" ] && [ -f "${SUBSET:-}" ]; then
    # Subset file holds corpus-relative paths, one per line.
    : > /tmp/cartlist
    missing=0
    while IFS= read -r rel; do
        [ -z "$rel" ] && continue
        case "$rel" in \#*) continue ;; esac
        if [ -f "/carts/$rel" ]; then
            printf '%s\0' "/carts/$rel" >> /tmp/cartlist
        else
            echo "SUBSET-MISSING|-|$rel|not in the corpus" >> "$OUT/results.txt"
            missing=$((missing + 1))
        fi
    done < "$SUBSET"
    # 🛑 A subset entry that no longer exists must be LOUD. Silently skipping it
    # shrinks the gate every time a cart is renamed, and a gate that quietly
    # covers less is worse than one that fails.
    [ "$missing" -gt 0 ] && echo "WARNING: $missing subset entr(y|ies) not found in the corpus"
    echo "gate: $(tr -cd '\0' < /tmp/cartlist | wc -c) carts (subset)"
else
    find /carts \( -iname '*.p8' -o -iname '*.p8.png' \) -print0 | sort -z > /tmp/cartlist
    echo "gate: $(tr -cd '\0' < /tmp/cartlist | wc -c) carts (FULL corpus)"
fi

xargs -0 -P "$(nproc)" -I{} bash /z8/golden_check.sh {} < /tmp/cartlist

echo "=== class histogram ==="
cut -d'|' -f1 "$OUT/results.txt" | sort | uniq -c | sort -rn

fails=$(grep -cvE '^MATCH\|' "$OUT/results.txt" || true)
if [ "${fails:-0}" -gt 0 ]; then
    echo "=== NOT MATCHING (first 25) ==="
    grep -vE '^MATCH\|' "$OUT/results.txt" | head -25
    echo "GATE FAIL: $fails cart(s) did not match their golden"
    exit 1
fi
echo "GATE PASS: every cart matched its golden"
