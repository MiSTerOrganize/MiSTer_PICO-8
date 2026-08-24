#!/usr/bin/env bash
# Block a MiSTer_Frontier DB-rebuild dispatch until raw.githubusercontent
# actually serves what main now has.
#
# WHY THIS EXISTS
# ---------------
# build_db.py FETCHES the raw.githubusercontent URLs named in
# MiSTer_Frontier/external_files.csv. It does NOT read a checkout. So a
# dispatch fired the instant `git push` returns races CDN propagation, and
# db.json can record the PREVIOUS hash for a file the push just replaced.
#
# Measured 2026-08-24 on MiSTer_PICO-8: commit-back 23:26:19Z, DB rebuild
# start 23:26:22Z -- three seconds. The ARM binary had propagated; bios.p8
# had not, so db.json shipped a stale bios hash. That is the update_all
# failure mode of 2026-05-26: the downloader rejects the mismatched file and
# DELETES it, leaving a broken core on the user's card until the daily cron.
#
# Both dispatch paths race it -- build.yml's commit-back (binary) and
# dispatch_db.yml (docs/handler/RBF pushes) -- which is why this is a script
# both call rather than a block pasted into each.
#
# EXIT CODES (distinct on purpose -- a caller testing `rc != 0` would treat a
# usage error as a refusal and skip a dispatch that should have happened)
#   0  every named file is served correctly    -> dispatch
#   1  gave up waiting, or a file is missing   -> do NOT dispatch
#   2  usage error
#
# LIMIT, stated honestly: raw.githubusercontent is edge-cached per-POP, and
# the DB runner may resolve to a different edge than this one. This makes the
# race unlikely, not impossible. The daily cron remains the backstop; a
# post-build check in MiSTer_Frontier is the complete fix.
set -u

TRIES="${CDN_TRIES:-15}"
NAP="${CDN_NAP:-10}"
BRANCH="${CDN_BRANCH:-main}"
REPO="${GITHUB_REPOSITORY:-}"

if [ -z "$REPO" ]; then
    echo "cdn_ready: GITHUB_REPOSITORY is not set" >&2
    exit 2
fi
if [ "$#" -eq 0 ]; then
    echo "usage: cdn_ready.sh <repo-relative-path>..." >&2
    exit 2
fi

probe="$(mktemp)"
trap 'rm -f "$probe"' EXIT

for i in $(seq 1 "$TRIES"); do
    all_ok=1
    for p in "$@"; do
        if [ ! -f "$p" ]; then
            # Vouching for a file we cannot read would be worse than waiting.
            echo "  $p: not in the working tree -- cannot verify" >&2
            all_ok=0
            continue
        fi
        want=$(md5sum "$p" | cut -d' ' -f1)
        url="https://raw.githubusercontent.com/$REPO/$BRANCH/$p"
        if curl -fsSL --max-time 30 -o "$probe" "$url"; then
            got=$(md5sum "$probe" | cut -d' ' -f1)
        else
            got="fetch-failed"
        fi
        if [ "$got" != "$want" ]; then
            echo "  attempt $i/$TRIES: $p not propagated (cdn=$got want=$want)"
            all_ok=0
        fi
    done
    if [ "$all_ok" = "1" ]; then
        echo "CDN serves this commit's files (attempt $i) -- proceeding."
        exit 0
    fi
    if [ "$i" -lt "$TRIES" ]; then
        sleep "$NAP"
    fi
done

exit 1
