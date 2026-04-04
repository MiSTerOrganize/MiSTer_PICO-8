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

/// Read the joystick state from DDR3 (written by FPGA from hps_io).
/// Returns joystick_0 bitmask: bit0=right, bit1=left, bit2=down, bit3=up,
/// bit4=O, bit5=X, bit6=Pause (matching CONF_STR "J1,O,X,Pause").
/// Returns 0 if native video is not active.
uint32_t NativeVideoWriter_ReadJoystick(void);

/// Read the analog stick data from DDR3 (written by FPGA from hps_io).
/// Returns raw 16-bit value: [15:8]=Y signed, [7:0]=X signed, range -127..+127.
/// Returns 0 if native video is not active.
uint16_t NativeVideoWriter_ReadAnalog(void);

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

#ifdef __cplusplus
}
#endif

#endif
