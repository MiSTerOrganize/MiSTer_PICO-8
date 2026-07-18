// test_trace.h — golden-master frame/audio hash tracing shared by the
// x86 oracle build (tools/z8headless.cpp, --test flag) and the MiSTer ARM
// build (src/mister_main.cpp, -test flag). Both emit the same trace format
// from the same hash points, so a headless golden trace diffs directly
// against a hardware trace: the first divergent line is the first divergent
// frame (see #Debugging_Tools/CLAUDE-debugging-methodology.md, component 2).
//
// Trace line format (one per frame):   FRAME:VIDEOCRC:AUDIOCRC
//   FRAME    — decimal frame number, counted from the first stepped frame
//   VIDEOCRC — CRC32 over the R,G,B bytes of the native 128x128 render
//              output (pre-upscale; alpha excluded — presentation-only)
//   AUDIOCRC — CRC32 over the raw little-endian int16 bytes of the frame's
//              22050 Hz mono engine audio (pre-upsample; see pacing below)
//
// Audio pacing: 22050 Hz / 60 fps = 367.5 samples per frame, non-integer,
// so each frame pulls rate*(f+1)/fps - rate*f/fps samples (alternating
// 367/368) — long-run exact, both targets identical.
//
// CRC32 is IEEE 802.3 (poly 0xEDB88320), zlib-compatible chaining
// (tt_crc32(tt_crc32(0, a), b) == tt_crc32(0, a..b)); no zlib dependency.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

static inline uint32_t tt_crc32(uint32_t crc, const void *data, size_t len)
{
    // Lazy table init — called from a single (main-loop) thread only.
    static uint32_t table[256];
    static int have_table = 0;
    if (!have_table) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i;
            for (int j = 0; j < 8; j++)
                r = (r >> 1) ^ (0xEDB88320u & (0u - (r & 1u)));
            table[i] = r;
        }
        have_table = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--)
        crc = (crc >> 8) ^ table[(crc ^ *p++) & 0xFF];
    return ~crc;
}

// Samples of RATE-Hz audio belonging to frame F at FPS frames/sec.
// Non-integer rates alternate floor/ceil so long-run pacing is exact.
static inline int tt_audio_samples_for_frame(long long f, int rate, int fps)
{
    return (int)(((long long)rate * (f + 1)) / fps
               - ((long long)rate * f) / fps);
}

static inline void tt_emit(FILE *f, long long frame,
                           uint32_t video_crc, uint32_t audio_crc)
{
    fprintf(f, "%lld:%08x:%08x\n", frame, video_crc, audio_crc);
}
