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

#ifdef __cplusplus
}
#endif

#endif
