//============================================================================
//
//  PICO-8 Cart Loader — receives file data from MiSTer OSD file browser
//  via hps_io ioctl interface and writes it to DDR3 for the ARM to read.
//
//  DDR3 Layout:
//    0x3A000010 (CART_CTRL): file_size[31:0] — non-zero = new cart ready
//    0x3A010000 (CART_DATA): cart file data (up to 256KB)
//
//  Copyright (C) 2026 MiSTer Organize — GPL-3.0
//
//============================================================================

module pico8_cart_loader (
    input  wire        clk,
    input  wire        reset,

    // ioctl from hps_io
    input  wire        ioctl_download,
    input  wire        ioctl_wr,
    input  wire [26:0] ioctl_addr,
    input  wire  [7:0] ioctl_dout,

    // DDR3 write interface
    input  wire        ddr_busy,
    output reg   [7:0] ddr_burstcnt,
    output reg  [28:0] ddr_addr,
    output reg  [63:0] ddr_din,
    output reg         ddr_we,
    output wire  [7:0] ddr_be,

    // Status
    output wire        active
);

assign ddr_be = 8'hFF;
assign active = loading | flushing;

localparam [28:0] CART_CTRL_ADDR = 29'h07400002;  // 0x3A000010 >> 3
localparam [28:0] CART_DATA_ADDR = 29'h07402000;  // 0x3A010000 >> 3

reg        loading;
reg        flushing;
reg [63:0] byte_buf;
reg  [2:0] byte_cnt;
reg [26:0] total_bytes;
reg        write_pending;
reg [28:0] pending_addr;
reg [63:0] pending_data;
reg        size_pending;
reg        download_prev;

always @(posedge clk) begin
    if (reset) begin
        loading       <= 1'b0;
        flushing      <= 1'b0;
        byte_buf      <= 64'd0;
        byte_cnt      <= 3'd0;
        total_bytes   <= 27'd0;
        write_pending <= 1'b0;
        size_pending  <= 1'b0;
        pending_addr  <= 29'd0;
        pending_data  <= 64'd0;
        download_prev <= 1'b0;
        ddr_we        <= 1'b0;
        ddr_burstcnt  <= 8'd1;
        ddr_addr      <= 29'd0;
        ddr_din       <= 64'd0;
    end
    else begin
        if (!ddr_busy) ddr_we <= 1'b0;
        download_prev <= ioctl_download;

        // Download start
        if (ioctl_download && !download_prev) begin
            loading     <= 1'b1;
            flushing    <= 1'b0;
            byte_cnt    <= 3'd0;
            byte_buf    <= 64'd0;
            total_bytes <= 27'd0;
        end

        // Collect bytes
        if (ioctl_download && ioctl_wr && !write_pending) begin
            case (byte_cnt)
                3'd0: byte_buf[ 7: 0] <= ioctl_dout;
                3'd1: byte_buf[15: 8] <= ioctl_dout;
                3'd2: byte_buf[23:16] <= ioctl_dout;
                3'd3: byte_buf[31:24] <= ioctl_dout;
                3'd4: byte_buf[39:32] <= ioctl_dout;
                3'd5: byte_buf[47:40] <= ioctl_dout;
                3'd6: byte_buf[55:48] <= ioctl_dout;
                3'd7: byte_buf[63:56] <= ioctl_dout;
            endcase
            total_bytes <= ioctl_addr + 27'd1;

            if (byte_cnt == 3'd7) begin
                write_pending <= 1'b1;
                pending_addr  <= CART_DATA_ADDR + {2'd0, ioctl_addr[26:3]};
                pending_data  <= {ioctl_dout, byte_buf[55:0]};
                byte_cnt      <= 3'd0;
            end
            else begin
                byte_cnt <= byte_cnt + 3'd1;
            end
        end

        // Download end — flush + write size
        if (!ioctl_download && download_prev && loading) begin
            loading  <= 1'b0;
            flushing <= 1'b1;
            if (byte_cnt != 3'd0) begin
                write_pending <= 1'b1;
                pending_addr  <= CART_DATA_ADDR + {2'd0, total_bytes[26:3]};
                pending_data  <= byte_buf;
                byte_cnt      <= 3'd0;
            end
            else begin
                size_pending <= 1'b1;
            end
        end

        // DDR3 write: data
        if (write_pending && !ddr_busy && !ddr_we) begin
            ddr_addr      <= pending_addr;
            ddr_din       <= pending_data;
            ddr_burstcnt  <= 8'd1;
            ddr_we        <= 1'b1;
            write_pending <= 1'b0;
            byte_buf      <= 64'd0;
            if (flushing && !size_pending)
                size_pending <= 1'b1;
        end

        // DDR3 write: file size
        if (size_pending && !write_pending && !ddr_busy && !ddr_we) begin
            ddr_addr     <= CART_CTRL_ADDR;
            ddr_din      <= {32'd0, 5'd0, total_bytes};
            ddr_burstcnt <= 8'd1;
            ddr_we       <= 1'b1;
            size_pending <= 1'b0;
            flushing     <= 1'b0;
        end
    end
end

endmodule
