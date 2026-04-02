//============================================================================
//
//  PICO-8 Native Video Timing Generator
//
//  256x256 active area @ ~59.64 Hz (500x262 total)
//  CLK_VIDEO: 31.25 MHz, CE_PIXEL: divide-by-4 (7.8125 MHz effective)
//
//  H: 256 active + 52 FP + 38 sync + 154 BP = 500 total
//  V: 256 active +  2 FP +  3 sync +   1 BP = 262 total
//
//  Refresh: 7,812,500 / (500*262) = 59.637 Hz
//  H freq:  7,812,500 / 500       = 15,625 Hz (NTSC-compatible)
//
//  NOTE: V blanking is only 6 lines (tight). Works on most CRTs and all
//  HDMI displays. If a specific CRT loses sync, increase V_BP and reduce
//  V_ACTIVE to 240 (losing 8 rows top/bottom from the PICO-8 image).
//
//  Reuses 3SX's PLL: 50 MHz * 5/8 = 31.25 MHz, /4 = 7.8125 MHz pixels.
//
//  Adapted from 3SX project (kimchiman52/3sx-mister)
//  Copyright (C) 2026 MiSTer Organize — GPL-3.0
//
//============================================================================

module pico8_video_timing (
    input  wire        clk,        // CLK_VIDEO (31.25 MHz)
    input  wire        ce_pix,     // pixel enable (divide-by-4 = 7.8125 MHz)
    input  wire        reset,

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
// 256×256 active, centered for 15kHz CRT
localparam H_ACTIVE = 256;
localparam H_FP     = 52;
localparam H_SYNC   = 38;
localparam H_BP     = 154;
localparam H_TOTAL  = 500;   // 256+52+38+154

localparam V_ACTIVE = 256;
localparam V_FP     = 2;
localparam V_SYNC   = 3;
localparam V_BP     = 1;
localparam V_TOTAL  = 262;   // 256+2+3+1

// Derived boundaries
localparam H_SYNC_START = H_ACTIVE + H_FP;        // 308
localparam H_SYNC_END   = H_SYNC_START + H_SYNC;  // 346
localparam V_SYNC_START = V_ACTIVE + V_FP;         // 258
localparam V_SYNC_END   = V_SYNC_START + V_SYNC;   // 261

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
        if (hcount == H_SYNC_START - 1)
            hsync <= 1'b0;
        else if (hcount == H_SYNC_END - 1)
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
            if (vcount == V_SYNC_START - 1)
                vsync <= 1'b0;
            else if (vcount == V_SYNC_END - 1)
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
