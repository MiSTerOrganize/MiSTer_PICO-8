#ifndef PICO8_NATIVE_VIDEO_WRITER_H
#define PICO8_NATIVE_VIDEO_WRITER_H

//
//  Native Video DDR3 Writer for PICO-8 on MiSTer
//
//  Maps /dev/mem at 0x3A000000 and writes 128x128 RGB565 frames
//  into a double-buffered DDR3 region. The FPGA-side pico8_video_reader
//  polls a control word and reads pixel data for native video output.
//
//  Usage:
//    NativeVideoWriter_Init();
//    // each frame:
//    NativeVideoWriter_WriteFrame(rgba_buf, 128, 128);
//    // on shutdown:
//    NativeVideoWriter_Shutdown();
//
//  Copyright (C) 2026 MiSTer Organize — GPL-3.0
//

/* ==========================================================================
 * DDR3 MEMORY MAP -- SINGLE SOURCE OF TRUTH
 *
 * 🛑 These offsets are mirrored in fpga/rtl/pico8_video_reader.sv. Change them
 * together or the FPGA reads a region nobody is writing and shows garbage.
 *
 * They live in the HEADER, not the .c, so every consumer shares one copy.
 * nv_test_pattern.c used to carry its own duplicate and had ALREADY drifted --
 * its NV_DDR_REGION_SIZE said 0x20000 against the writer's 0x60000. Harmless
 * there by luck (it only touches the control word and the buffers, all below
 * 0x10100), but it is exactly the silent divergence this project has been
 * bitten by before, and the second copy is what made it possible.
 * ========================================================================== */
#define NV_DDR_PHYS_BASE    0x3A000000u
#define NV_DDR_REGION_SIZE  0x00060000u   /* 384KB covers buffers + control + cart data */
#define NV_CTRL_OFFSET      0x00000000u
#define NV_JOY0_OFFSET      0x00000008u  /* P1 joystick_0 from FPGA (physical 0x3A000008) */
/* JOY1/2/3 placed at 0x030/0x038/0x040 — distinct from PICO-8's audio
 * pointers at 0x020/0x028. Matches FPGA reader's JOY1/2/3_ADDR. */
#define NV_JOY1_OFFSET      0x00000030u  /* P2 joystick_1 */
#define NV_JOY2_OFFSET      0x00000038u  /* P3 joystick_2 */
#define NV_JOY3_OFFSET      0x00000040u  /* P4 joystick_3 */
#define NV_SS_OFFSET        0x00000048u  /* save state ctrl word (FPGA writes, ARM reads) */
                                         /* layout (LE): byte 0 = cmd (0=idle 1=save 2=load),
                                          *              byte 1 = slot (0..3),
                                          *              byte 2 = sequence counter (changes each event) */
#define NV_BUF0_OFFSET      0x00000100u
#define NV_BUF1_OFFSET      0x00008100u
#define NV_CART_CTRL_OFFSET  0x00000010u
#define NV_FEEDBACK_OFFSET   0x00000018u  /* vsync feedback (physical 0x3A000018) */
#define NV_AUD_WPTR_OFFSET   0x00000020u  /* audio write pointer (physical 0x3A000020) */
#define NV_AUD_RPTR_OFFSET   0x00000028u  /* audio read pointer (physical 0x3A000028) */
#define NV_CART_DATA_OFFSET  0x00020000u
#define NV_CART_MAX_SIZE     0x00040000u  /* 256KB max cart size */
#define NV_AUD_RING_OFFSET   0x00010200u  /* audio ring buffer (physical 0x3A010200) */
#define NV_AUD_RING_SAMPLES  4096         /* stereo samples (L+R = 4 bytes each) */
#define NV_AUD_RING_MASK     (NV_AUD_RING_SAMPLES - 1)
#define NV_FRAME_WIDTH      128
#define NV_FRAME_HEIGHT     128
#define NV_FRAME_BYTES      (NV_FRAME_WIDTH * NV_FRAME_HEIGHT * 2)  /* 32,768 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize the DDR3 direct writer. Maps /dev/mem at the native video
/// buffer region and clears both frame buffers. Returns true on success.
bool NativeVideoWriter_Init(void);

/// Release DDR3 mapping and close /dev/mem.
void NativeVideoWriter_Shutdown(void);

/// Convert one 128x128 RGBA8888 frame to RGB565 and write it into the
/// inactive DDR3 double-buffer, then flip the control word.
/// @param rgba8_pixels  Source pixel data: 128x128 array of {r,g,b,a} bytes
/// @param width         Must be 128
/// @param height        Must be 128
void NativeVideoWriter_WriteFrame(const void* rgba8_pixels, int width, int height);

/// True if the DDR3 writer has been initialized and is ready for frames.
bool NativeVideoWriter_IsActive(void);

/// Increment the FPGA frame counter without writing pixel data. Points the
/// active-buffer bit at the LAST-written buffer so the FPGA video reader
/// keeps re-displaying that frame instead of hitting its 30-vblank staleness
/// timeout (~500ms) which would blank the screen to black.
///
/// Use case: keep the previous frame visible during cart hot-swap, save-state
/// load, or any other operation that pauses real frame writes for >500ms.
/// Run on a timer thread at ~150ms cadence — well under the timeout, doesn't
/// add latency to real frames since it's a single 32-bit ctrl-word write.
void NativeVideoWriter_KeepaliveTick(void);

/// Zero both DDR3 frame buffers and tick the control word so the FPGA flips
/// to a freshly-cleared black frame. Use on Quit (when returning to the
/// .s0-wait loop) so the keepalive thread doesn't keep showing the previous
/// cart's last frame frozen on screen — would look like the game crashed.
void NativeVideoWriter_ClearScreen(void);

/// Check if a new cart has been loaded via OSD file browser.
/// Returns file size in bytes if a new cart is available, 0 otherwise.
uint32_t NativeVideoWriter_CheckCart(void);

/// Read cart data from DDR3 into the provided buffer.
/// @param buf     Destination buffer (must be at least max_size bytes)
/// @param max_size Maximum bytes to read
/// @return Actual bytes read
uint32_t NativeVideoWriter_ReadCart(void* buf, uint32_t max_size);

/// Clear the cart control word so the FPGA knows the ARM has read the cart.
void NativeVideoWriter_AckCart(void);

/// Read joystick state from DDR3 (written by FPGA from hps_io).
/// Returns MiSTer joystick_N bitmask: bit0=R, bit1=L, bit2=D, bit3=U,
/// bit4=A, bit5=B, bit6=X, bit7=Y, bit10=Select, bit11=Start
/// @param player  0..3 (P1..P4); out-of-range returns 0
uint32_t NativeVideoWriter_ReadJoystick(int player);

/// Read VSync feedback word from DDR3 (written by FPGA each vblank).
/// Bits [31:2] = vblank_counter, bits [1:0] = buffer_status.
uint32_t NativeVideoWriter_ReadFeedback(void);

/// Read save state control word from DDR3 (written by FPGA each frame).
/// Layout (little-endian): byte 0 = cmd (0=idle, 1=save, 2=load),
///                         byte 1 = slot (0..3),
///                         byte 2 = sequence counter (changes on each new event).
/// ARM tracks the sequence counter; when it changes, dispatch save/load.
uint32_t NativeVideoWriter_ReadSavestate(void);

/// Helpers for unpacking the savestate control word.
static inline uint8_t  NV_SsCmd (uint32_t w) { return (uint8_t)( w        & 0xFFu); }
static inline uint8_t  NV_SsSlot(uint32_t w) { return (uint8_t)((w >> 8 ) & 0xFFu); }
static inline uint8_t  NV_SsSeq (uint32_t w) { return (uint8_t)((w >> 16) & 0xFFu); }

/// Extract vblank counter from feedback word.
static inline uint32_t NV_FeedbackVblankCounter(uint32_t fb) { return fb >> 2; }

/// Extract buffer status from feedback word (0 or 1 = which buffer FPGA is reading).
static inline uint32_t NV_FeedbackBufferStatus(uint32_t fb) { return fb & 3; }

/// Returns number of stereo samples that can be written to the audio ring buffer.
uint32_t NativeVideoWriter_AudioSpace(void);

/// Write stereo samples to the DDR3 audio ring buffer.
/// @param stereo_samples  Interleaved int16_t L,R pairs
/// @param num_samples     Number of stereo sample PAIRS to write
void NativeVideoWriter_WriteAudio(const int16_t *stereo_samples, uint32_t num_samples);

/// FPS overlay (pause menu -> Options -> "fps display"). Required on every
/// hybrid core: bottom-right, red <30 / yellow 30-59 / green >=60. Defaults
/// OFF and resets to OFF each launch, so it can never silently contaminate a
/// screenshot or a frame-hash comparison. Safe to leave on while recording or
/// replaying -- it lives in the framebuffer, not in the input stream.
void NativeVideoWriter_SetFpsOverlay(int on);

/* Show a short line at the top of the screen for `seconds` (0 = default 4).
 * Uppercased and word-wrapped to 21 columns, up to 3 lines. Pass NULL to clear.
 *
 * For anything the player must KNOW -- a refused replay, a version mismatch, a
 * missing snapshot, take-over, a failed Stop Recording. Those all used to go
 * only to pico8.log, which is unreadable from a couch. Keep the log line too:
 * this is the headline, the log is the detail. */
void NativeVideoWriter_Notice(const char* msg, int seconds);

/* Height in rows of the notice band at the top of the frame, 0 when no notice
 * is live.
 *
 * 🛑 EVERY publisher of a DDR3 frame must start its copy below this. The notice
 * is drawn ONCE into each buffer and then left alone; a copy path that writes
 * over those rows puts the notice back to being repainted every frame, and the
 * frames scanned between the copy and the repaint show no notice. That is the
 * flicker -- reported on OpenBOR 2026-08-05 and PICO-8 2026-08-06, one defect
 * with one fix on every hybrid core. */
int  NativeVideoWriter_NoticeRows(void);

/* Forget which buffers already hold the notice band, so a live notice is
 * painted into them again. Call after anything wipes a frame buffer -- the row
 * skip above would otherwise protect a hole nothing ever fills. */
void NativeVideoWriter_NoticeRepaint(void);

int  NativeVideoWriter_GetFpsOverlay(void);

#ifdef __cplusplus
}
#endif

#endif
