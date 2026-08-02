#!/bin/bash
#
# PICO-8 handler — invoked by Master_Daemon when the PICO-8 core loads.
#
# Master_Daemon owns the lifecycle (kill on switch, zombie sweep,
# .s0 cleanup). This handler just sets up the environment and execs
# the binary.

GAMEDIR="/media/fat/games/PICO-8"
LOGDIR="/media/fat/logs/PICO-8"

cd "$GAMEDIR" || exit 1

mkdir -p "$LOGDIR" "$GAMEDIR/Carts" "$GAMEDIR/Replays"

# Rotate ARM-binary stdout/stderr log
mv -f "$LOGDIR/PICO-8.log" "$LOGDIR/PICO-8.prev.log" 2>/dev/null

# Belt-and-suspenders .s0 cleanup — Master_Daemon already clears .s0 on
# core transitions, but MiSTer Main's auto-resume-last-file behavior can
# leave PICO-8.s0 populated when handler spawns. Clearing here at handler
# start (before binary launches, before MGL's 2-second timer) gives users
# a clean "go to OSD picker" experience on every entry. Cross-applied from
# the OpenBOR handler fix per `feedback_s0_reboot_survival_cleanup.md`.
#
# EXCEPTION: pause-menu Reset Cart writes /tmp/pico8_reset_marker before
# _exit(0). Reset needs .s0 PRESERVED so binary re-mounts the same cart.
# If marker exists: skip cleanup + delete marker. (Mirrors OpenBOR's
# pattern; same regression family — without the exception, Reset would
# break Quit/Reset asymmetry and produce black screen on Reset Cart.)
if [ -f /tmp/pico8_reset_marker ]; then
    rm -f /tmp/pico8_reset_marker 2>/dev/null
else
    rm -f /media/fat/config/PICO-8.s0 2>/dev/null
fi

# NOTE: .s1 (the OSD "Load Replay" slot) is intentionally NOT cleared here.
# The binary detects a replay pick by .s1's MTIME, baselined at startup —
# MiSTer bumps that mtime on every pick, even re-picking the same .inp, so a
# fresh pick triggers while a stale/unchanged .s1 never auto-replays. Clearing
# it would risk a clear-then-restore reading as a pick. Mirrors the OpenBOR
# handler; see feedback_hybrid_core_s1_replay_slot.md.

# Free kernel page cache before launch — PICO-8 carts are small but
# this keeps RAM state clean across multicart sub-loads.
echo 3 > /proc/sys/vm/drop_caches 2>/dev/null

# FPGA settle on first launch (the FPGA was just reflashed)
sleep 1

# taskset 0x03 (both cores) so the binary's own thread pins take effect — a child
# thread cannot widen affinity past the process mask. WHICH thread goes WHERE is
# decided in mister_main.cpp, not here: render/main -> core 0, audio -> core 1
# (rule INVERTED 2026-06-13; core 0 has ~1.85x core 1's DDR3 read bandwidth and
# render is the memory-bound one). See feedback_affinity_render_core0_audio_core1.md.
exec taskset 0x03 ./PICO-8 -nativevideo -data "$GAMEDIR/" > "$LOGDIR/PICO-8.log" 2>&1
