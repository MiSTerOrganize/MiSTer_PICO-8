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
#include <time.h>

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
    /* Zero the joystick offsets so the cart's first frame doesn't read
     * leftover state from whatever was in DDR3 before the RBF loaded.
     * Without this, the cart sees a "button held" state on frame 1
     * (whatever bits happened to be set in stale DDR3), which causes
     * btnp() edge detection to miss the user's real first press —
     * symptom: user must press button TWICE on every MGL cart load
     * for the cart to register the input. The FPGA writes fresh
     * joystick state every frame, so this only matters for frame 0. */
    *(volatile uint32_t*)(ddr_base + NV_JOY0_OFFSET) = 0;
    *(volatile uint32_t*)(ddr_base + NV_JOY1_OFFSET) = 0;
    *(volatile uint32_t*)(ddr_base + NV_JOY2_OFFSET) = 0;
    *(volatile uint32_t*)(ddr_base + NV_JOY3_OFFSET) = 0;
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

/* ==========================================================================
 * FPS OVERLAY  (pause menu -> Options -> "fps display")
 *
 * Required on every hybrid core. Bottom-right, red <30 / yellow 30-59 /
 * green >=60, default OFF and reset to OFF every launch.
 *
 * WHERE THIS IS DRAWN, and why it differs from OpenBOR's:
 * OpenBOR's ARM DOWNSCALES the engine's native render to 320x224, so its
 * overlay must be drawn AFTER that squish or it gets shrunk with the game
 * image. PICO-8 is the mirror image -- the ARM writes NATIVE 128x128 and the
 * FPGA does the upscale (2x horizontal, 1.75x vertical Bresenham) -- so there
 * is no ARM-side resample to draw after. The rule generalises as "draw at the
 * last stage the ARM controls", which for both cores is right here in
 * WriteFrame. The FPGA then scales this overlay exactly like the game pixels,
 * so at 5x7 it lands about 10x12 on screen: the same apparent size as
 * OpenBOR's 5x7-at-2x, and bigger than PICO-8's own 3x5 font.
 *
 * The read-out is RENDER rate, not displayed rate. PICO-8 is vsync-paced so
 * it should sit at ~60; a number below that means frames are being missed.
 *
 * Safe during recording and replay: the overlay lives in the framebuffer, so
 * it cannot enter an input recording. It DOES appear in screenshots and would
 * change any frame-hash comparison, which is why it defaults OFF.
 * ========================================================================== */
static int nv_fps_overlay = 0;

void NativeVideoWriter_SetFpsOverlay(int on) { nv_fps_overlay = on ? 1 : 0; }
int  NativeVideoWriter_GetFpsOverlay(void)   { return nv_fps_overlay; }

/* 5x7 digits, low 5 bits per row, drawn 1:1 into the 128x128 buffer. */
static const uint8_t nv_font5x7[10][7] = {
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
};

/* A-Z, same 5x7 cell. Added for the notice overlay: the recorder needs to say
 * things like "NO RECORDING FOR THIS CART" on screen, and until now the only
 * font here was digits -- every message went to a log file the user never
 * opens while they are sitting in front of a TV. */
static const uint8_t nv_font_az[26][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x01,0x01,0x01,0x01,0x01,0x11,0x0E}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
};

/* The punctuation the messages actually use, nothing more. */
static const uint8_t nv_font_punct[7][7] = {
    {0x00,0x00,0x00,0x0E,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /* . */
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, /* : */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* ? */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* ! */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* ( */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ) */
};

/* NULL for space and for anything unmapped -- an unknown character renders as
 * a blank cell rather than dropping out, so the text keeps its shape. */
static const uint8_t* nv_glyph_rows(char c) {
    if (c >= '0' && c <= '9') return nv_font5x7[c - '0'];
    if (c >= 'A' && c <= 'Z') return nv_font_az[c - 'A'];
    if (c >= 'a' && c <= 'z') return nv_font_az[c - 'a'];
    switch (c) {
        case '-': return nv_font_punct[0];
        case '.': return nv_font_punct[1];
        case ':': return nv_font_punct[2];
        case '?': return nv_font_punct[3];
        case '!': return nv_font_punct[4];
        case '(': return nv_font_punct[5];
        case ')': return nv_font_punct[6];
        default:  return NULL;
    }
}

#define NV_GLYPH_W    5
#define NV_GLYPH_H    7
#define NV_GLYPH_GAP  1
#define NV_FPS_MARGIN 2

/* 128 px / 6 px per cell = 21 characters. Messages are wrapped to this, not
 * cropped -- a half-shown message is worse than the log line it replaces. */
#define NV_COLS       21
#define NV_NOTICE_MAX 3

static int      nv_fps_value   = 0;
static uint32_t nv_fps_frames  = 0;
static uint64_t nv_fps_last_ns = 0;

static uint64_t nv_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Recompute about twice a second -- live enough to be useful, slow enough
 * that the digits do not flicker between adjacent values as you read them. */
static void nv_fps_tick(void) {
    uint64_t now = nv_now_ns();
    nv_fps_frames++;
    if (nv_fps_last_ns == 0) { nv_fps_last_ns = now; nv_fps_frames = 0; return; }
    uint64_t dt = now - nv_fps_last_ns;
    if (dt >= 500000000ull) {
        nv_fps_value   = (int)((nv_fps_frames * 1000000000ull + dt / 2) / dt);
        if (nv_fps_value > 999) nv_fps_value = 999;
        nv_fps_frames  = 0;
        nv_fps_last_ns = now;
    }
}

static void nv_blit_rows(volatile uint16_t* dst, int gx, int gy,
                         const uint8_t* rows, uint16_t colour) {
    if (!rows) return;                       /* space / unmapped -> blank cell */
    for (int ry = 0; ry < NV_GLYPH_H; ry++) {
        uint8_t bits = rows[ry];
        int py = gy + ry;
        if (py < 0 || py >= NV_FRAME_HEIGHT) continue;
        volatile uint16_t* row = dst + (size_t)py * NV_FRAME_WIDTH;
        for (int rx = 0; rx < NV_GLYPH_W; rx++) {
            if (!(bits & (0x10 >> rx))) continue;
            int px = gx + rx;
            if (px < 0 || px >= NV_FRAME_WIDTH) continue;
            row[px] = colour;
        }
    }
}

static void nv_blit_glyph(volatile uint16_t* dst, int gx, int gy,
                          int digit, uint16_t colour) {
    const uint8_t* rows = nv_font5x7[digit];
    for (int ry = 0; ry < NV_GLYPH_H; ry++) {
        uint8_t bits = rows[ry];
        int py = gy + ry;
        if (py < 0 || py >= NV_FRAME_HEIGHT) continue;
        volatile uint16_t* row = dst + (size_t)py * NV_FRAME_WIDTH;
        for (int rx = 0; rx < NV_GLYPH_W; rx++) {
            if (!(bits & (0x10 >> rx))) continue;
            int px = gx + rx;
            if (px < 0 || px >= NV_FRAME_WIDTH) continue;
            row[px] = colour;
        }
    }
}

/* Bottom-right, colour-coded, with a black copy offset 1px so it stays
 * legible over a bright or busy scene. */
static void nv_draw_fps(volatile uint16_t* dst) {
    int v = nv_fps_value;
    int digits[3], nd = 0;
    if (v <= 0) { digits[nd++] = 0; }
    else { while (v > 0 && nd < 3) { digits[nd++] = v % 10; v /= 10; } }

    uint16_t colour = (nv_fps_value >= 60) ? 0x07E0        /* green  */
                    : (nv_fps_value >= 30) ? 0xFFE0        /* yellow */
                                           : 0xF800;       /* red    */

    int total_w = nd * NV_GLYPH_W + (nd - 1) * NV_GLYPH_GAP;
    int x0 = NV_FRAME_WIDTH  - NV_FPS_MARGIN - total_w;
    int y0 = NV_FRAME_HEIGHT - NV_FPS_MARGIN - NV_GLYPH_H;

    for (int pass = 0; pass < 2; pass++) {
        uint16_t c   = (pass == 0) ? 0x0000 : colour;
        int      off = (pass == 0) ? 1 : 0;
        for (int i = 0; i < nd; i++) {
            int gx = x0 + (nd - 1 - i) * (NV_GLYPH_W + NV_GLYPH_GAP);
            nv_blit_glyph(dst, gx + off, y0 + off, digits[i], c);
        }
    }
}

/* ==========================================================================
 * NOTICE OVERLAY
 *
 * Every message this recorder emits went to stderr -- i.e. to
 * /media/fat/logs/PICO-8/pico8.log, which nobody reads while sitting in front
 * of a TV holding a controller. So "wrong cart", "corrupt file", "recorded on
 * an older build", "you took over", and "Stop Recording failed, SD full" all
 * presented identically: the game reset and then behaved oddly.
 *
 * A notice is a short line held for a few seconds, drawn TOP-left so it never
 * collides with the bottom-right fps read-out. Same two-pass shadow, same
 * post-transform position -- it inherits the fps overlay's correctness.
 *
 * Log lines are KEPT. This is the headline; the log is the detail.
 * ========================================================================== */
static char     nv_notice_text[NV_COLS * NV_NOTICE_MAX + 1];
static uint64_t nv_notice_until_ns = 0;

void NativeVideoWriter_Notice(const char* msg, int seconds) {
    if (!msg) { nv_notice_until_ns = 0; return; }
    size_t i = 0;
    while (msg[i] && i < sizeof(nv_notice_text) - 1) {
        char c = msg[i];
        nv_notice_text[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        i++;
    }
    nv_notice_text[i] = 0;
    if (seconds <= 0) seconds = 4;
    nv_notice_until_ns = nv_now_ns() + (uint64_t)seconds * 1000000000ull;
}

/* Solid backing so the text stays readable over busy art. OpenBOR_7533 got
 * this and PICO-8 did not, which is the real notice-parity gap between the
 * cores -- the FONT already matches (5x7 here upscaled 2x/1.75x by the FPGA
 * lands ~10x12, against OpenBOR's directly-drawn 10x14). */
static void nv_fill_rect(volatile uint16_t* dst, int x0, int y0, int w, int h,
                         uint16_t colour) {
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= NV_FRAME_HEIGHT) continue;
        volatile uint16_t* row = dst + (size_t)y * NV_FRAME_WIDTH;
        for (int x = x0; x < x0 + w; x++) {
            if (x < 0 || x >= NV_FRAME_WIDTH) continue;
            row[x] = colour;
        }
    }
}

/* Word-wrap at NV_COLS. A word longer than a line is hard-broken rather than
 * dropped, so a long cart name still shows something useful. */
static void nv_draw_notice(volatile uint16_t* dst) {
    if (!nv_notice_until_ns || nv_now_ns() >= nv_notice_until_ns) return;

    /* Measure first, then paint the panel, then the text -- same two-pass
     * shape OpenBOR uses, so the panel is only as wide as the longest line. */
    {
        const char* q = nv_notice_text;
        int nlines = 0, widest = 0;
        while (*q && nlines < NV_NOTICE_MAX) {
            while (*q == ' ') q++;
            if (!*q) break;
            int t = 0, b = 0;
            while (q[t] && t < NV_COLS) { if (q[t] == ' ') b = t; t++; }
            if (q[t] && b > 0) t = b;
            if (t > widest) widest = t;
            q += t;
            nlines++;
        }
        if (nlines == 0) return;
        nv_fill_rect(dst, NV_FPS_MARGIN - 2, NV_FPS_MARGIN - 2,
                     widest * (NV_GLYPH_W + NV_GLYPH_GAP) + 4,
                     nlines * (NV_GLYPH_H + 2) + 3, 0x0000);
    }

    const char* p = nv_notice_text;
    int line = 0;
    while (*p && line < NV_NOTICE_MAX) {
        while (*p == ' ') p++;                     /* no leading blanks */
        if (!*p) break;

        int take = 0, brk = 0;
        while (p[take] && take < NV_COLS) {
            if (p[take] == ' ') brk = take;
            take++;
        }
        if (p[take] && brk > 0) take = brk;        /* break on the last space */

        int y = NV_FPS_MARGIN + line * (NV_GLYPH_H + 2);
        for (int pass = 0; pass < 2; pass++) {
            uint16_t c   = (pass == 0) ? 0x0000 : 0xFFFF;
            int      off = (pass == 0) ? 1 : 0;
            for (int i = 0; i < take; i++)
                nv_blit_rows(dst, NV_FPS_MARGIN + i * (NV_GLYPH_W + NV_GLYPH_GAP) + off,
                             y + off, nv_glyph_rows(p[i]), c);
        }
        p += take;
        line++;
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

    /* FPS overlay: after the pixel conversion, before the control-word flip,
     * so it lands in the frame the FPGA is about to scan out. */
    nv_fps_tick();
    if (nv_fps_overlay) nv_draw_fps(dst);
    /* After the fps read-out so a notice is never painted over by it. Not
     * gated on any toggle: a notice only appears when something needs saying. */
    nv_draw_notice(dst);

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

void NativeVideoWriter_KeepaliveTick(void) {
    if (!ddr_base) return;
    /* Bump the frame counter so the FPGA reader's stale-vblank detector
     * resets, but point the active-buffer bit at the LAST-written buffer.
     * After WriteFrame, active_buf has been toggled to the NEXT-write
     * target, so last-written = active_buf ^ 1.
     *
     * If we pointed at the next-to-write buffer instead, the FPGA would
     * jitter between the last-rendered frame and the previous one
     * (since we double-buffer with toggle). Pointing at last-written
     * keeps the same frame visible — frozen, not flickering. */
    frame_counter++;
    volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = (frame_counter << 2) | ((active_buf ^ 1) & 1);
}

void NativeVideoWriter_ClearScreen(void) {
    if (!ddr_base) return;
    /* Zero both buffers so the FPGA shows clean black no matter which
     * one the ctrl word points at. Then bump the frame counter so the
     * FPGA reader picks up the cleared buffer immediately (otherwise
     * it would keep showing the cached previous frame for up to a
     * vblank). */
    memset((void*)(ddr_base + NV_BUF0_OFFSET), 0, NV_FRAME_BYTES);
    memset((void*)(ddr_base + NV_BUF1_OFFSET), 0, NV_FRAME_BYTES);
    frame_counter++;
    volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = (frame_counter << 2) | (active_buf & 1);
    active_buf ^= 1;
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

uint32_t NativeVideoWriter_ReadSavestate(void) {
    if (!ddr_base) return 0;
    volatile uint32_t *ss = (volatile uint32_t *)(ddr_base + NV_SS_OFFSET);
    return *ss;
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
