#!/bin/bash
# pico8_daemon.sh — Auto-start PICO-8 emulator when core loads
# Installed by Install_PICO-8.sh into games/PICO-8/
#
# Uses mkdir as atomic lock to guarantee only ONE daemon runs.
# Monitors /tmp/CORENAME for "PICO-8" and starts/stops the binary.

LOCKDIR="/tmp/pico8_daemon.lock"
PIDFILE="/tmp/pico8_arm.pid"
BINARY="/media/fat/games/PICO-8/PICO-8"
ARGS="-nativevideo -data /media/fat/games/PICO-8/"

# Prevent multiple daemon instances (mkdir is atomic)
if ! mkdir "$LOCKDIR" 2>/dev/null; then
    # Check if the old daemon is actually still running
    OLDPID=$(cat "$LOCKDIR/pid" 2>/dev/null)
    if [ -n "$OLDPID" ] && kill -0 "$OLDPID" 2>/dev/null; then
        exit 0  # another daemon is alive
    fi
    # Old daemon is dead, take over
    rm -rf "$LOCKDIR"
    mkdir "$LOCKDIR" 2>/dev/null || exit 0
fi
echo $$ > "$LOCKDIR/pid"

cleanup() {
    kill $(cat "$PIDFILE" 2>/dev/null) 2>/dev/null
    rm -f "$PIDFILE"
    rm -rf "$LOCKDIR"
    exit 0
}
trap cleanup TERM INT

LAST_CORE=""
while true; do
    CUR=$(cat /tmp/CORENAME 2>/dev/null)
    if [ "$CUR" = "PICO-8" ]; then
        if [ "$LAST_CORE" != "PICO-8" ]; then
            # Initial core load — FPGA needs settling time
            kill $(cat "$PIDFILE" 2>/dev/null) 2>/dev/null
            sleep 1
            taskset 03 $BINARY $ARGS > /dev/null 2>&1 &
            echo $! > "$PIDFILE"
        elif ! kill -0 $(cat "$PIDFILE" 2>/dev/null) 2>/dev/null; then
            # Process died (hot-swap or crash) — restart fast
            taskset 03 $BINARY $ARGS > /dev/null 2>&1 &
            echo $! > "$PIDFILE"
        fi
    elif [ "$LAST_CORE" = "PICO-8" ]; then
        kill $(cat "$PIDFILE" 2>/dev/null) 2>/dev/null
        rm -f "$PIDFILE"
    fi
    LAST_CORE="$CUR"
    sleep 0.25
done
