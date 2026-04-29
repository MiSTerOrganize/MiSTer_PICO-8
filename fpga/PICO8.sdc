# PICO-8 project-level timing constraints.
#
# The MiSTer framework's sys/sys_top.sdc handles base clocks (FPGA_CLK*_50,
# HPS clocks, HDMI, SPI). This file declares the user-side clock relationships
# specific to the PICO-8 core.
#
# Three PLL output clocks come from emu|pll|pll_inst (see PICO8.sv):
#   general[0] = clk_sys  (~100 MHz, DDR3 + reader state machine)
#   general[1] = clk_20m  (currently unused)
#   general[2] = clk_pix  (21.477 MHz, CLK_VIDEO, divides /4 to 5.369 MHz CE_PIXEL)
# A separate PLL drives clk_audio (24.576 MHz) for the audio output domain.
#
# All cross-domain signaling between these clocks goes through proper
# synchronizers:
#   - DDR3 reader uses 2-FF synchronizers for new_frame, new_line, vblank,
#     enable, frame_ready (clk_sys <-> clk_pix)
#   - Video FIFO (line_fifo) is a dcfifo — clk_sys write, clk_pix read
#   - Audio FIFO (audio_fifo_inst) is a dcfifo — clk_sys write, clk_audio read
#
# Without these constraints, Quartus tries to time paths between clk_sys and
# clk_pix as synchronous, reporting -4ns+ setup failures even though the
# bitstream is correct. Asynchronous clock-group declarations tell the timing
# analyzer "these are unrelated; do not check setup/hold between them."
#
# Cause of the PICO-8 v1.0 -> a783c94 video corruption:
#   v1.0 had clk_pix slack of -4.389 ns (failing, but bitstream worked by
#   silicon luck). a783c94 shifted slack to -4.876 ns; same RTL semantics
#   but worse placement = incoherent solid-color / vertical-stripe video.
#   Fix is to declare these CDC paths properly so timing analyzer doesn't
#   report phantom failures, AND so Quartus doesn't waste optimization
#   effort on paths that don't need to meet single-cycle setup.

set_clock_groups -asynchronous \
    -group [get_clocks {emu|pll|pll_inst|altera_pll_i|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}] \
    -group [get_clocks {emu|pll|pll_inst|altera_pll_i|general[2].gpll~PLL_OUTPUT_COUNTER|divclk}] \
    -group [get_clocks {pll_audio|pll_audio_inst|altera_pll_i|general[0].gpll~PLL_OUTPUT_COUNTER|divclk}]
