#!/bin/bash
#
# PICO-8 handler — invoked by Master_Daemon when the PICO-8 core loads.
#
# Master_Daemon owns the lifecycle (kill on switch, zombie sweep,
# .s0 cleanup). This handler just sets up the environment and execs
# the binary.

GAMEDIR="/media/fat/games/PICO-8"
LOGDIR="/media/fat/logs/PICO-8"

# Recorder tree: top-level and per-core, alongside saves/ savestates/ config/
# logs/. Takes are <cart>_<slot>.inp, slots 1..8. The dot-directories hold the
# save-state snapshots a take carries and the isolated scratch a session runs
# against; dot-prefixed so the OSD "Load Replay" picker never lists them.
REPLAYS="/media/fat/replays/PICO-8"

cd "$GAMEDIR" || exit 1

# Created EVERY launch, and that is load-bearing rather than tidy: MiSTer only
# remembers a browse directory (Selected_S[]) while it still exists, so if this
# vanished, "Load Replay" would snap back to games/ and the takes would look
# gone. Also the binary can then assume the tree exists on the write path.
mkdir -p "$LOGDIR" "$GAMEDIR/Carts" "$REPLAYS" "$REPLAYS/.snapshots"

# Retire the pre-2026-08-02 location. rmdir (not rm -rf) is the whole point: it
# refuses on a non-empty directory, so anyone holding takes in there keeps them
# exactly where they are -- still playable via the OSD "Load Replay", which
# browses anywhere. Only the empty folder our old handler created goes away,
# rather than lingering in the cart browser as a decoy.
rmdir "$GAMEDIR/Replays" 2>/dev/null

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
# A recmode marker with NO reset marker beside it was never asked for.
#
# Record, Play and Reset-while-recording all write recmode and the reset
# marker together, then _exit(0) so the daemon respawns us. But recmode is
# consumed by the BINARY, at the first cart load. Kill the process in that
# window -- a core switch, a reboot, a crash -- and recmode outlives the
# request, so the next launch of ANY cart armed a recording nobody asked
# for, aimed at the previous cart's slot.
#
# The window is far shorter here than on OpenBOR (a cart loads in a moment,
# a PAK takes 12-90 s) and this core does not isolate saves from the marker,
# so it cannot lose real progress the way OpenBOR could -- but an unrequested
# recording is still wrong, and both cores should behave the same.
#
# Runs BEFORE the marker is consumed just below: its presence is the
# evidence, so this has to read it first.
if [ -f /tmp/pico8_recmode ] && [ ! -f /tmp/pico8_reset_marker ]; then
    rm -f /tmp/pico8_recmode /tmp/pico8_recslot /tmp/pico8_playfile 2>/dev/null
fi

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
# Rotate the ARM binary's log, IMMEDIATELY before the redirect that recreates
# it. The name is pico8.log, matching what the docs, the feature matrix and every
# debugging note already call it -- the binary used to dup2 its own pico8.log
# over stderr, so this rotation ran on a PICO-8.log that was permanently 0 bytes
# while the real output accumulated, unrotated, next to it.
#
# It sits HERE, not 40 lines up, because the two operations have to be
# neighbours. During a sister-core RBF swap MiSTer's CORENAME lags the loaded
# RBF by a second or two, so Master_Daemon briefly spawns the OUTGOING core's
# handler against the INCOMING RBF and kills it moments later -- a documented,
# harmless transient. With the rotation up top that short-lived spawn reached
# the mv but died in the `sleep 1` below before the exec, so it rotated the log
# away and never recreated it: pico8.log simply absent, which reads like the
# core never ran. Observed 2026-08-03 at 18:57:47. Nothing was lost (mv -f on a
# missing source is a no-op, so prev always holds the last real session), but a
# handler that never launches a binary should not touch the logs at all.
mv -f "$LOGDIR/pico8.log" "$LOGDIR/pico8.prev.log" 2>/dev/null

exec taskset 0x03 ./PICO-8 -nativevideo -data "$GAMEDIR/" > "$LOGDIR/pico8.log" 2>&1
