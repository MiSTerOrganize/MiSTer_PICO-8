//============================================================================
//
//  PICO-8 Native Video Top-Level Wrapper
//
//  Instantiates the timing generator and DDR3 reader, providing a clean
//  interface for integration into menu.sv (or a standalone PICO-8 core).
//
//  Runs on CLK_VIDEO (31.25 MHz) with integer divide-by-4 ce_pix for
//  7.8125 MHz effective pixel rate. Same PLL as 3SX.
//
//  Adapted from 3SX project (kimchiman52/3sx-mister)
//  Copyright (C) 2026 MiSTer Organize — GPL-3.0
//
//============================================================================

module pico8_video_top (
    input  wire        clk_sys,       // system clock (100 MHz) for DDR3
    input  wire        clk_vid,       // video clock (31.25 MHz, CLK_VIDEO)
    input  wire        ce_pix,        // pixel enable (divide-by-4 = 7.8125 MHz)
    input  wire        reset,

    // DDR3 Avalon-MM master
    input  wire        ddr_busy,
    output wire  [7:0] ddr_burstcnt,
    output wire [28:0] ddr_addr,
    input  wire [63:0] ddr_dout,
    input  wire        ddr_dout_ready,
    output wire        ddr_rd,
    output wire [63:0] ddr_din,
    output wire  [7:0] ddr_be,
    output wire        ddr_we,

    // Video output (clk_vid domain)
    output wire  [7:0] vga_r,
    output wire  [7:0] vga_g,
    output wire  [7:0] vga_b,
    output wire        vga_hs,
    output wire        vga_vs,
    output wire        vga_de,

    // Control
    input  wire        enable,        // from ARM: activate native video
    output wire        active,        // module is outputting valid video
    output wire        vsync_out,     // active-low vsync for frame sync

    // Audio output (48KHz signed 16-bit, clk_audio domain via FIFO)
    input  wire        clk_audio,
    output wire [15:0] audio_l,
    output wire [15:0] audio_r,

    // Joystick (from hps_io, written to DDR3 for ARM)
    input  wire [31:0] joystick_0,
    input  wire [15:0] joystick_l_analog_0,

    // Cart loading via ioctl
    input  wire        ioctl_download,
    input  wire        ioctl_wr,
    input  wire [26:0] ioctl_addr,
    input  wire  [7:0] ioctl_dout,
    output wire        ioctl_wait
);

// ── Timing Generator ──────────────────────────────────────────────────
wire        tim_hsync, tim_vsync;
wire        tim_hblank, tim_vblank;
wire        tim_de;
wire [9:0]  tim_hcount;
wire [8:0]  tim_vcount;
wire        tim_new_frame, tim_new_line;

pico8_video_timing timing (
    .clk       (clk_vid),
    .ce_pix    (ce_pix),
    .reset     (reset),
    .hsync     (tim_hsync),
    .vsync     (tim_vsync),
    .hblank    (tim_hblank),
    .vblank    (tim_vblank),
    .de        (tim_de),
    .hcount    (tim_hcount),
    .vcount    (tim_vcount),
    .new_frame (tim_new_frame),
    .new_line  (tim_new_line)
);

// ── DDR3 Pixel Reader ─────────────────────────────────────────────────
wire [7:0]  reader_r, reader_g, reader_b;
wire        reader_frame_ready;
wire [15:0] reader_audio_l, reader_audio_r;

// Image area: 256px centered in 320px active (32px border each side)
wire in_image = (tim_hcount >= 10'd32) && (tim_hcount < 10'd288);
// Only drain FIFO during the image area, not during borders
wire image_de = tim_de & in_image;

pico8_video_reader reader (
    .ddr_clk        (clk_sys),
    .ddr_busy       (ddr_busy),
    .ddr_burstcnt   (ddr_burstcnt),
    .ddr_addr       (ddr_addr),
    .ddr_dout       (ddr_dout),
    .ddr_dout_ready (ddr_dout_ready),
    .ddr_rd         (ddr_rd),
    .ddr_din        (ddr_din),
    .ddr_be         (ddr_be),
    .ddr_we         (ddr_we),

    .clk_vid        (clk_vid),
    .ce_pix         (ce_pix),
    .reset          (reset),

    .de             (image_de),
    .hblank         (tim_hblank),
    .vblank         (tim_vblank),
    .new_frame      (tim_new_frame),
    .new_line       (tim_new_line),
    .vcount         (tim_vcount),

    .r_out          (reader_r),
    .g_out          (reader_g),
    .b_out          (reader_b),

    .clk_audio      (clk_audio),
    .audio_l_out    (reader_audio_l),
    .audio_r_out    (reader_audio_r),

    .enable         (enable),
    .frame_ready    (reader_frame_ready),

    .joystick_0     (joystick_0),
    .joystick_l_analog_0 (joystick_l_analog_0),

    .ioctl_download (ioctl_download),
    .ioctl_wr       (ioctl_wr),
    .ioctl_addr     (ioctl_addr),
    .ioctl_dout     (ioctl_dout),
    .ioctl_wait     (ioctl_wait)
);

// ── Output assignments ────────────────────────────────────────────────
assign vga_r     = (in_image && reader_frame_ready) ? reader_r : 8'd0;
assign vga_g     = (in_image && reader_frame_ready) ? reader_g : 8'd0;
assign vga_b     = (in_image && reader_frame_ready) ? reader_b : 8'd0;
assign vga_hs    = tim_hsync;
assign vga_vs    = tim_vsync;
assign vga_de    = tim_de;
assign active    = enable & reader_frame_ready;
assign vsync_out = tim_vsync;
assign audio_l   = reader_audio_l;
assign audio_r   = reader_audio_r;

endmodule
