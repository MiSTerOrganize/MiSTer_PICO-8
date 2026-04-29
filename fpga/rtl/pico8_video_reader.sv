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
//    128x128 source -> 256x256 display
//
//  DDR3 Memory Map (physical addresses):
//    0x3A000000 + 0x000    : Control word (frame_counter[31:2], active_buffer[1:0])
//    0x3A000000 + 0x008    : Joystick data (FPGA writes, ARM reads)
//    0x3A000000 + 0x010    : Cart control (file_size, ARM polls)
//    0x3A000000 + 0x018    : VSync feedback (vblank_counter[31:2], buffer_status[1:0])
//    0x3A000000 + 0x020    : Audio WPTR (ARM writes, FPGA reads)
//    0x3A000000 + 0x028    : Audio RPTR (FPGA writes, ARM reads)
//    0x3A000000 + 0x100    : Buffer 0 (128x128 RGB565 = 32,768 bytes)
//    0x3A000000 + 0x8100   : Buffer 1 (32,768 bytes)
//    0x3A000000 + 0x10200  : Audio ring buffer (4096 stereo samples = 16KB)
//
//  Bandwidth: 32KB x 2 (line doubling) x 60fps = 3.7 MB/s (DDR3 can do >1000)
//
//  Adapted from 3SX project (kimchiman52/3sx-mister)
//  Copyright (C) 2026 MiSTer Organize -- GPL-3.0
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

    // Cart loading via ioctl (from hps_io)
    input  wire        ioctl_download,
    input  wire        ioctl_wr,
    input  wire [26:0] ioctl_addr,
    input  wire  [7:0] ioctl_dout,
    output wire        ioctl_wait,

    // Joystick inputs (P1-P4 from hps_io, clk_sys domain = ddr_clk domain)
    input  wire [31:0] joystick_0,
    input  wire [31:0] joystick_1,
    input  wire [31:0] joystick_2,
    input  wire [31:0] joystick_3,
    input  wire [15:0] joystick_l_analog_0,

    // Pixel output
    output reg   [7:0] r_out,
    output reg   [7:0] g_out,
    output reg   [7:0] b_out,

    // Audio output (48KHz, signed 16-bit, clk_audio domain)
    input  wire        clk_audio,
    output reg  [15:0] audio_l_out,
    output reg  [15:0] audio_r_out,

    // Control
    input  wire        enable,
    output wire        frame_ready
);

// DDR3 byte enable (always all bytes)
assign ddr_be  = 8'hFF;

// -- DDR3 Address Constants --------------------------------------------
// 29-bit qword addresses = physical >> 3
localparam [28:0] CTRL_ADDR   = 29'h07400000;  // 0x3A000000 >> 3
localparam [28:0] JOY0_ADDR   = 29'h07400001;  // 0x3A000008 >> 3 (P1 joystick)
// JOY1/2/3 placed at 0x030/0x038/0x040 — distinct from PICO-8's audio
// pointers at 0x020/0x028 (which OpenBOR's layout uses for joy P3/P4).
localparam [28:0] JOY1_ADDR   = 29'h07400006;  // 0x3A000030 >> 3 (P2 joystick)
localparam [28:0] JOY2_ADDR   = 29'h07400007;  // 0x3A000038 >> 3 (P3 joystick)
localparam [28:0] JOY3_ADDR   = 29'h07400008;  // 0x3A000040 >> 3 (P4 joystick)
localparam [28:0] BUF0_ADDR   = 29'h07400020;  // 0x3A000100 >> 3
localparam [28:0] BUF1_ADDR   = 29'h07401020;  // 0x3A008100 >> 3
localparam [7:0]  LINE_BURST  = 8'd32;         // 128px * 2B / 8 = 32 beats
localparam [28:0] LINE_STRIDE = 29'd32;        // 32 qword addresses per source line
localparam [8:0]  V_ACTIVE    = 9'd224;        // NES exact (128 source -> 1.75x via Bresenham 4/7)
localparam [6:0]  SRC_LINES   = 7'd128;        // source lines in DDR3

localparam [19:0] TIMEOUT_MAX = 20'hF_FFFF;

// -- Enable synchronizer ----------------------------------------------
reg [1:0] enable_sync;
always @(posedge ddr_clk) begin
    if (reset)
        enable_sync <= 2'b0;
    else
        enable_sync <= {enable_sync[0], enable};
end
wire enable_ddr = enable_sync[1];

// -- CDC: new_frame ----------------------------------------------------
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
reg synced;  // Set after first ctrl read -- prevents displaying stale DDR3 data

// -- CDC: new_line -----------------------------------------------------
reg [1:0] new_line_sync;
always @(posedge ddr_clk) begin
    if (reset)
        new_line_sync <= 2'b0;
    else
        new_line_sync <= {new_line_sync[0], new_line};
end
wire new_line_ddr = ~new_line_sync[1] & new_line_sync[0];

// -- CDC: vblank level -------------------------------------------------
reg [1:0] vblank_sync;
always @(posedge ddr_clk) begin
    if (reset)
        vblank_sync <= 2'b0;
    else
        vblank_sync <= {vblank_sync[0], vblank};
end
wire vblank_ddr = vblank_sync[1];

// -- Reset synchronizer for clk_vid -----------------------------------
reg [1:0] reset_vid_sync;
always @(posedge clk_vid or posedge reset)
    if (reset) reset_vid_sync <= 2'b11;
    else       reset_vid_sync <= {reset_vid_sync[0], 1'b0};
wire reset_vid = reset_vid_sync[1];

// -- CDC: frame_ready --------------------------------------------------
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

// -- DDR3 Read State Machine ------------------------------------------
localparam [4:0] ST_IDLE         = 5'd0;
localparam [4:0] ST_POLL_CTRL    = 5'd1;
localparam [4:0] ST_WAIT_CTRL    = 5'd2;
localparam [4:0] ST_CHECK_CTRL   = 5'd3;
localparam [4:0] ST_READ_LINE    = 5'd4;
localparam [4:0] ST_WAIT_LINE    = 5'd5;
localparam [4:0] ST_LINE_DONE    = 5'd6;
localparam [4:0] ST_WAIT_DISPLAY = 5'd7;
localparam [4:0] ST_WRITE_JOY0  = 5'd8;
localparam [4:0] ST_WRITE_CART  = 5'd9;
localparam [4:0] ST_WRITE_CART_SIZE = 5'd10;
localparam [4:0] ST_WRITE_FEEDBACK = 5'd11;
localparam [4:0] ST_POLL_AUD_WR    = 5'd12;
localparam [4:0] ST_WAIT_AUD_WR    = 5'd13;
localparam [4:0] ST_READ_AUD_RING  = 5'd14;
localparam [4:0] ST_WAIT_AUD_RING  = 5'd15;
localparam [4:0] ST_WRITE_AUD_RD   = 5'd16;
localparam [4:0] ST_WRITE_JOY1  = 5'd17;
localparam [4:0] ST_WRITE_JOY2  = 5'd18;
localparam [4:0] ST_WRITE_JOY3  = 5'd19;

// Cart loading DDR3 addresses
localparam [28:0] CART_CTRL_ADDR = 29'h07400002;  // 0x3A000010 >> 3
localparam [28:0] CART_DATA_ADDR = 29'h07404000;  // 0x3A020000 >> 3 (past video buffers)
localparam [28:0] FEEDBACK_ADDR  = 29'h07400003;  // 0x3A000018 >> 3 (vsync feedback)

// Audio DDR3 addresses
localparam [28:0] AUD_WPTR_ADDR  = 29'h07400004;  // 0x3A000020 >> 3
localparam [28:0] AUD_RPTR_ADDR  = 29'h07400005;  // 0x3A000028 >> 3
localparam [28:0] AUD_RING_ADDR  = 29'h07402040;  // 0x3A010200 >> 3
localparam [11:0] AUD_RING_MASK  = 12'hFFF;        // 4096 samples

// 48kHz tick derived from CLK_AUDIO (24.576MHz / 512 = 48kHz) in output section

reg  [4:0]  state;
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

// Audio registers (ddr_clk domain — fills DCFIFO from DDR3)
// Ported from OpenBOR's proven dual-clock FIFO architecture.
// All planning uses 32-bit values to avoid 12-bit overflow.
reg  [31:0] aud_wr_ptr;        // cached write pointer (sample units, from ARM)
reg  [31:0] aud_rd_ptr;        // read pointer (sample units, written to DDR3)
reg   [7:0] aud_burst_rem;     // remaining qwords in current burst
reg  [31:0] aud_burst_samples; // total samples in this burst (for ptr advance)
reg         aud_fifo_wr;
reg  [63:0] aud_fifo_wr_data;
reg  [19:0] aud_backoff;       // cooldown after fetch to let video run

// FIFO fill level tracking
wire  [9:0] aud_fifo_wrusedw;
wire        aud_fifo_empty;
localparam [9:0] AUD_FIFO_REFILL = 10'd384;
wire aud_fifo_low = (aud_fifo_wrusedw < AUD_FIFO_REFILL);

// Burst planning (32-bit, no overflow possible)
localparam [31:0] AUD_RING_SAMPLES_32 = 32'd4096;
localparam [31:0] AUD_RING_MASK_32    = 32'd4095;
wire [31:0] aud_avail_32      = (aud_wr_ptr - aud_rd_ptr) & AUD_RING_MASK_32;
wire [31:0] aud_plan_cand_a   = (aud_avail_32 > 32'd64) ? 32'd64 : aud_avail_32;
wire [31:0] aud_plan_wrap     = AUD_RING_SAMPLES_32 - (aud_rd_ptr & AUD_RING_MASK_32);
wire [31:0] aud_plan_cand_b   = (aud_plan_cand_a > aud_plan_wrap) ? aud_plan_wrap : aud_plan_cand_a;
wire [31:0] aud_plan_samples  = aud_plan_cand_b & 32'hFFFFFFFE;
wire  [7:0] aud_plan_qwords   = aud_plan_samples[8:1];
wire        aud_wake          = enable_ddr && aud_fifo_low && (aud_backoff == 20'd0);

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

// VSync feedback
reg  [29:0] vblank_counter;

assign ioctl_wait = cart_write_pending & ioctl_download;

// -- Source-line computation (Bresenham 4/7) --------------------------
// 128 source rows scaled to 224 display lines (1.75x vertical), matching
// NES exact V_ACTIVE. Per 4 source rows, 7 display lines: pattern 2,2,2,1.
// All 128 source rows displayed; no pixels lost. Pixel aspect 8:7.
//
// Bresenham state advanced in ST_LINE_DONE (about to enter next display
// line). Reset at frame start (in ST_CHECK_CTRL).
reg [6:0] safe_src_line;   // 0..127, source row for current display_line
reg [2:0] safe_accum;      // Bresenham accumulator, 0..6 (carries (eff*4) mod 7)

wire [6:0] source_line = safe_src_line;

// -- FIFO write signals -----------------------------------------------
reg         fifo_wr;
reg  [63:0] fifo_wr_data;
wire        fifo_full;

// -- FIFO async clear -------------------------------------------------
reg [3:0] fifo_aclr_cnt;
wire fifo_aclr_ddr_active = (fifo_aclr_cnt != 4'd0);
wire fifo_aclr = reset | fifo_aclr_ddr_active;

// -- Main state machine -----------------------------------------------
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
        synced              <= 1'b0;
        vblank_counter      <= 30'd0;
        aud_wr_ptr          <= 32'd0;
        aud_rd_ptr          <= 32'd0;
        aud_burst_rem       <= 8'd0;
        aud_burst_samples   <= 32'd0;
        aud_fifo_wr         <= 1'b0;
        aud_fifo_wr_data    <= 64'd0;
        aud_backoff         <= 20'd0;
        safe_src_line       <= 7'd0;
        safe_accum          <= 3'd0;
    end
    else begin
        fifo_wr <= 1'b0;
        if (fifo_aclr_cnt != 4'd0) fifo_aclr_cnt <= fifo_aclr_cnt - 4'd1;
        if (!ddr_busy) ddr_rd <= 1'b0;
        if (!ddr_busy) ddr_we <= 1'b0;

        // Latch new_frame pulse so cart writes can't cause it to be missed
        if (new_frame_ddr) new_frame_pending <= 1'b1;

        // Audio FIFO write pulse is one-cycle
        aud_fifo_wr <= 1'b0;
        if (aud_backoff != 20'd0) aud_backoff <= aud_backoff - 20'd1;

        // Beat capture (runs in parallel with state machine)
        if (state == ST_WAIT_LINE && ddr_dout_ready) begin
            fifo_wr      <= 1'b1;
            fifo_wr_data <= ddr_dout;
            beat_count   <= beat_count + 7'd1;
            timeout_cnt  <= 20'd0;
        end

        // -- Cart byte collection (runs in parallel) --------------
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

        // Download end -- flush partial + write size
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
                // Frame reads always get priority -- video must never be starved.
                // Cart writes happen between frame reads.
                // new_frame_pending is latched so it can't be missed.
                if (enable_ddr && new_frame_pending) begin
                    new_frame_pending <= 1'b0;  // consumed
                    state <= ST_WRITE_JOY0;
                end
                else if (cart_write_pending)
                    state <= ST_WRITE_CART;
                else if (cart_size_pending)
                    state <= ST_WRITE_CART_SIZE;
                else if (aud_wake)
                    state <= ST_POLL_AUD_WR;
            end

            ST_WRITE_JOY0: begin
                // Write joystick_0 (P1) to DDR3 so ARM can read it
                if (!ddr_busy) begin
                    ddr_addr     <= JOY0_ADDR;
                    ddr_din      <= {32'd0, joystick_0};
                    ddr_burstcnt <= 8'd1;
                    ddr_we       <= 1'b1;
                    state        <= ST_WRITE_JOY1;
                end
            end

            ST_WRITE_JOY1: begin
                // Write joystick_1 (P2) to DDR3
                if (!ddr_busy) begin
                    ddr_addr     <= JOY1_ADDR;
                    ddr_din      <= {32'd0, joystick_1};
                    ddr_burstcnt <= 8'd1;
                    ddr_we       <= 1'b1;
                    state        <= ST_WRITE_JOY2;
                end
            end

            ST_WRITE_JOY2: begin
                // Write joystick_2 (P3) to DDR3
                if (!ddr_busy) begin
                    ddr_addr     <= JOY2_ADDR;
                    ddr_din      <= {32'd0, joystick_2};
                    ddr_burstcnt <= 8'd1;
                    ddr_we       <= 1'b1;
                    state        <= ST_WRITE_JOY3;
                end
            end

            ST_WRITE_JOY3: begin
                // Write joystick_3 (P4) to DDR3, then vsync feedback
                if (!ddr_busy) begin
                    ddr_addr     <= JOY3_ADDR;
                    ddr_din      <= {32'd0, joystick_3};
                    ddr_burstcnt <= 8'd1;
                    ddr_we       <= 1'b1;
                    state        <= ST_WRITE_FEEDBACK;
                end
            end

            ST_WRITE_FEEDBACK: begin
                // Write vsync feedback word so ARM knows when frames are consumed
                if (!ddr_busy) begin
                    vblank_counter <= vblank_counter + 30'd1;
                    ddr_addr       <= FEEDBACK_ADDR;
                    ddr_din        <= {32'd0, vblank_counter + 30'd1, active_buffer, 1'b0};
                    ddr_burstcnt   <= 8'd1;
                    ddr_we         <= 1'b1;
                    state          <= ST_POLL_CTRL;
                end
            end

            ST_POLL_AUD_WR: begin
                if (!ddr_busy) begin
                    ddr_addr     <= AUD_WPTR_ADDR;
                    ddr_burstcnt <= 8'd1;
                    ddr_rd       <= 1'b1;
                    timeout_cnt  <= 20'd0;
                    state        <= ST_WAIT_AUD_WR;
                end
            end

            ST_WAIT_AUD_WR: begin
                if (ddr_dout_ready) begin
                    aud_wr_ptr <= {20'd0, ddr_dout[11:0]};
                    if (!aud_fifo_low) begin
                        state <= ST_IDLE;
                    end
                    else if (aud_plan_qwords == 8'd0) begin
                        state <= ST_IDLE;
                    end
                    else begin
                        aud_burst_rem     <= aud_plan_qwords;
                        aud_burst_samples <= aud_plan_samples;
                        state             <= ST_READ_AUD_RING;
                    end
                end
                else if (timeout_cnt == TIMEOUT_MAX)
                    state <= ST_IDLE;
                else
                    timeout_cnt <= timeout_cnt + 20'd1;
            end

            ST_READ_AUD_RING: begin
                if (!ddr_busy) begin
                    // DDR3 qword address: ring_base + rd_ptr_in_samples / 2
                    ddr_addr     <= AUD_RING_ADDR + aud_rd_ptr[11:1];
                    ddr_burstcnt <= aud_burst_rem;
                    ddr_rd       <= 1'b1;
                    timeout_cnt  <= 20'd0;
                    state        <= ST_WAIT_AUD_RING;
                end
            end

            ST_WAIT_AUD_RING: begin
                if (ddr_dout_ready) begin
                    aud_fifo_wr_data <= ddr_dout;
                    aud_fifo_wr      <= 1'b1;
                    aud_burst_rem    <= aud_burst_rem - 8'd1;
                    if (aud_burst_rem == 8'd1) begin
                        aud_rd_ptr <= (aud_rd_ptr + aud_burst_samples) & AUD_RING_MASK_32;
                        state      <= ST_WRITE_AUD_RD;
                    end
                end
                else if (timeout_cnt == TIMEOUT_MAX) begin
                    state <= ST_IDLE;
                end
                else begin
                    timeout_cnt <= timeout_cnt + 20'd1;
                end
            end

            ST_WRITE_AUD_RD: begin
                if (!ddr_busy) begin
                    ddr_addr     <= AUD_RPTR_ADDR;
                    ddr_din      <= {32'd0, aud_rd_ptr[31:0]};
                    ddr_burstcnt <= 8'd1;
                    ddr_we       <= 1'b1;
                    aud_backoff  <= 20'd1000;
                    state        <= ST_IDLE;
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
                if (!synced) begin
                    // First read after reset -- capture stale DDR3 counter
                    // without displaying anything. Prevents showing old game
                    // data that persists in DDR3 across reboots.
                    prev_frame_counter <= ctrl_word[31:2];
                    synced <= 1'b1;
                    state <= ST_IDLE;
                end
                else if (ctrl_word[31:2] != prev_frame_counter) begin
                    // New frame available
                    prev_frame_counter <= ctrl_word[31:2];
                    active_buffer      <= ctrl_word[0];
                    stale_vblank_count <= 5'd0;
                    buf_base_addr      <= ctrl_word[0] ? BUF1_ADDR : BUF0_ADDR;
                    display_line       <= 9'd0;
                    safe_src_line      <= 7'd0;   // reset Bresenham at frame start
                    safe_accum         <= 3'd0;
                    preloading         <= 1'b1;
                    fifo_aclr_cnt      <= 4'd8;
                    state              <= ST_READ_LINE;
                end
                else if (first_frame_loaded) begin
                    // Stale frame -- re-read previous buffer
                    if (stale_vblank_count < 5'd30)
                        stale_vblank_count <= stale_vblank_count + 5'd1;
                    if (stale_vblank_count >= 5'd29)
                        frame_ready_reg <= 1'b0;
                    display_line  <= 9'd0;
                    safe_src_line <= 7'd0;   // reset Bresenham on stale-frame replay too
                    safe_accum    <= 3'd0;
                    preloading    <= 1'b1;
                    fifo_aclr_cnt <= 4'd8;
                    state         <= ST_READ_LINE;
                end
                else
                    state <= ST_IDLE;
            end

            ST_READ_LINE: begin
                if (!ddr_busy && !fifo_aclr_ddr_active) begin
                    // source_line is Bresenham-tracked safe_src_line (1.75x
                    // vertical, 128->224 over the entire 224-line active area).
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

                // Advance Bresenham state for the NEXT display line.
                // 4/7 pattern: per 4 source rows produce 7 display lines
                // (2,2,2,1). Don't advance on the very last transition
                // (V_ACTIVE-1 -> V_ACTIVE) since there's no next line.
                //
                // Critical: do the +4 in 4-bit arithmetic so the carry test
                // does NOT truncate. With 3-bit (`safe_accum + 3'd4`), the
                // sum overflows when safe_accum >= 4 (e.g., 4+4=8 truncates
                // to 0, 5+4=9 truncates to 1, 6+4=10 truncates to 2), so
                // the >=7 test silently misses every carry past the first
                // and safe_src_line never advances past 0. Symptom:
                // every display line reads source row 0, producing
                // per-cart solid colors / vertical stripes.
                if (display_line < V_ACTIVE - 9'd1) begin
                    if ({1'b0, safe_accum} + 4'd4 >= 4'd7) begin
                        safe_src_line <= safe_src_line + 7'd1;
                        safe_accum    <= safe_accum + 3'd4 - 3'd7;
                    end
                    else begin
                        safe_accum <= safe_accum + 3'd4;
                    end
                end

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

// -- Dual-Clock FIFO --------------------------------------------------
// 64-bit wide, stores raw DDR3 beats (4 RGB565 pixels per entry)
// Depth 64: holds 2 scanlines (32 beats/line x 2)
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

// -- Pixel Output with 2x Horizontal Doubling -------------------------
//
// Each 64-bit FIFO word = 4 source pixels (RGB565).
// With 2x horizontal doubling, each source pixel outputs twice,
// so each word produces 8 display pixels.
//
// pixel_sub[1:0] selects which of the 4 source pixels (0..3)
// pixel_phase toggles 0->1 between first/second copy of each pixel
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
                        // First copy done -- output same pixel again next cycle
                        pixel_phase <= 1'b1;
                    end
                    else begin
                        // Second copy done -- advance to next source pixel
                        pixel_phase <= 1'b0;

                        if (pixel_sub == 2'd3) begin
                            // Word exhausted -- load next from FIFO
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

// -- Audio dual-clock FIFO (ddr_clk write, clk_audio read) ---------------
wire [63:0] aud_fifo_rd_data;
reg         aud_fifo_rd;

dcfifo #(
    .intended_device_family ("Cyclone V"),
    .lpm_numwords           (1024),
    .lpm_showahead          ("ON"),
    .lpm_type               ("dcfifo"),
    .lpm_width              (64),
    .lpm_widthu             (10),
    .overflow_checking      ("ON"),
    .rdsync_delaypipe       (4),
    .underflow_checking     ("ON"),
    .use_eab                ("ON"),
    .wrsync_delaypipe       (4)
) audio_fifo_inst (
    .aclr     (reset),
    .data     (aud_fifo_wr_data),
    .rdclk    (clk_audio),
    .rdreq    (aud_fifo_rd),
    .wrclk    (ddr_clk),
    .wrreq    (aud_fifo_wr),
    .q        (aud_fifo_rd_data),
    .rdempty  (aud_fifo_empty),
    .wrfull   (),
    .wrusedw  (aud_fifo_wrusedw),
    .eccstatus(),
    .rdfull   (),
    .rdusedw  (),
    .wrempty  ()
);

// -- 48 kHz sample clock (clk_audio / 512 = 48 kHz exactly) --------------
reg [8:0] aud_clk_div;
reg       aud_tick;
reg [1:0] reset_aud_sync;
always @(posedge clk_audio or posedge reset)
    if (reset) reset_aud_sync <= 2'b11;
    else       reset_aud_sync <= {reset_aud_sync[0], 1'b0};
wire reset_aud = reset_aud_sync[1];

always @(posedge clk_audio) begin
    if (reset_aud) begin
        aud_clk_div <= 9'd0;
        aud_tick    <= 1'b0;
    end
    else begin
        aud_clk_div <= aud_clk_div + 9'd1;
        aud_tick    <= (aud_clk_div == 9'd0);
    end
end

// -- Audio sample output (clk_audio domain) ------------------------------
reg aud_half_sel;
always @(posedge clk_audio) begin
    if (reset_aud) begin
        audio_l_out  <= 16'd0;
        audio_r_out  <= 16'd0;
        aud_fifo_rd  <= 1'b0;
        aud_half_sel <= 1'b0;
    end
    else begin
        aud_fifo_rd <= 1'b0;
        if (aud_tick) begin
            if (!aud_fifo_empty) begin
                if (aud_half_sel == 1'b0) begin
                    audio_l_out  <= aud_fifo_rd_data[15:0];
                    audio_r_out  <= aud_fifo_rd_data[31:16];
                    aud_half_sel <= 1'b1;
                end
                else begin
                    audio_l_out  <= aud_fifo_rd_data[47:32];
                    audio_r_out  <= aud_fifo_rd_data[63:48];
                    aud_fifo_rd  <= 1'b1;
                    aud_half_sel <= 1'b0;
                end
            end
        end
    end
end

endmodule
