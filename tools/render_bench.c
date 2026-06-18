/*
 * render_bench.c -- PICO-8 (zepto8) per-frame framebuffer-resolve kernel for the A9.
 *
 * PICO-8's per-frame video work has two halves: (1) vm::render() resolves the
 * 128x128 4bpp hardware framebuffer into a 16384-entry RGBA8888 surface, then
 * (2) native_video_writer.c::WriteFrame converts that RGBA8888 -> RGB565 and
 * writes the DDR3 ring. present_bench covers (2); THIS bench covers (1) -- the
 * one previously-unbenched piece of the per-frame video path. Run both together
 * to characterise the whole render->present chain.
 *
 * Faithful port of vm::render (render.cpp:40) + vm::pixel (render.cpp:76). Per
 * output pixel the real kernel does, inline:
 *   - read a 4-bit framebuffer nibble (2 px/byte, low nibble = even x)   = screen.get(x,y)
 *   - apply the screen-mode transform (rotation 0x84-0x87 / mirror / stretch /
 *     flip / identity branches on draw_state.screen_mode) to remap (x,y)
 *   - apply the raster-mode branch (mode 0x10 alternate-palette per-scanline,
 *     or (mode & 0x30)==0x30 per-color gradient) reading hw_state.raster.bits[y]
 *   - select a palette index: raster path -> raster.palette[], else
 *     draw_state.screen_palette[c]; normalize via & 0x8f
 *   - look up the 144-entry RGBA LUT (lut[idx&0xf for low half, 128+ for high
 *     bit] -- exactly render.cpp's lut[128+16] split) and store 4-byte RGBA
 * The LUT is rebuilt once per render() call (32 stores) -- amortised here.
 *
 * Modes benched head-to-head:
 *   plain   : screen_mode=0, raster off  -- the common fast path (identity x/y,
 *             straight screen_palette LUT). What 99% of frames cost.
 *   raster  : raster.mode=0x10 alternate-palette  -- heavy path: every scanline
 *             takes the raster.bits[y] branch + raster.palette[] lookup.
 *   rotate  : screen_mode=0x85 (rotate+swap)  -- exercises the screen-mode
 *             coordinate transform (swap + 127-x), the "very random data access"
 *             render.cpp's comment warns the 256-LUT was avoided for.
 *
 * run pinned to core 0 (the memory-fast render core):  taskset 0x01 ./render_bench
 *
 * Build (CI): arm-linux-gnueabihf-gcc -O2 -static -mcpu=cortex-a9 -mfpu=neon
 *             -mfloat-abi=hard tools/render_bench.c -o render_bench -lrt
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define W   128
#define H   128
#define NPX (W * H)              /* 16384 */

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (double)t.tv_sec*1e9+(double)t.tv_nsec; }

/* --- emulated PICO-8 hardware state (the bits vm::pixel reads) ---------------*/

/* 4bpp packed framebuffer: 2 px/byte, low nibble = even x (u4mat2<128,128>). */
static uint8_t fb[W*H/2];

/* screen_palette[c]: maps the 16 frame colours -> a palette index 0..143
 * (high bit selects the 128+ half of the LUT). normalize is & 0x8f. */
static uint8_t screen_palette[16];

/* raster state */
static uint8_t raster_mode;             /* 0=off, 0x10=alt-pal, 0x3X=gradient   */
static uint8_t raster_bits[H];          /* per-scanline select bit              */
static uint8_t raster_palette[16];      /* alternate palette                    */

/* draw_state.screen_mode (rotation/mirror/stretch/flip selector) */
static uint8_t screen_mode;

/* 144-entry RGBA LUT (render.cpp's lut[128+16]): [0..15] low half, [128..143]
 * high half. Built once per render(). 4 bytes/entry RGBA8888. */
static uint8_t lut[(128+16)*4];

static inline uint8_t normalize_palette_color(uint8_t c){ return (uint8_t)(c & 0x8f); }

static inline uint8_t fb_get(int x,int y){
    int idx = y*W + x; uint8_t b = fb[idx>>1];
    return (idx&1) ? (uint8_t)(b>>4) : (uint8_t)(b&0xf);
}

/* faithful vm::pixel: screen-mode transform + raster branch + palette select.
 * Returns the normalized palette index (0..143) to index the RGBA LUT. */
static inline uint8_t pixel(int x,int y){
    uint8_t mode = screen_mode;

    /* Apply screen mode (rotation, mirror, flip, stretch) */
    if ((mode & 0xbc) == 0x84){
        /* Rotation modes (0x84..0x87) */
        if (mode & 1){ int t=x; x=y; y=t; }
        x = (mode & 2) ? 127 - x : x;
        y = ((mode + 1) & 2) ? 127 - y : y;
    } else {
        x = (mode & 0xbd) == 0x05 ? (x < 127 - x ? x : 127 - x)   /* mirror  */
          : (mode & 0xbd) == 0x01 ? x / 2                          /* stretch */
          : (mode & 0xbd) == 0x81 ? 127 - x : x;                   /* flip    */
        y = (mode & 0xbe) == 0x06 ? (y < 127 - y ? y : 127 - y)   /* mirror  */
          : (mode & 0xbe) == 0x02 ? y / 2                          /* stretch */
          : (mode & 0xbe) == 0x82 ? 127 - y : y;                   /* flip    */
    }

    int c = fb_get(x,y);

    /* Apply raster mode */
    if (raster_mode == 0x10){
        if (raster_bits[y])
            return normalize_palette_color(raster_palette[c]);
    } else if ((raster_mode & 0x30) == 0x30){
        if ((raster_mode & 0x0f) == c){
            int c2 = (y / 8 + (raster_bits[y] ? 1 : 0)) % 16;
            return normalize_palette_color(raster_palette[c2]);
        }
    }
    return normalize_palette_color(screen_palette[c]);
}

/* faithful vm::render: rebuild LUT, then resolve all 128x128 pixels to RGBA. */
static void render_frame(uint8_t *screen){
    int x,y,c;
    /* rebuild LUT (render.cpp:44-49): 16 low + 16 high entries */
    for (c = 0; c < 16; ++c){
        /* synthetic palette colours; layout matches palette::get8 packing */
        lut[(c)*4+0]      = (uint8_t)(c*16);   lut[(c)*4+1]      = (uint8_t)(c*8);
        lut[(c)*4+2]      = (uint8_t)(c*4);    lut[(c)*4+3]      = 0xFF;
        lut[(128+c)*4+0]  = (uint8_t)(c*9);    lut[(128+c)*4+1]  = (uint8_t)(c*5);
        lut[(128+c)*4+2]  = (uint8_t)(c*3);    lut[(128+c)*4+3]  = 0xFF;
    }
    /* the LUT is indexed by the normalized 0..143 value, where 0x80 maps to 128 */
    for (y = 0; y < H; ++y)
        for (x = 0; x < W; ++x){
            uint8_t idx = pixel(x,y);
            uint8_t lidx = (uint8_t)((idx & 0x80) ? (128 + (idx & 0x0f)) : (idx & 0x0f));
            const uint8_t *src = &lut[lidx*4];
            *screen++ = src[0]; *screen++ = src[1]; *screen++ = src[2]; *screen++ = src[3];
        }
}

static uint32_t fnv1a(const void*p,long n){ const uint8_t*b=p; uint32_t h=2166136261u; long i; for(i=0;i<n;i++){h^=b[i];h*=16777619u;} return h; }

static double bench_mode(const char *name, uint8_t *screen, int NF){
    int r,k;
    render_frame(screen);                       /* warm */
    uint32_t golden = fnv1a(screen, (long)NPX*4);
    double best=1e30;
    for(r=0;r<7;r++){                            /* min-of-7 */
        double t0=now_ns();
        for(k=0;k<NF;k++) render_frame(screen);
        double dt=now_ns()-t0; if(dt<best)best=dt;
    }
    double per_frame = best/NF;
    printf("%-7s : %6.3f ns/px   %.4f ms/frame   %5.2f%% of a 60fps frame   golden 0x%08X\n",
           name, per_frame/NPX, per_frame/1e6, per_frame/1e6/16.67*100.0, golden);
    return per_frame;
}

int main(void){
    int i;
    int NF = 1000;   /* 1000 reps/measure -- 16384px is tiny, need high count for stable timing */
    uint8_t *screen = malloc((long)NPX*4);

    /* deterministic synthetic framebuffer: seed nibbles arithmetically (0..15) */
    for(i=0;i<W*H/2;i++) fb[i] = (uint8_t)((i*7+1) & 0xff);
    /* screen_palette: identity-ish (0..15), one entry uses the high-half bit */
    for(i=0;i<16;i++) screen_palette[i] = (uint8_t)i;
    screen_palette[7] = 0x83;                 /* exercise the 128+ LUT half */
    for(i=0;i<16;i++) raster_palette[i] = (uint8_t)((i+5) & 0x0f);
    for(i=0;i<H;i++)  raster_bits[i] = (uint8_t)(i & 1);   /* alternating scanlines */

    printf("== render_bench (PICO-8, A9) vm::render 4bpp 128x128 fb -> RGBA8888, %d reps/measure ==\n", NF);
    printf("(pairs with present_bench: render resolves framebuffer->RGBA, WriteFrame does RGBA->RGB565+DDR3)\n");

    /* (a) plain fast path */
    screen_mode = 0x00; raster_mode = 0x00;
    double t_plain  = bench_mode("plain",  screen, NF);

    /* (b) raster-on worst case: alternate-palette branch every scanline */
    screen_mode = 0x00; raster_mode = 0x10;
    double t_raster = bench_mode("raster", screen, NF);

    /* (c) screen-mode coordinate transform (rotate + swap) */
    screen_mode = 0x85; raster_mode = 0x00;
    double t_rotate = bench_mode("rotate", screen, NF);

    printf("=> raster-on costs %.2fx the plain path; rotate %.2fx\n",
           t_raster/t_plain, t_rotate/t_plain);
    printf("=> A9 can do ~%.0f plain render passes per 60fps frame before it slips\n", 16.67e6/t_plain);
    free(screen);
    return 0;
}
