//
//  Native Video DDR3 Writer — PICO-8 MiSTer
//
//  Writes 128x128 RGB565 frames to DDR3 at 0x3A000000 for FPGA native
//  video output. Double-buffered with control word handshake.
//
//  DDR3 Memory Map:
//    0x3A000000 + 0x000  : Control word (frame_counter[31:2] | active_buf[1:0])
//    0x3A000000 + 0x008  : Joystick data (FPGA writes, ARM reads)
//    0x3A000000 + 0x010  : Cart control (file_size, ARM polls)
//    0x3A000000 + 0x018  : VSync feedback (vblank_counter[31:2] | buffer_status[1:0])
//    0x3A000000 + 0x100  : Buffer 0 (128*128*2 = 32,768 bytes)
//    0x3A000000 + 0x8100 : Buffer 1 (32,768 bytes)
//
//  The FPGA reader polls the control word each vblank. When frame_counter
//  changes, it switches to the indicated buffer.
//
//  Adapted from 3SX project (kimchiman52/3sx-mister)
//  Copyright (C) 2026 MiSTer Organize — GPL-3.0
//

#include "native_video_writer.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#define NV_DDR_PHYS_BASE    0x3A000000u
#define NV_DDR_REGION_SIZE  0x00060000u   /* 384KB covers buffers + control + cart data */
#define NV_CTRL_OFFSET      0x00000000u
#define NV_JOY0_OFFSET      0x00000008u  /* P1 joystick_0 from FPGA (physical 0x3A000008) */
/* JOY1/2/3 placed at 0x030/0x038/0x040 — distinct from PICO-8's audio
 * pointers at 0x020/0x028. Matches FPGA reader's JOY1/2/3_ADDR. */
#define NV_JOY1_OFFSET      0x00000030u  /* P2 joystick_1 */
#define NV_JOY2_OFFSET      0x00000038u  /* P3 joystick_2 */
#define NV_JOY3_OFFSET      0x00000040u  /* P4 joystick_3 */
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

static int mem_fd = -1;
static volatile uint8_t* ddr_base = NULL;
static uint32_t frame_counter = 0;
static int active_buf = 0;

bool NativeVideoWriter_Init(void) {
    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("NativeVideoWriter: open /dev/mem");
        return false;
    }

    ddr_base = (volatile uint8_t*)mmap(NULL, NV_DDR_REGION_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, NV_DDR_PHYS_BASE);
    if (ddr_base == MAP_FAILED) {
        perror("NativeVideoWriter: mmap");
        ddr_base = NULL;
        close(mem_fd);
        mem_fd = -1;
        return false;
    }

    /* Clear both buffers and control words */
    memset((void*)(ddr_base + NV_BUF0_OFFSET), 0, NV_FRAME_BYTES);
    memset((void*)(ddr_base + NV_BUF1_OFFSET), 0, NV_FRAME_BYTES);
    volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = 0;
    volatile uint32_t* cart_ctrl = (volatile uint32_t*)(ddr_base + NV_CART_CTRL_OFFSET);
    *cart_ctrl = 0;
    volatile uint32_t* feedback = (volatile uint32_t*)(ddr_base + NV_FEEDBACK_OFFSET);
    *feedback = 0;
    volatile uint32_t* aud_wptr = (volatile uint32_t*)(ddr_base + NV_AUD_WPTR_OFFSET);
    *aud_wptr = 0;
    volatile uint32_t* aud_rptr = (volatile uint32_t*)(ddr_base + NV_AUD_RPTR_OFFSET);
    *aud_rptr = 0;
    memset((void*)(ddr_base + NV_AUD_RING_OFFSET), 0, NV_AUD_RING_SAMPLES * 4);
    frame_counter = 0;
    active_buf = 0;

    fprintf(stderr, "NativeVideoWriter: mapped 0x%08X, %d bytes per frame\n",
            NV_DDR_PHYS_BASE, NV_FRAME_BYTES);
    return true;
}

void NativeVideoWriter_Shutdown(void) {
    if (ddr_base) {
        volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
        *ctrl = 0;
        munmap((void*)ddr_base, NV_DDR_REGION_SIZE);
        ddr_base = NULL;
    }
    if (mem_fd >= 0) {
        close(mem_fd);
        mem_fd = -1;
    }
}

void NativeVideoWriter_WriteFrame(const void* rgba8_pixels, int width, int height) {
    if (!ddr_base || width != NV_FRAME_WIDTH || height != NV_FRAME_HEIGHT)
        return;

    uint32_t buf_offset = (active_buf == 0) ? NV_BUF0_OFFSET : NV_BUF1_OFFSET;
    volatile uint16_t* dst = (volatile uint16_t*)(ddr_base + buf_offset);
    const uint8_t* src = (const uint8_t*)rgba8_pixels;

    /* Convert RGBA8888 → RGB565 and write to DDR3.
     * The source is lol::u8vec4 {r, g, b, a} — 4 bytes per pixel.
     * PICO-8 only uses 16 colors so the conversion is simple. */
    int total_pixels = NV_FRAME_WIDTH * NV_FRAME_HEIGHT;
    for (int i = 0; i < total_pixels; i++) {
        uint8_t r = src[i * 4 + 0];
        uint8_t g = src[i * 4 + 1];
        uint8_t b = src[i * 4 + 2];
        dst[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    /* Flip control word — ARM write ordering on O_SYNC/MAP_SHARED memory
     * guarantees pixel data is visible before the control word update. */
    frame_counter++;
    volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = (frame_counter << 2) | (active_buf & 1);

    active_buf ^= 1;
}

bool NativeVideoWriter_IsActive(void) {
    return ddr_base != NULL;
}

uint32_t NativeVideoWriter_CheckCart(void) {
    if (!ddr_base) return 0;
    volatile uint32_t *ctrl = (volatile uint32_t *)(ddr_base + NV_CART_CTRL_OFFSET);
    uint32_t val = *ctrl;
    /* Sanity check: if value exceeds max cart size, treat as garbage */
    if (val > NV_CART_MAX_SIZE) return 0;
    return val;
}

uint32_t NativeVideoWriter_ReadCart(void* buf, uint32_t max_size) {
    if (!ddr_base || !buf) return 0;
    uint32_t file_size = NativeVideoWriter_CheckCart();
    if (file_size == 0) return 0;
    if (file_size > max_size) file_size = max_size;
    if (file_size > NV_CART_MAX_SIZE) file_size = NV_CART_MAX_SIZE;
    memcpy(buf, (const void *)(ddr_base + NV_CART_DATA_OFFSET), file_size);
    return file_size;
}

void NativeVideoWriter_AckCart(void) {
    if (!ddr_base) return;
    volatile uint32_t *ctrl = (volatile uint32_t *)(ddr_base + NV_CART_CTRL_OFFSET);
    *ctrl = 0;
}

uint32_t NativeVideoWriter_ReadJoystick(int player) {
    if (!ddr_base || player < 0 || player > 3) return 0;
    static const uint32_t joy_offsets[4] = {
        NV_JOY0_OFFSET, NV_JOY1_OFFSET, NV_JOY2_OFFSET, NV_JOY3_OFFSET
    };
    volatile uint32_t *joy = (volatile uint32_t *)(ddr_base + joy_offsets[player]);
    return *joy;
}

uint32_t NativeVideoWriter_ReadFeedback(void) {
    if (!ddr_base) return 0;
    volatile uint32_t *fb = (volatile uint32_t *)(ddr_base + NV_FEEDBACK_OFFSET);
    return *fb;
}

uint32_t NativeVideoWriter_AudioSpace(void) {
    if (!ddr_base) return 0;
    volatile uint32_t *wptr = (volatile uint32_t *)(ddr_base + NV_AUD_WPTR_OFFSET);
    volatile uint32_t *rptr = (volatile uint32_t *)(ddr_base + NV_AUD_RPTR_OFFSET);
    uint32_t w = *wptr & NV_AUD_RING_MASK;
    uint32_t r = *rptr & NV_AUD_RING_MASK;
    /* Available space = ring_size - 1 - used */
    uint32_t used = (w - r) & NV_AUD_RING_MASK;
    return NV_AUD_RING_SAMPLES - 1 - used;
}

void NativeVideoWriter_WriteAudio(const int16_t *stereo_samples, uint32_t num_samples) {
    if (!ddr_base || !stereo_samples || num_samples == 0) return;

    volatile uint32_t *wptr_reg = (volatile uint32_t *)(ddr_base + NV_AUD_WPTR_OFFSET);
    volatile int16_t *ring = (volatile int16_t *)(ddr_base + NV_AUD_RING_OFFSET);
    uint32_t wp = *wptr_reg & NV_AUD_RING_MASK;

    for (uint32_t i = 0; i < num_samples; i++) {
        uint32_t idx = (wp + i) & NV_AUD_RING_MASK;
        ring[idx * 2 + 0] = stereo_samples[i * 2 + 0];  /* Left */
        ring[idx * 2 + 1] = stereo_samples[i * 2 + 1];  /* Right */
    }

    /* Memory barrier before updating pointer — ensures samples are visible
     * to the FPGA before the write pointer advances. */
    __sync_synchronize();
    *wptr_reg = (wp + num_samples) & NV_AUD_RING_MASK;
}
