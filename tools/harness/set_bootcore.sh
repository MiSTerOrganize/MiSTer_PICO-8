#!/bin/sh
# set_bootcore.sh -- pick which hybrid core the MiSTer auto-boots into.
# Run ON the MiSTer:  sh /tmp/set_bootcore.sh PICO-8
#                     sh /tmp/set_bootcore.sh OpenBOR_7533
#                     sh /tmp/set_bootcore.sh --off        (back to the menu)
#
# This is the core-loading half of the menu-free test chain:
#   bootcore= + reboot -> core loads    .s0 -> content    .s1 -> replay
#
# VALUE FORMAT (this is the trap -- a wrong value silently boots to MENU):
#   corename                 -> first corename_*.rbf          <-- use this
#   corename_yyyymmdd.rbf    -> that exact file
#   lastcore / lastexactcore -> whatever was loaded last
# The bare RBF basename WITHOUT .rbf (e.g. OpenBOR_7533_20260726) is none of
# these and boots to MENU with no error. Cost a boot cycle to learn.

INI=/media/fat/MiSTer.ini
set -e

if [ "$1" = "--off" ]; then
    if [ -f "$INI" ]; then rm -f "$INI"; echo "removed $INI -- boots to MENU (stock default)"
    else echo "no $INI -- already booting to MENU"; fi
    sync; exit 0
fi

CORE="$1"
[ -n "$CORE" ] || { echo "usage: $0 <corename|--off> [timeout]"; exit 2; }
TMO="${2:-10}"

# Refuse a value that cannot match, rather than letting it fail silently at boot.
if ! ls /media/fat/_Other/"$CORE"_*.rbf >/dev/null 2>&1 && [ ! -f /media/fat/_Other/"$CORE" ]; then
    echo "ERROR: no /media/fat/_Other/${CORE}_*.rbf -- this would boot to MENU."
    echo "Available:"; ls /media/fat/_Other/*.rbf | sed 's|.*/|  |'
    exit 1
fi

[ -f "$INI" ] && cp "$INI" "$INI.bak"
cat > "$INI" <<INIEOF
[MiSTer]
; written by set_bootcore.sh -- menu-free core loading for the test harness.
; UNDO: rm $INI   (or: sh set_bootcore.sh --off)
bootcore=$CORE
bootcore_timeout=$TMO
INIEOF
sync
echo "$INI now boots: $CORE (timeout ${TMO}s, cancellable from the pad)"
echo "reboot to apply:  reboot"
