//============================================================================
//
//  PICO-8 Native Video Timing Generator
//
//  256x256 active area @ 60.1 Hz (341x262 total)
//  Exact NES timing — NTSC-derived pixel clock from colorburst crystal.
//  CLK_VIDEO: 21.477 MHz, CE_PIXEL: divide-by-4 (5.36932 MHz effective)
//
//  H: 256 active + 15 FP + 25 sync + 45 BP = 341 total
//  V: 256 active +  1 FP +  3 sync +  2 BP = 262 total
//
//  Refresh: 5,369,318 / (341*262) = 60.10 Hz (exact NES)
//  H freq:  5,369,318 / 341       = 15,746 Hz (exact NES)
//  H active time: 256/5.369MHz = 47.68 µs (exact NES/SNES/Genesis)
//
//  The 128x128 PICO-8 image is doubled to 256x256. H_ACTIVE=256
//  matches the content exactly — no borders.
//
//  Adapted from 3SX project (kimchiman52/3sx-mister)
//  Copyright (C) 2026 MiSTer Organize — GPL-3.0
//
//============================================================================

module pico8_video_timing (
    input  wire        clk,        // CLK_VIDEO (21.477 MHz)
    input  wire        ce_pix,     // pixel enable (divide-by-4 = 5.369 MHz, exact NES)
    input  wire        reset,

    // CRT position offset (signed: -3 to +3, from OSD)
    input  wire signed [4:0] h_adj,  // horizontal: positive = shift right
    input  wire signed [3:0] v_adj,  // vertical: positive = shift down

    output reg         hsync,      // active low
    output reg         vsync,      // active low
    output reg         hblank,
    output reg         vblank,
    output reg         de,         // data enable = ~(hblank | vblank)
    output reg  [9:0]  hcount,
    output reg  [8:0]  vcount,
    output reg         new_frame,  // pulse at vblank start
    output reg         new_line    // pulse at hblank start
);

// ── Timing constants ──────────────────────────────────────────────────
// Byte-for-byte match with the MiSTer NES core's default output
// (hide_overscan=2'b00) — see rtl/video.sv in NES_MiSTer.
// H: 256 active + 21 FP + 25 sync + 39 BP = 341 (47.68 µs active, 15,746 Hz)
// V: 224 active + 13 FP +  3 sync + 22 BP = 262 (60.10 Hz, 38 lines blanking)
// Source 128×128 is doubled horizontally (256) and scaled 1.75× vertically
// (224 via Bresenham 4/7) inside the reader. Pixel aspect 8:7 matches
// NES/SNES proportions on a TV.
localparam H_ACTIVE = 256;
localparam H_FP     = 21;
localparam H_SYNC   = 25;
localparam H_BP     = 39;
localparam H_TOTAL  = 341;   // 256+21+25+39 (NES exact)

localparam V_ACTIVE = 224;
localparam V_FP     = 13;
localparam V_SYNC   = 3;
localparam V_BP     = 22;
localparam V_TOTAL  = 262;   // 224+13+3+22 = 60.10 Hz (NES exact)

// Derived boundaries — adjusted by OSD H/V position offset.
// Each step shifts sync by 4 pixels (H) or 1 line (V), moving FP/BP balance.
wire [9:0] h_sync_start = H_ACTIVE + H_FP + {{5{h_adj[4]}}, h_adj};
wire [9:0] h_sync_end   = h_sync_start + H_SYNC;
wire [8:0] v_sync_start = V_ACTIVE + V_FP + {{5{v_adj[3]}}, v_adj};
wire [8:0] v_sync_end   = v_sync_start + V_SYNC;

always @(posedge clk) begin
    if (reset) begin
        hcount    <= 10'd0;
        vcount    <= 9'd0;
        hsync     <= 1'b1;
        vsync     <= 1'b1;
        hblank    <= 1'b0;
        vblank    <= 1'b0;
        de        <= 1'b1;
        new_frame <= 1'b0;
        new_line  <= 1'b0;
    end
    else if (ce_pix) begin
        new_frame <= 1'b0;
        new_line  <= 1'b0;

        // Horizontal counter
        if (hcount == H_TOTAL - 1) begin
            hcount <= 10'd0;
            if (vcount == V_TOTAL - 1)
                vcount <= 9'd0;
            else
                vcount <= vcount + 9'd1;
        end
        else begin
            hcount <= hcount + 10'd1;
        end

        // Horizontal blanking
        if (hcount == H_ACTIVE - 1)
            hblank <= 1'b1;
        else if (hcount == H_TOTAL - 1)
            hblank <= 1'b0;

        // Horizontal sync (active low)
        if (hcount == h_sync_start - 1)
            hsync <= 1'b0;
        else if (hcount == h_sync_end - 1)
            hsync <= 1'b1;

        // Vertical blanking (transitions on line boundaries)
        if (hcount == H_TOTAL - 1) begin
            if (vcount == V_ACTIVE - 1)
                vblank <= 1'b1;
            else if (vcount == V_TOTAL - 1)
                vblank <= 1'b0;
        end

        // Vertical sync (active low)
        if (hcount == H_TOTAL - 1) begin
            if (vcount == v_sync_start - 1)
                vsync <= 1'b0;
            else if (vcount == v_sync_end - 1)
                vsync <= 1'b1;
        end

        // New line pulse
        if (hcount == H_ACTIVE - 1)
            new_line <= 1'b1;

        // New frame pulse
        if (hcount == H_TOTAL - 1 && vcount == V_ACTIVE - 1)
            new_frame <= 1'b1;

        // Data enable (combinational from next-cycle blanking state)
        begin
            reg next_hblank, next_vblank;

            if (hcount == H_ACTIVE - 1)
                next_hblank = 1'b1;
            else if (hcount == H_TOTAL - 1)
                next_hblank = 1'b0;
            else
                next_hblank = hblank;

            if (hcount == H_TOTAL - 1) begin
                if (vcount == V_ACTIVE - 1)
                    next_vblank = 1'b1;
                else if (vcount == V_TOTAL - 1)
                    next_vblank = 1'b0;
                else
                    next_vblank = vblank;
            end
            else
                next_vblank = vblank;

            de <= ~next_hblank & ~next_vblank;
        end
    end
end

endmodule
