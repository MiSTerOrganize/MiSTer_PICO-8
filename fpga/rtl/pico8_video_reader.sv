//============================================================================
//
//  PICO-8 Native Video DDR3 Reader
//
//  Reads 128x128 RGB565 frames from DDR3 and outputs 256x256 pixels
//  with 2x horizontal and 2x vertical scaling for CRT-friendly output.
//
//  Scaling:
//    Horizontal: each source pixel output twice (pixel doubling)
//    Vertical:   each source line read twice from DDR3 (line doubling)
//    128x128 source → 256x256 display
//
//  DDR3 Memory Map (physical addresses):
//    0x3A000000 + 0x000  : Control word (frame_counter[31:2], active_buffer[1:0])
//    0x3A000000 + 0x100  : Buffer 0 (128x128 RGB565 = 32,768 bytes)
//    0x3A000000 + 0x8100 : Buffer 1 (32,768 bytes)
//
//  Bandwidth: 32KB × 2 (line doubling) × 60fps = 3.7 MB/s (DDR3 can do >1000)
//
//  Adapted from 3SX project (kimchiman52/3sx-mister)
//  Copyright (C) 2026 MiSTer Organize — GPL-3.0
//
//============================================================================

module pico8_video_reader (
    // DDR3 Avalon-MM master
    input  wire        ddr_clk,
    input  wire        ddr_busy,
    output reg   [7:0] ddr_burstcnt,
    output reg  [28:0] ddr_addr,
    input  wire [63:0] ddr_dout,
    input  wire        ddr_dout_ready,
    output reg         ddr_rd,
    output reg  [63:0] ddr_din,
    output wire  [7:0] ddr_be,
    output reg         ddr_we,

    // Pixel output (clk_vid domain)
    input  wire        clk_vid,
    input  wire        ce_pix,
    input  wire        reset,

    // Timing inputs (from pico8_video_timing)
    input  wire        de,
    input  wire        hblank,
    input  wire        vblank,
    input  wire        new_frame,
    input  wire        new_line,
    input  wire  [8:0] vcount,

    // Joystick input (from hps_io, clk_sys domain = ddr_clk domain)
    input  wire [31:0] joystick_0,
    input  wire [15:0] joystick_l_analog_0,

    // Cart loading via ioctl (from hps_io)
    input  wire        ioctl_download,
    input  wire        ioctl_wr,
    input  wire [26:0] ioctl_addr,
    input  wire  [7:0] ioctl_dout,
    output wire        ioctl_wait,

    // Pixel output
    output reg   [7:0] r_out,
    output reg   [7:0] g_out,
    output reg   [7:0] b_out,

    // Control
    input  wire        enable,
    output wire        frame_ready
);

// DDR3 byte enable (always all bytes)
assign ddr_be  = 8'hFF;

// ── DDR3 Address Constants ────────────────────────────────────────────
// 29-bit qword addresses = physical >> 3
localparam [28:0] CTRL_ADDR   = 29'h07400000;  // 0x3A000000 >> 3
localparam [28:0] JOY_ADDR    = 29'h07400001;  // 0x3A000008 >> 3 (joystick data)
localparam [28:0] BUF0_ADDR   = 29'h07400020;  // 0x3A000100 >> 3
localparam [28:0] BUF1_ADDR   = 29'h07401020;  // 0x3A008100 >> 3
localparam [7:0]  LINE_BURST  = 8'd32;         // 128px * 2B / 8 = 32 beats
localparam [28:0] LINE_STRIDE = 29'd32;        // 32 qword addresses per source line
localparam [8:0]  V_ACTIVE    = 9'd256;        // display lines (2× source)
localparam [6:0]  SRC_LINES   = 7'd128;        // source lines in DDR3

localparam [19:0] TIMEOUT_MAX = 20'hF_FFFF;

// ── Enable synchronizer ──────────────────────────────────────────────
reg [1:0] enable_sync;
always @(posedge ddr_clk) begin
    if (reset)
        enable_sync <= 2'b0;
    else
        enable_sync <= {enable_sync[0], enable};
end
wire enable_ddr = enable_sync[1];

// ── CDC: new_frame ────────────────────────────────────────────────────
reg [1:0] new_frame_sync;
always @(posedge ddr_clk) begin
    if (reset)
        new_frame_sync <= 2'b0;
    else
        new_frame_sync <= {new_frame_sync[0], new_frame};
end
wire new_frame_ddr = ~new_frame_sync[1] & new_frame_sync[0];

// Latch new_frame so it can't be missed during cart writes
reg new_frame_pending;

// ── CDC: new_line ─────────────────────────────────────────────────────
reg [1:0] new_line_sync;
always @(posedge ddr_clk) begin
    if (reset)
        new_line_sync <= 2'b0;
    else
        new_line_sync <= {new_line_sync[0], new_line};
end
wire new_line_ddr = ~new_line_sync[1] & new_line_sync[0];

// ── CDC: vblank level ─────────────────────────────────────────────────
reg [1:0] vblank_sync;
always @(posedge ddr_clk) begin
    if (reset)
        vblank_sync <= 2'b0;
    else
        vblank_sync <= {vblank_sync[0], vblank};
end
wire vblank_ddr = vblank_sync[1];

// ── Reset synchronizer for clk_vid ───────────────────────────────────
reg [1:0] reset_vid_sync;
always @(posedge clk_vid or posedge reset)
    if (reset) reset_vid_sync <= 2'b11;
    else       reset_vid_sync <= {reset_vid_sync[0], 1'b0};
wire reset_vid = reset_vid_sync[1];

// ── CDC: frame_ready ──────────────────────────────────────────────────
reg frame_ready_reg;
reg [1:0] frame_ready_sync;
always @(posedge clk_vid) begin
    if (reset_vid)
        frame_ready_sync <= 2'b0;
    else
        frame_ready_sync <= {frame_ready_sync[0], frame_ready_reg};
end
wire frame_ready_vid = frame_ready_sync[1];
assign frame_ready = frame_ready_vid;

// ── DDR3 Read State Machine ──────────────────────────────────────────
localparam [3:0] ST_IDLE         = 4'd0;
localparam [3:0] ST_POLL_CTRL    = 4'd1;
localparam [3:0] ST_WAIT_CTRL    = 4'd2;
localparam [3:0] ST_CHECK_CTRL   = 4'd3;
localparam [3:0] ST_READ_LINE    = 4'd4;
localparam [3:0] ST_WAIT_LINE    = 4'd5;
localparam [3:0] ST_LINE_DONE    = 4'd6;
localparam [3:0] ST_WAIT_DISPLAY = 4'd7;
localparam [3:0] ST_WRITE_JOY   = 4'd8;
localparam [3:0] ST_WRITE_CART  = 4'd9;
localparam [3:0] ST_WRITE_CART_SIZE = 4'd10;

// Cart loading DDR3 addresses
localparam [28:0] CART_CTRL_ADDR = 29'h07400002;  // 0x3A000010 >> 3
localparam [28:0] CART_DATA_ADDR = 29'h07404000;  // 0x3A020000 >> 3 (past video buffers)

reg  [3:0]  state;
reg  [31:0] ctrl_word;
reg  [29:0] prev_frame_counter;
reg         active_buffer;
reg  [28:0] buf_base_addr;
reg  [8:0]  display_line;     // 0..255 (output display line)
reg  [6:0]  beat_count;
reg         first_frame_loaded;
reg  [4:0]  stale_vblank_count;
reg         preloading;
reg  [19:0] timeout_cnt;

// Cart loading registers
reg  [63:0] cart_buf;
reg   [2:0] cart_byte_cnt;
reg         cart_write_pending;
reg  [28:0] cart_write_addr;
reg  [63:0] cart_write_data;
reg         cart_size_pending;
reg  [26:0] cart_total_bytes;
reg         cart_dl_prev;
reg         cart_loading;

assign ioctl_wait = cart_write_pending & ioctl_download;

// Source line = display_line / 2 (vertical doubling)
wire [6:0] source_line = display_line[8:1];

// ── FIFO write signals ───────────────────────────────────────────────
reg         fifo_wr;
reg  [63:0] fifo_wr_data;
wire        fifo_full;

// ── FIFO async clear ─────────────────────────────────────────────────
reg [3:0] fifo_aclr_cnt;
wire fifo_aclr_ddr_active = (fifo_aclr_cnt != 4'd0);
wire fifo_aclr = reset | fifo_aclr_ddr_active;

// ── Main state machine ───────────────────────────────────────────────
always @(posedge ddr_clk) begin
    if (reset) begin
        state              <= ST_IDLE;
        ddr_rd             <= 1'b0;
        ddr_we             <= 1'b0;
        ddr_din            <= 64'd0;
        ddr_burstcnt       <= 8'd1;
        ddr_addr           <= 29'd0;
        ctrl_word          <= 32'd0;
        prev_frame_counter <= 30'd0;
        active_buffer      <= 1'b0;
        buf_base_addr      <= 29'd0;
        display_line       <= 9'd0;
        beat_count         <= 7'd0;
        first_frame_loaded <= 1'b0;
        frame_ready_reg    <= 1'b0;
        stale_vblank_count <= 5'd0;
        preloading         <= 1'b0;
        timeout_cnt        <= 20'd0;
        fifo_wr            <= 1'b0;
        fifo_wr_data       <= 64'd0;
        fifo_aclr_cnt      <= 4'd0;
        cart_buf            <= 64'd0;
        cart_byte_cnt       <= 3'd0;
        cart_write_pending  <= 1'b0;
        cart_write_addr     <= 29'd0;
        cart_write_data     <= 64'd0;
        cart_size_pending   <= 1'b0;
        cart_total_bytes    <= 27'd0;
        cart_dl_prev        <= 1'b0;
        cart_loading        <= 1'b0;
        new_frame_pending   <= 1'b0;
    end
    else begin
        fifo_wr <= 1'b0;
        if (fifo_aclr_cnt != 4'd0) fifo_aclr_cnt <= fifo_aclr_cnt - 4'd1;
        if (!ddr_busy) ddr_rd <= 1'b0;
        if (!ddr_busy) ddr_we <= 1'b0;

        // Latch new_frame pulse so cart writes can't cause it to be missed
        if (new_frame_ddr) new_frame_pending <= 1'b1;

        // Beat capture (runs in parallel with state machine)
        if (state == ST_WAIT_LINE && ddr_dout_ready) begin
            fifo_wr      <= 1'b1;
            fifo_wr_data <= ddr_dout;
            beat_count   <= beat_count + 7'd1;
            timeout_cnt  <= 20'd0;
        end

        // ── Cart byte collection (runs in parallel) ──────────────
        cart_dl_prev <= ioctl_download;

        // Download start
        if (ioctl_download && !cart_dl_prev) begin
            cart_loading    <= 1'b1;
            cart_byte_cnt   <= 3'd0;
            cart_buf        <= 64'd0;
            cart_total_bytes <= 27'd0;
        end

        // Collect bytes
        if (ioctl_download && ioctl_wr && !cart_write_pending) begin
            case (cart_byte_cnt)
                3'd0: cart_buf[ 7: 0] <= ioctl_dout;
                3'd1: cart_buf[15: 8] <= ioctl_dout;
                3'd2: cart_buf[23:16] <= ioctl_dout;
                3'd3: cart_buf[31:24] <= ioctl_dout;
                3'd4: cart_buf[39:32] <= ioctl_dout;
                3'd5: cart_buf[47:40] <= ioctl_dout;
                3'd6: cart_buf[55:48] <= ioctl_dout;
                3'd7: cart_buf[63:56] <= ioctl_dout;
            endcase
            cart_total_bytes <= ioctl_addr + 27'd1;

            if (cart_byte_cnt == 3'd7) begin
                cart_write_pending <= 1'b1;
                cart_write_addr   <= CART_DATA_ADDR + {2'd0, ioctl_addr[26:3]};
                cart_write_data   <= {ioctl_dout, cart_buf[55:0]};
                cart_byte_cnt     <= 3'd0;
            end
            else begin
                cart_byte_cnt <= cart_byte_cnt + 3'd1;
            end
        end

        // Download end — flush partial + write size
        if (!ioctl_download && cart_dl_prev && cart_loading) begin
            cart_loading <= 1'b0;
            cart_size_pending <= 1'b1;
            if (cart_byte_cnt != 3'd0 && !cart_write_pending) begin
                cart_write_pending <= 1'b1;
                cart_write_addr   <= CART_DATA_ADDR + {2'd0, cart_total_bytes[26:3]};
                cart_write_data   <= cart_buf;
                cart_byte_cnt     <= 3'd0;
            end
        end

        case (state)
            ST_IDLE: begin
                if (ioctl_download) begin
                    // During cart file transfer, only handle cart writes.
                    // Video reading is paused — display holds the last frame.
                    // This prevents DDR3 contention between cart writes and video reads.
                    if (cart_write_pending)
                        state <= ST_WRITE_CART;
                    // Clear new_frame_pending so it doesn't accumulate
                    new_frame_pending <= 1'b0;
                end
                else begin
                    // Normal operation: frame cycle takes priority
                    if (enable_ddr && new_frame_pending)
                        state <= ST_WRITE_JOY;
                    else if (cart_write_pending)
                        state <= ST_WRITE_CART;
                    else if (cart_size_pending)
                        state <= ST_WRITE_CART_SIZE;
                end
            end

            ST_WRITE_JOY: begin
                // Write joystick_0 to DDR3 so ARM can read it
                if (!ddr_busy) begin
                    ddr_addr     <= JOY_ADDR;
                    ddr_din      <= {32'd0, joystick_0};
                    ddr_burstcnt <= 8'd1;
                    ddr_we       <= 1'b1;
                    new_frame_pending <= 1'b0;  // consumed
                    state        <= ST_POLL_CTRL;
                end
            end

            ST_WRITE_CART: begin
                // Write 8 bytes of cart data to DDR3
                if (!ddr_busy) begin
                    ddr_addr         <= cart_write_addr;
                    ddr_din          <= cart_write_data;
                    ddr_burstcnt     <= 8'd1;
                    ddr_we           <= 1'b1;
                    cart_write_pending <= 1'b0;
                    cart_buf         <= 64'd0;
                    // If download ended and this was the flush, write size next
                    if (!cart_loading && cart_size_pending)
                        state <= ST_WRITE_CART_SIZE;
                    else
                        state <= ST_IDLE;
                end
            end

            ST_WRITE_CART_SIZE: begin
                // Write file size to cart control address
                if (!ddr_busy) begin
                    ddr_addr         <= CART_CTRL_ADDR;
                    ddr_din          <= {32'd0, 5'd0, cart_total_bytes};
                    ddr_burstcnt     <= 8'd1;
                    ddr_we           <= 1'b1;
                    cart_size_pending <= 1'b0;
                    state            <= ST_IDLE;
                end
            end

            ST_POLL_CTRL: begin
                if (!ddr_busy) begin
                    ddr_addr     <= CTRL_ADDR;
                    ddr_burstcnt <= 8'd1;
                    ddr_rd       <= 1'b1;
                    timeout_cnt  <= 20'd0;
                    state        <= ST_WAIT_CTRL;
                end
            end

            ST_WAIT_CTRL: begin
                if (ddr_dout_ready) begin
                    ctrl_word   <= ddr_dout[31:0];
                    timeout_cnt <= 20'd0;
                    state       <= ST_CHECK_CTRL;
                end
                else if (timeout_cnt == TIMEOUT_MAX)
                    state <= ST_IDLE;
                else
                    timeout_cnt <= timeout_cnt + 20'd1;
            end

            ST_CHECK_CTRL: begin
                if (ctrl_word[31:2] != prev_frame_counter) begin
                    // New frame available
                    prev_frame_counter <= ctrl_word[31:2];
                    active_buffer      <= ctrl_word[0];
                    stale_vblank_count <= 5'd0;
                    buf_base_addr      <= ctrl_word[0] ? BUF1_ADDR : BUF0_ADDR;
                    display_line       <= 9'd0;
                    preloading         <= 1'b1;
                    fifo_aclr_cnt      <= 4'd8;
                    state              <= ST_READ_LINE;
                end
                else if (first_frame_loaded) begin
                    // Stale frame — re-read previous buffer
                    if (stale_vblank_count < 5'd30)
                        stale_vblank_count <= stale_vblank_count + 5'd1;
                    if (stale_vblank_count >= 5'd29)
                        frame_ready_reg <= 1'b0;
                    display_line  <= 9'd0;
                    preloading    <= 1'b1;
                    fifo_aclr_cnt <= 4'd8;
                    state         <= ST_READ_LINE;
                end
                else
                    state <= ST_IDLE;
            end

            ST_READ_LINE: begin
                if (!ddr_busy && !fifo_aclr_ddr_active) begin
                    // source_line = display_line[8:1] gives vertical doubling:
                    // display lines 0,1 → source line 0
                    // display lines 2,3 → source line 1, etc.
                    ddr_addr     <= buf_base_addr + ({22'd0, source_line} * LINE_STRIDE);
                    ddr_burstcnt <= LINE_BURST;
                    ddr_rd       <= 1'b1;
                    beat_count   <= 7'd0;
                    timeout_cnt  <= 20'd0;
                    state        <= ST_WAIT_LINE;
                end
            end

            ST_WAIT_LINE: begin
                if (beat_count == LINE_BURST[6:0])
                    state <= ST_LINE_DONE;
                else if (timeout_cnt == TIMEOUT_MAX)
                    state <= ST_IDLE;
                else if (!ddr_dout_ready)
                    timeout_cnt <= timeout_cnt + 20'd1;
            end

            ST_LINE_DONE: begin
                display_line <= display_line + 9'd1;

                if (display_line == V_ACTIVE - 9'd1) begin
                    first_frame_loaded <= 1'b1;
                    frame_ready_reg    <= 1'b1;
                    preloading         <= 1'b0;
                    state              <= ST_IDLE;
                end
                else if (preloading && display_line < 9'd1)
                    state <= ST_READ_LINE;
                else begin
                    preloading <= 1'b0;
                    state      <= ST_WAIT_DISPLAY;
                end
            end

            ST_WAIT_DISPLAY: begin
                if (display_line < V_ACTIVE && new_line_ddr && !vblank_ddr)
                    state <= ST_READ_LINE;
            end

            default: state <= ST_IDLE;
        endcase
    end
end

// ── Dual-Clock FIFO ──────────────────────────────────────────────────
// 64-bit wide, stores raw DDR3 beats (4 RGB565 pixels per entry)
// Depth 64: holds 2 scanlines (32 beats/line × 2)
wire [63:0] fifo_rd_data;
wire        fifo_empty;
reg         fifo_rd;

dcfifo #(
    .intended_device_family ("Cyclone V"),
    .lpm_numwords           (64),
    .lpm_showahead          ("ON"),
    .lpm_type               ("dcfifo"),
    .lpm_width              (64),
    .lpm_widthu             (6),
    .overflow_checking      ("ON"),
    .rdsync_delaypipe       (4),
    .underflow_checking     ("ON"),
    .use_eab                ("ON"),
    .wrsync_delaypipe       (4)
) line_fifo (
    .aclr     (fifo_aclr),
    .data     (fifo_wr_data),
    .rdclk    (clk_vid),
    .rdreq    (fifo_rd),
    .wrclk    (ddr_clk),
    .wrreq    (fifo_wr),
    .q        (fifo_rd_data),
    .rdempty  (fifo_empty),
    .wrfull   (fifo_full),
    .eccstatus(),
    .rdfull   (),
    .rdusedw  (),
    .wrempty  (),
    .wrusedw  ()
);

// ── Pixel Output with 2× Horizontal Doubling ─────────────────────────
//
// Each 64-bit FIFO word = 4 source pixels (RGB565).
// With 2× horizontal doubling, each source pixel outputs twice,
// so each word produces 8 display pixels.
//
// pixel_sub[1:0] selects which of the 4 source pixels (0..3)
// pixel_phase toggles 0→1 between first/second copy of each pixel
//
reg  [63:0] pixel_word;
reg  [1:0]  pixel_sub;
reg         pixel_phase;      // 0=first copy, 1=second copy
reg         pixel_word_valid;

// RGB565 decode from current sub-pixel
wire [15:0] cur_pix = pixel_word[{pixel_sub, 4'b0000} +: 16];
wire  [7:0] dec_r = {cur_pix[15:11], cur_pix[15:13]};
wire  [7:0] dec_g = {cur_pix[10:5],  cur_pix[10:9]};
wire  [7:0] dec_b = {cur_pix[4:0],   cur_pix[4:2]};

always @(posedge clk_vid) begin
    if (reset_vid) begin
        fifo_rd          <= 1'b0;
        r_out            <= 8'd0;
        g_out            <= 8'd0;
        b_out            <= 8'd0;
        pixel_word       <= 64'd0;
        pixel_sub        <= 2'd0;
        pixel_phase      <= 1'b0;
        pixel_word_valid <= 1'b0;
    end
    else begin
        fifo_rd <= 1'b0;

        if (ce_pix) begin
            if (de && frame_ready_vid) begin
                if (pixel_word_valid) begin
                    // Output current pixel
                    r_out <= dec_r;
                    g_out <= dec_g;
                    b_out <= dec_b;

                    if (pixel_phase == 1'b0) begin
                        // First copy done — output same pixel again next cycle
                        pixel_phase <= 1'b1;
                    end
                    else begin
                        // Second copy done — advance to next source pixel
                        pixel_phase <= 1'b0;

                        if (pixel_sub == 2'd3) begin
                            // Word exhausted — load next from FIFO
                            pixel_word_valid <= 1'b0;
                            if (!fifo_empty) begin
                                pixel_word       <= fifo_rd_data;
                                pixel_word_valid <= 1'b1;
                                pixel_sub        <= 2'd0;
                                fifo_rd          <= 1'b1;
                            end
                        end
                        else begin
                            pixel_sub <= pixel_sub + 2'd1;
                        end
                    end
                end
                else if (!fifo_empty) begin
                    // Load first word from FIFO (show-ahead)
                    pixel_word       <= fifo_rd_data;
                    pixel_word_valid <= 1'b1;
                    pixel_sub        <= 2'd0;
                    pixel_phase      <= 1'b0;
                    fifo_rd          <= 1'b1;
                    // Output first pixel immediately
                    r_out <= {fifo_rd_data[15:11], fifo_rd_data[15:13]};
                    g_out <= {fifo_rd_data[10:5],  fifo_rd_data[10:9]};
                    b_out <= {fifo_rd_data[4:0],   fifo_rd_data[4:2]};
                end
                else begin
                    r_out <= 8'd0;
                    g_out <= 8'd0;
                    b_out <= 8'd0;
                end
            end
            else begin
                // Outside active display
                r_out            <= 8'd0;
                g_out            <= 8'd0;
                b_out            <= 8'd0;
                pixel_sub        <= 2'd0;
                pixel_phase      <= 1'b0;
                pixel_word_valid <= 1'b0;
            end
        end
    end
end

endmodule
