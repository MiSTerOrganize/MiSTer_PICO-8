// replay_slot_ui -- ONE replay-slot value, shared between the OSD picker
// ("Replay Slot 1-8") and the core's own in-game pause-menu picker.
//
// The rule this module exists to enforce: the slot the user SEES in the OSD
// and the slot the pause menu SEES are the same value, so moving it in either
// place moves the other. Two storage locations would let the two displays
// disagree, which is the bug class this design pre-empts.
//
// Both directions, mirroring savestate_ui.sv (the working precedent that lets
// F1-F4 move the OSD's save-slot display):
//
//   OSD  -> core : lastOSDsetting != status_slot
//   core -> OSD  : arm_seq changes -> adopt arm_slot and pulse statusUpdate,
//                  which makes hps_io write status_in back as the new status.
//
// Wire encoding is 0-based (0..7) end to end; the ARM presents 1..8 to the
// user and converts at its own boundary. Keeping one convention on the wire
// avoids an off-by-one that would otherwise have to be got right twice, in
// two languages, on two cores.
//
// NOTE ON SCOPE: this is Slot + Play only. Record/Stop deliberately stay in
// the pause menu -- Record resets the content (it is title-anchored), which
// is not something to put one OSD click away.
//
// NO CDC HERE, and that is checked rather than assumed: rs_play is a 1-cycle
// pulse, which would be unsafe crossing to a slower domain (see the Option Y
// pulse-width race). It does not cross one -- both cores pass clk_sys as the
// video reader's `ddr_clk`, so this module and the latch that consumes the
// pulse run on the same edge. If a future core wires a genuinely separate
// DDR3 clock, this pulse needs a handshake or a widened strobe.
//
// ---------------------------------------------------------------------------
// 🛑 THE ARM BASELINE IS THE WHOLE DESIGN. READ THIS BEFORE CHANGING `armed`.
//
// The core->OSD direction is an EDGE detector on arm_seq, so it is only as
// good as the value it first compares against. Getting that baseline wrong
// does not fail loudly -- it silently rewrites the user's saved OSD slot.
//
// The first version of this module armed on the first CLOCK EDGE and its
// comment claimed that covered stale DDR3. It did not, and the gap was not
// small: arm_seq does not arrive from DDR3 until the reader's first ctrl
// read, which needs a vblank -- roughly 1.7 MILLION cycles later. Measured on
// real hardware, 0x04 held 0xAABBBFFA left by a previous core, so the module
// adopted seq=0xBF/slot=2 and pushed "Slot 3" into the OSD on EVERY core
// load, clobbering whatever the user had saved in the .cfg.
//
// So `armed` now waits for arm_valid -- the reader's "I have latched a real
// DDR3-sourced arm_seq" strobe. Because last_arm_seq tracks arm_seq on EVERY
// cycle (including while unarmed and during reset), releasing on the same
// cycle arm_valid first asserts leaves last_arm_seq holding that very first
// sample, so the next comparison is equal and nothing is adopted. Stale DDR3
// becomes the baseline instead of becoming a command.
//
// 🛑 Do NOT "simplify" this to the reader's existing `synced` flag: the
// SECOND ctrl read is equally stale, because the ARM binary is spawned about
// a second after the RBF loads and does not write 0x04 until the user first
// moves the slot. Only a real ARM publish can end that.
// ---------------------------------------------------------------------------

module replay_slot_ui
(
	input            clk,
	input            reset,

	// OSD side
	input      [2:0] status_slot,     // CONF_STR "O13,Replay Slot,1..8"
	input            OSD_play,        // CONF_STR "T9,Play Replay" (toggle bit)

	// ARM side (published by the pause menu, read out of DDR3 by the reader)
	input      [2:0] arm_slot,
	input      [7:0] arm_seq,         // ARM bumps this on every change it makes
	input            arm_valid,       // reader has latched a real DDR3 sample

	output reg       rs_play      = 1'b0,   // 1-cycle pulse
	output reg       statusUpdate = 1'b0,   // tells hps_io to write status_in back
	output     [2:0] selected_slot
);

reg [2:0] slot           = 3'd0;   // 0 == "Slot 1"; matches both cores' default
reg [2:0] lastOSDsetting = 3'd0;
reg [7:0] last_arm_seq   = 8'd0;
reg       old_play       = 1'b0;
reg       armed          = 1'b0;   // released by arm_valid, NOT by the first clock

assign selected_slot = slot;

always @(posedge clk) begin
	rs_play      <= 1'b0;
	statusUpdate <= 1'b0;

	// Track the previous sample of everything we edge-detect, EVERY cycle --
	// including while unarmed and while in reset. That is what makes the
	// baseline self-maintaining: whatever arm_seq is when we start caring, it
	// is already sitting in last_arm_seq, so starting to care cannot itself
	// look like a change.
	lastOSDsetting <= status_slot;
	last_arm_seq   <= arm_seq;
	old_play       <= OSD_play;

	if (reset) begin
		// The reader resets arm_seq to 0 on this same edge. Without dropping
		// `armed` here, last_arm_seq would still hold the pre-reset value and
		// the release of reset would read as an ARM move to slot 0 -- which
		// snapped the user's OSD slot to 1 on every reset.
		armed <= 1'b0;

		// `slot` is deliberately NOT reset. It holds the user's chosen slot,
		// which has no reason to be forgotten by a soft reset, and zeroing it
		// here would be indistinguishable from a real move to Slot 1.
	end
	else begin
		// The OSD picker moved.
		//
		// No statusUpdate on this branch, deliberately. It would be pure
		// redundancy -- the OSD is where the value just came FROM -- and it is
		// actively harmful: hps_io writes `status` in 16-bit chunks, LOW HALF
		// FIRST, and the slot lives at status[3:1] in that first chunk. Pulsing
		// one cycle later captures status_in while the HIGH half is still the
		// pre-push value, so a status write-back can revert unrelated OSD
		// settings (Aspect Ratio, Scale Mode, Swap Joysticks, Save Slot).
		// savestate_ui pulses on its equivalent branch and gets away with it
		// only because ITS bits sit at status[21:20], in the chunk written
		// SECOND -- immunity by placement, not by design. Do not copy it here.
		if (lastOSDsetting != status_slot) begin
			slot <= status_slot;
		end

		// The core's pause menu moved it -- push the new value out to the OSD.
		//
		// Checked SECOND so that if both move on the same cycle the ARM wins:
		// it is the side that just acted on a real button press, whereas the
		// OSD value is whatever was already sitting in the status word.
		if (!armed) begin
			armed <= arm_valid;
		end
		else if (last_arm_seq != arm_seq) begin
			slot         <= arm_slot;
			statusUpdate <= 1'b1;
		end

		// OSD "Play Replay" is a CONF_STR toggle bit: it flips on each
		// selection, so the event is the edge, not the level.
		if (old_play ^ OSD_play) rs_play <= 1'b1;
	end
end

endmodule
