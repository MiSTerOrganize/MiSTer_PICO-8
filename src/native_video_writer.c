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

/* The DDR3 memory map moved to native_video_writer.h (included above), so the
 * writer and the standalone nv_test_pattern tool share ONE definition of it
 * rather than each carrying a copy that can drift. */

static int mem_fd = -1;
static volatile uint8_t* ddr_base = NULL;
static uint32_t frame_counter = 0;
static int active_buf = 0;

/* The buffer the keepalive thread may republish. Written by a publisher ONLY
 * once that buffer is completely drawn, so a keepalive tick can never point the
 * FPGA at a buffer still being filled. See NativeVideoWriter_KeepaliveTick for
 * why deriving it there instead is a race. */
static volatile int nv_last_published = 0;

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
    nv_last_published = 0;

    /* Both buffers were just zeroed, so any notice band painted into them went
     * with it. Drop the message (a re-init is a fresh start) and record that
     * neither buffer holds a band -- otherwise the row skip would protect a
     * hole the cart can never cover. */
    NativeVideoWriter_Notice(NULL, 0);
    NativeVideoWriter_NoticeRepaint();

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
/* 32-BIT MILLISECONDS, mirroring OpenBOR_7533. A naturally-aligned 32-bit
 * access is single-copy atomic on ARM32; a uint64_t is not.
 *
 * PICO-8 is immune to the tear TODAY -- verified, not assumed: every
 * NativeVideoWriter_Notice call site is in mister_main.cpp on the main thread,
 * and neither audio_thread_func nor the keepalive lambda contains one. The
 * width is here so it STAYS immune. The failure it forecloses is not subtle:
 * a torn read lands the deadline far in the future, nv_notice_rows_now() never
 * returns 0 again, and the top rows hold one frozen message for the rest of the
 * session. Anyone adding a notice from a thread should not have to know that.
 *
 * 🛑 Not a seqlock (assumes one writer) and not __atomic_load_n on 8 bytes (can
 * emit a libatomic call this build does not link). See the OpenBOR copy. */
static volatile uint32_t nv_notice_until_ms = 0;   /* 0 == no notice live */

/* No notice runs longer than this; anything further out cannot be real. */
#define NV_NOTICE_MAX_MS  30000u

/* THE NOTICE IS STATIC: written into each buffer ONCE and then left alone.
 *
 * nv_notice_rows is the full-width band at the top of the frame the notice
 * occupies. While a notice is live the per-frame game copy starts at that row
 * instead of 0 (NativeVideoWriter_NoticeRows), so nothing rewrites those
 * pixels, and nv_notice_painted_gen records which buffers already have them.
 *
 * This is the fix for the notice flicker, reported on OpenBOR 2026-08-05 and on
 * PICO-8 2026-08-06 -- it is a hybrid-core-wide defect, not a per-core quirk,
 * because every core has the same shape: write game pixels over the WHOLE
 * frame, then repaint the overlays, then publish. A scan landing between those
 * two writes shows game pixels and no notice. THAT repaint is the race.
 *
 * The model is the letterbox border, which never flickers at any frame rate
 * because it is written once and the per-frame copy never touches it again.
 *
 * Painting once per buffer still leaves a two-frame exposure at the moment a
 * notice appears; that is once, against a repaint on every frame for the
 * notice's whole lifetime, and it is the exposure the game image already has.
 *
 * The band is full width, not just the text panel: the skipped rows keep
 * whatever was in them when the notice went up, so anything not painted would
 * be a frozen strip of stale game image beside the text.
 *
 * Tracked by GENERATION rather than a painted flag so that a Notice() arriving
 * while a frame is mid-publish cannot mark the NEW message as already painted
 * and leave the OLD one on screen for its full duration. A stale generation
 * cannot match, so the next frame repaints. Generation 0 means "no notice". */
/* 🛑 volatile, and PAIRED with nv_notice_until_ms below.
 *
 * These two are written together by the swap thread and read together by
 * the engine thread every frame. Making only the deadline volatile left the
 * height as a plain load the compiler may cache (-flto) and the CPU may
 * reorder on weakly-ordered ARM32 -- so the reader could pair the NEW
 * deadline with the PREVIOUS message's height, skipping the wrong number of
 * rows: stale pixels below the band, or a black strip the paint never
 * covers. That is the artifact class the static-notice design exists to
 * eliminate.
 *
 * The writer's __sync_synchronize() is a release; without the acquire in
 * nv_notice_rows_now() it guarantees nothing to the reader. */
static volatile int nv_notice_rows = 0;
static volatile unsigned nv_notice_gen = 0;
static unsigned nv_notice_painted_gen[2] = { 0, 0 };

/* Advance *p past leading spaces and return how many characters fit on one
 * line, breaking at the last space rather than mid-word. 0 at end of text.
 *
 * ONE implementation, used by both the measure pass in Notice() and the draw
 * pass in nv_paint_notice_band(). They were separate copies of the same loop;
 * the band height is exactly what the copy path skips, so a drift between them
 * would size the band for text it does not hold. */
static int nv_wrap_take(const char** p) {
    const char* q = *p;
    int take = 0, brk = 0;
    while (*q == ' ') q++;
    *p = q;
    if (!*q) return 0;
    while (q[take] && take < NV_COLS) {
        if (q[take] == ' ') brk = take;
        take++;
    }
    if (q[take] && brk > 0) take = brk;
    return take;
}

/* 0 when no notice is live. The publisher starts its copy at this row. */
static uint32_t nv_now_ms(void) {
    return (uint32_t)(nv_now_ns() / 1000000ull);
}

static int nv_notice_rows_now(void) {
    const uint32_t until = nv_notice_until_ms;   /* read once */
    int32_t rem;
    if (!until) return 0;
    /* SIGNED difference, not `now >= until`: wrap-correct (the time_after
     * idiom) across the 49-day rollover of the millisecond counter. */
    rem = (int32_t)(until - nv_now_ms());
    if (rem <= 0) return 0;
    if ((uint32_t)rem > NV_NOTICE_MAX_MS) return 0;   /* cannot be a real deadline */
    /* ACQUIRE side of the writer's release. Ordered AFTER the deadline read, so
     * a live deadline implies the height published with it. */
    __sync_synchronize();
    return nv_notice_rows;
}

int NativeVideoWriter_NoticeRows(void) { return nv_notice_rows_now(); }

/* Forget which buffers hold the band, so a live notice is painted again.
 * Call after ANYTHING wipes a frame buffer -- the band is written once and then
 * protected by the row skip, so a wipe we are not told about leaves a black bar
 * with nothing in it until the notice expires. ClearScreen does this; Init
 * cancels the notice outright, which is stronger. */
void NativeVideoWriter_NoticeRepaint(void) {
    nv_notice_painted_gen[0] = 0;
    nv_notice_painted_gen[1] = 0;
}

/* NO WRITER LOCK HERE, and that is deliberate -- do not "restore parity" with
 * OpenBOR by adding one.
 *
 * OpenBOR's copy takes a mutex because it has TWO writer threads: the engine
 * thread and a dedicated swap thread that calls Notice() from the .s1 poll and
 * from every refusal inside mrec_arm_slot_play. Two overlapping calls there
 * splice the text, and the line count measured from that text is the band
 * height every frame-copy path skips.
 *
 * This core has one writer. Every caller -- main(), p8rec_write, p8rec_load,
 * p8rec_note_cart_file -- runs on the engine thread; the audio pthread and the
 * keepalive std::thread never call it. The difference is architectural, the
 * same one that made the two recorder desync bugs invert between the cores:
 * PICO-8 hot-swaps IN-PROCESS on the main loop, so there is no swap thread to
 * race with. A lock here would be contention guarding nothing.
 *
 * If this core ever grows a second thread that reports to the user, this
 * becomes a real hazard and the lock comes with it. */
void NativeVideoWriter_Notice(const char* msg, int seconds) {
    if (!msg) { nv_notice_until_ms = 0; return; }
    size_t i = 0;
    while (msg[i] && i < sizeof(nv_notice_text) - 1) {
        char c = msg[i];
        nv_notice_text[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        i++;
    }
    nv_notice_text[i] = 0;
    if (seconds <= 0) seconds = 4;

    /* Measure the wrap ONCE here rather than per frame in the draw: the band
     * height is what the copy skips, so it is a stored fact about this message.
     * lines*(GLYPH_H+2) + 3 spans row 0 to the bottom of the old text panel. */
    {
        const char* q = nv_notice_text;
        int lines = 0, take;
        while (lines < NV_NOTICE_MAX && (take = nv_wrap_take(&q)) > 0) {
            q += take;
            lines++;
        }
        if (lines == 0) { nv_notice_until_ms = 0; return; }
        {   /* clamp in a LOCAL, publish once: nv_notice_rows is volatile, so a
             * two-step store makes the unclamped value observable. */
            int r = lines * (NV_GLYPH_H + 2) + 3;
            if (r > NV_FRAME_HEIGHT) r = NV_FRAME_HEIGHT;
            nv_notice_rows = r;
        }
    }

    /* nv_notice_rows is set above and read by the same consumer. Order it
     * BEFORE the deadline behind a barrier, so a reader seeing a live deadline
     * necessarily sees the height that belongs to it. */
    __sync_synchronize();
    {
        uint32_t until = nv_now_ms() + (uint32_t)seconds * 1000u;
        if (!until) until = 1u;   /* 0 is the "no notice" sentinel */
        nv_notice_until_ms = until;
    }

    /* Bump the generation LAST, behind a barrier: it is what triggers the
     * repaint, so it must change only once the text and height it refers to
     * are in place. Skip 0 on wrap -- 0 means "this buffer holds no notice". */
    __sync_synchronize();
    if (++nv_notice_gen == 0) nv_notice_gen = 1;
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

/* Paint the band into ONE buffer. Word-wrapped at NV_COLS, not cropped: a word
 * longer than a line is hard-broken rather than dropped, so a long cart name
 * still shows something useful.
 *
 * Called once per buffer per notice, never per frame -- see nv_notice_rows. */
static void nv_paint_notice_band(volatile uint16_t* dst, int rows) {
    const char* p = nv_notice_text;
    int line = 0, take;

    nv_fill_rect(dst, 0, 0, NV_FRAME_WIDTH, rows, 0x0000);

    while (line < NV_NOTICE_MAX && (take = nv_wrap_take(&p)) > 0) {
        int y = NV_FPS_MARGIN + line * (NV_GLYPH_H + 2);
        /* Shadow pass kept: the band is black so the white glyphs already have
         * contrast, but the offset copy keeps them readable if a future caller
         * ever draws a notice without the band behind it. */
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

/* Which of the two frame buffers `dst` is, or -1 if it is neither.
 *
 * Derived from the POINTER rather than passed in: the pointer IS the buffer, so
 * it cannot drift out of step with the caller's own active_buf bookkeeping. */
static int nv_buf_index(volatile uint16_t* dst) {
    const volatile uint8_t* p = (const volatile uint8_t*)dst;
    if (!ddr_base) return -1;
    if (p == ddr_base + NV_BUF0_OFFSET) return 0;
    if (p == ddr_base + NV_BUF1_OFFSET) return 1;
    return -1;
}

/* Overlays for a frame about to be published: the fps read-out every frame,
 * the notice band only into a buffer that does not already hold it. */
static void nv_draw_overlays(volatile uint16_t* dst) {
    /* The fps read-out is redrawn every frame and so STILL drops on the odd
     * frame at high render rates, exactly as the notice used to. Accepted, and
     * a COSTED CHOICE rather than an impossibility.
     *
     * 🛑 It is NOT exempt because "it changes every frame". nv_fps_value is
     * recomputed only twice a second, so the same digits are drawn for ~30
     * consecutive frames at 60 fps -- it could be made static like the notice.
     * It is not, because it is a bottom-RIGHT rectangle rather than a
     * full-width band: skipping a rectangle needs per-row X guards in every
     * copy path, and the cheap band form would put a permanent black bar over
     * rows 119-127, 7% of the frame, the whole time the overlay is on. */
    nv_fps_tick();
    if (nv_fps_overlay) nv_draw_fps(dst);

    {
        int rows = nv_notice_rows_now();
        if (rows > 0) {
            unsigned gen = nv_notice_gen;
            int      idx = nv_buf_index(dst);
            if (idx < 0) {
                /* Not one of our buffers -- should not happen. Repaint every
                 * frame rather than skip: that is the old behaviour, never
                 * worse. */
                nv_paint_notice_band(dst, rows);
            } else if (nv_notice_painted_gen[idx] != gen) {
                nv_paint_notice_band(dst, rows);
                /* Record the generation READ ABOVE, not the current one: if
                 * Notice() bumped it mid-paint this stores a stale value, which
                 * cannot match next frame, so the buffer is repainted with the
                 * new message instead of being left holding the old one. */
                nv_notice_painted_gen[idx] = gen;
            }
        }
    }

    /* Expiry needs nothing here. The band simply stops being protected --
     * NoticeRows() returns 0, the copy covers the whole frame again, and the
     * cart paints over it. */
}

void NativeVideoWriter_WriteFrame(const void* rgba8_pixels, int width, int height) {
    if (!ddr_base || width != NV_FRAME_WIDTH || height != NV_FRAME_HEIGHT)
        return;

    uint32_t buf_offset = (active_buf == 0) ? NV_BUF0_OFFSET : NV_BUF1_OFFSET;
    volatile uint16_t* dst = (volatile uint16_t*)(ddr_base + buf_offset);
    const uint8_t* src = (const uint8_t*)rgba8_pixels;

    /* Convert RGBA8888 → RGB565 and write to DDR3.
     * The source is lol::u8vec4 {r, g, b, a} — 4 bytes per pixel.
     * PICO-8 only uses 16 colors so the conversion is simple.
     *
     * Starts BELOW the notice band, not at 0. Those rows already hold the
     * notice, drawn once, and copying cart pixels over them every frame is
     * precisely what made the notice flicker. The copy is a flat run over the
     * whole frame here (no per-row loop, since the FPGA does the 2x/1.75x
     * scaling), so the band is simply the first nv_top*WIDTH pixels.
     *
     * 🛑 This evaluates the expiry independently of nv_draw_overlays() below,
     * and that is DELIBERATE -- do not "fix" it into a single hoisted read.
     * OpenBOR carries the identical shape and the same adjudication (see the
     * note at the end of its NativeVideoWriter_DrawOverlaysAt): whichever way
     * the copy and the overlay straddle the expiry instant is benign -- one
     * frame keeps the band a moment longer, or loses it a moment early, and
     * neither shows stale or half-written pixels. It is recorded here because
     * an audit re-raised it as a PICO-8-only divergence, which it is not.
     *
     * The "read ONCE per frame" rule next to OpenBOR's copy is a DIFFERENT
     * case and still holds: there, two COPY paths write an intermediate and
     * read it back, so disagreeing within a frame leaves a band of last
     * frame's averaged pixels. That one is not benign. This one is. */
    int total_pixels = NV_FRAME_WIDTH * NV_FRAME_HEIGHT;
    int first_pixel  = nv_notice_rows_now() * NV_FRAME_WIDTH;

    /* Rotation (OSD "Rotate", for a physically rotated monitor). Done here
     * rather than in the video reader: that reads a source LINE as one DDR3
     * burst, and a 90-degree turn needs a source COLUMN per output line, which
     * would mean shredding the burst pattern or buffering the frame in M10K --
     * surgery on the one path whose timing is already closed. The frame is
     * square and is being copied anyway, so here it is just an index change.
     *
     * The signal stays landscape; the IMAGE turns. That is what a rotated
     * monitor wants, and it leaves the CRT timing contract alone. */
    int rot = (int)(NativeVideoWriter_ReadMisc() & 0x3u);
    if (rot == 0) {
        for (int i = first_pixel; i < total_pixels; i++) {
            uint8_t r = src[i * 4 + 0];
            uint8_t g = src[i * 4 + 1];
            uint8_t b = src[i * 4 + 2];
            dst[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    } else {
        const int W = NV_FRAME_WIDTH, H = NV_FRAME_HEIGHT;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                int dx, dy;
                if (rot == 1)      { dx = H - 1 - y; dy = x; }          /* 90 CW  */
                else if (rot == 2) { dx = y;         dy = W - 1 - x; }  /* 90 CCW */
                else               { dx = W - 1 - x; dy = H - 1 - y; }  /* 180    */
                int di = dy * W + dx;
                /* 🛑 tested on the DESTINATION index: after rotation a source
                 * row is not a destination row, and writing into the notice
                 * band is what the static-notice rule forbids. */
                if (di < first_pixel) continue;
                int si = y * W + x;
                uint8_t r = src[si * 4 + 0];
                uint8_t g = src[si * 4 + 1];
                uint8_t b = src[si * 4 + 2];
                dst[di] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }

    /* Overlays: after the pixel conversion, before the control-word flip, so
     * they land in the frame the FPGA is about to scan out. */
    nv_draw_overlays(dst);

    /* Flip control word — ARM write ordering on O_SYNC/MAP_SHARED memory
     * guarantees pixel data is visible before the control word update.
     *
     * Hand the finished buffer to the keepalive BEFORE publishing it, so a tick
     * landing anywhere around here republishes a fully-drawn buffer -- this one
     * or the previous one. Both are complete; neither is the one we fill next. */
    nv_last_published = active_buf & 1;
    __sync_synchronize();

    uint32_t fc = __sync_add_and_fetch(&frame_counter, 1);
    volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = (fc << 2) | (active_buf & 1);

    active_buf ^= 1;
}

bool NativeVideoWriter_IsActive(void) {
    return ddr_base != NULL;
}

void NativeVideoWriter_KeepaliveTick(void) {
    if (!ddr_base) return;
    /* Bump the frame counter so the FPGA reader's stale-vblank detector resets,
     * pointing the active-buffer bit at the LAST COMPLETED buffer so the same
     * frame stays visible -- frozen, not flickering.
     *
     * 🛑 Take that buffer from nv_last_published. Do NOT derive it here as
     * `active_buf ^ 1`, which is what this did until 2026-08-06. This runs on
     * its own thread, so active_buf can toggle between the read and the ctrl
     * write, and the derived value then names the buffer WriteFrame is ABOUT TO
     * FILL -- publishing an undrawn buffer. On OpenBOR that was measured as a
     * real contributor to the notice flicker (every frame inside a flicker gap
     * came from this path) and fixed the same way; PICO-8 kept the racy form,
     * which is exactly the kind of drift the parity check exists to catch.
     *
     * frame_counter is shared with WriteFrame across threads, so bump it
     * atomically: a plain ++ can drop an increment. The FPGA only needs the
     * value to CHANGE, but there is no reason to leave the race in. */
    uint32_t fc = __sync_add_and_fetch(&frame_counter, 1);
    volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = (fc << 2) | (nv_last_published & 1);
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

    /* Both buffers were just zeroed, so any notice band painted into them went
     * with it. Say so, or the row skip protects a hole the cart can never
     * cover: a black bar with nothing in it until the notice expires. */
    NativeVideoWriter_NoticeRepaint();

    nv_last_published = active_buf & 1;
    __sync_synchronize();
    uint32_t fc = __sync_add_and_fetch(&frame_counter, 1);
    volatile uint32_t* ctrl = (volatile uint32_t*)(ddr_base + NV_CTRL_OFFSET);
    *ctrl = (fc << 2) | (active_buf & 1);
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

/* Mouse, as published by the FPGA at NV_MOUSE_OFFSET. Position is already
 * absolute and clamped to the 0..127 PICO-8 screen, so the caller can pass it
 * straight to vm::mouse() without integrating anything. */
uint32_t NativeVideoWriter_ReadMouse(void) {
    if (!ddr_base) return 0;
    volatile uint32_t *m = (volatile uint32_t *)(ddr_base + NV_MOUSE_OFFSET);
    return *m;
}

/* OSD config published by the FPGA -- status bits live on that side, so this is
 * the only way the ARM can see them. [1:0] is rotation. */
uint32_t NativeVideoWriter_ReadMisc(void) {
    if (!ddr_base) return 0;
    volatile uint32_t *m = (volatile uint32_t *)(ddr_base + NV_MISC_OFFSET);
    return *m;
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

/* OSD "Replay Slot" / "Play Replay" -> ARM. Raw word; the caller decodes.
 *   bits [1:0]  cmd   0 = idle, 1 = play
 *   bits [10:8] slot  0..7
 *   bits [23:16] seq  bumped by the FPGA on every captured Play pulse, so a
 *                     repeat of the SAME slot still reads as a new event. */
uint32_t NativeVideoWriter_ReadReplay(void) {
    if (!ddr_base) return 0;
    volatile uint32_t *rs = (volatile uint32_t *)(ddr_base + NV_REPLAY_OFFSET);
    return *rs;
}

/* Pause-menu slot -> OSD. The FPGA edge-detects `seq` and pushes the new slot
 * into the OSD's status word, which is what makes the two pickers show the
 * same number.
 *
 * `slot` is the user-facing 1..8; it is converted to the 0-based wire value
 * here so exactly one place in the codebase knows about the offset.
 *
 * 🛑 THE SEQUENCE IS DERIVED FROM THE WIRE, NEVER FROM A COUNTER, and the
 * caller does not get to supply it. This used to take a `seq` argument fed by
 * a function-local `static` in the caller -- which restarts at 0 in every new
 * process, while 0x04 SURVIVES the _exit()/respawn that Record and Play both
 * perform. So a fresh process could publish a value already sitting on the
 * wire, and the FPGA -- being an edge detector on this byte -- saw no change
 * at all. The publish was dropped, the poll then adopted the unchanged echo
 * and REVERTED the user's press, and the next Record wrote to the wrong slot,
 * destroying whatever take was in it.
 *
 * Reading back is sound: the FPGA only ever READS 0x04, so this byte is
 * whatever the ARM last wrote (or stale DDR3 at first boot, which is equally
 * fine to increment from). Being one-more-than-what-is-there cannot collide
 * with what is there. */
void NativeVideoWriter_PublishReplaySlot(int slot) {
    if (!ddr_base) return;
    if (slot < 1) slot = 1;
    if (slot > 8) slot = 8;
    volatile uint32_t *pub = (volatile uint32_t *)(ddr_base + NV_REPLAY_PUB_OFFSET);
    uint32_t next = ((*pub >> 8) + 1u) & 0xFFu;
    *pub = (uint32_t)((slot - 1) & 7) | (next << 8);
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
