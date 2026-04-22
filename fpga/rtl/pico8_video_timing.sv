//============================================================================
//
//  PICO-8 Native Video Timing Generator
//
//  320x256 active area @ ~58.7 Hz (500x266 total)
//  CRT-compatible blanking with balanced porches.
//  CLK_VIDEO: 31.25 MHz, CE_PIXEL: divide-by-4 (7.8125 MHz effective)
//
//  H: 320 active + 36 FP + 32 sync + 112 BP = 500 total
//  V: 256 active +  3 FP +  3 sync +   4 BP = 266 total
//
//  Refresh: 7,812,500 / (500*266) = 58.7 Hz
//  H freq:  7,812,500 / 500       = 15,625 Hz (CRT-safe)
//
//  The 128x128 PICO-8 image is doubled to 256x256 and centered
//  horizontally in the 320-pixel active area (32px black each side).
//
//  Adapted from 3SX project (kimchiman52/3sx-mister)
//  Copyright (C) 2026 MiSTer Organize — GPL-3.0
//
//============================================================================

module pico8_video_timing (
    input  wire        clk,        // CLK_VIDEO (31.25 MHz)
    input  wire        ce_pix,     // pixel enable (divide-by-4 = 7.8125 MHz)
    input  wire        reset,

    // CRT position offset (signed: -3 to +3, from OSD)
    input  wire signed [3:0] h_adj,  // horizontal: positive = shift right
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
// 320×256 active, CRT-compatible blanking.
// H: balanced porches (Genesis-like proportions, not extreme BP)
// V: enough blanking for CRT raster repositioning (V_BP was 2 = too tight)
localparam H_ACTIVE = 320;
localparam H_FP     = 36;
localparam H_SYNC   = 32;
localparam H_BP     = 112;
localparam H_TOTAL  = 500;   // 320+36+32+112

localparam V_ACTIVE = 256;
localparam V_FP     = 3;
localparam V_SYNC   = 3;
localparam V_BP     = 4;
localparam V_TOTAL  = 266;   // 256+3+3+4 = 58.7 Hz

// Derived boundaries — adjusted by OSD H/V position offset.
// Each step shifts sync by 4 pixels (H) or 1 line (V), moving FP/BP balance.
wire [9:0] h_sync_start = H_ACTIVE + H_FP + {{6{h_adj[3]}}, h_adj};
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
