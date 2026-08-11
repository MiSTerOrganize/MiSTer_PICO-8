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

module replay_slot_ui
(
	input            clk,

	// OSD side
	input      [2:0] status_slot,     // CONF_STR "O13,Replay Slot,1..8"
	input            OSD_play,        // CONF_STR "T9,Play Replay" (toggle bit)

	// ARM side (published by the pause menu, read out of DDR3 by the reader)
	input      [2:0] arm_slot,
	input      [7:0] arm_seq,         // ARM bumps this on every change it makes

	output reg       rs_play,         // 1-cycle pulse
	output reg       statusUpdate,    // tells hps_io to write status_in back
	output     [2:0] selected_slot
);

reg [2:0] slot           = 3'd0;   // 0 == "Slot 1"; matches both cores' default
reg [2:0] lastOSDsetting = 3'd0;
reg [7:0] last_arm_seq   = 8'd0;
reg       old_play       = 1'b0;
reg       armed          = 1'b0;   // ignore the first arm_seq sample after reset

assign selected_slot = slot;

always @(posedge clk) begin
	rs_play      <= 1'b0;
	statusUpdate <= 1'b0;

	lastOSDsetting <= status_slot;
	last_arm_seq   <= arm_seq;

	// The OSD picker moved.
	if (lastOSDsetting != status_slot) begin
		slot         <= status_slot;
		statusUpdate <= 1'b1;
	end

	// The core's pause menu moved it -- push the new value out to the OSD.
	//
	// Checked SECOND so that if both move on the same cycle the ARM wins: it
	// is the side that just acted on a real button press, whereas the OSD
	// value is whatever was already sitting in the status word.
	//
	// `armed` suppresses the very first comparison after reset. DDR3 holds
	// whatever the previous core left there, so an unguarded first sample can
	// read a stale non-zero seq and yank the slot to a garbage value before
	// the ARM has written anything.
	if (!armed) begin
		armed <= 1'b1;
	end
	else if (last_arm_seq != arm_seq) begin
		slot         <= arm_slot;
		statusUpdate <= 1'b1;
	end

	// OSD "Play Replay" is a CONF_STR toggle bit: it flips on each selection,
	// so the event is the edge, not the level.
	old_play <= OSD_play;
	if (old_play ^ OSD_play) rs_play <= 1'b1;
end

endmodule
