/*
 * blit_bench.c -- PICO-8 (zepto8) sprite-raster kernel micro-benchmark for the A9.
 *
 * PICO-8's per-frame engine pixel work is the draw API rastering into the 128x128
 * 4-bit framebuffer. The hot path is api_spr -> set_pixel: read a 4bpp spritesheet
 * texel, map through draw_palette, transparency-test, then a clipped 4bpp packed
 * read-modify-write into the framebuffer. This is the PICO-8 analogue of OpenBOR's
 * blend_bench (which has no PICO-8 counterpart -- PICO-8 has no blend modes).
 *
 * Faithful port of vm::set_pixel (gfx.cpp:88) + vm::api_spr (gfx.cpp:1617): the
 * 4bpp packed framebuffer (u4mat2<128,128>), draw_palette mapping, palt
 * transparency (high nibble != 0 => transparent), clip rect, and the fillp
 * pattern path in set_pixel. Measures ns/sprite-px and sprites-per-60fps-frame.
 *
 * Build (CI): arm-linux-gnueabihf-gcc -O2 -static -mcpu=cortex-a9 -mfpu=neon
 *             -mfloat-abi=hard tools/blit_bench.c -o blit_bench -lrt
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define W 128
#define H 128

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (double)t.tv_sec*1e9+(double)t.tv_nsec; }

/* 4bpp packed plane: 2 px/byte (PICO-8 u4mat2 layout, low nibble = even x). */
static uint8_t fb[W*H/2];      /* framebuffer  */
static uint8_t gfx[W*H/2];     /* spritesheet  */
static uint8_t draw_palette[16];
/* clip rect (full screen) */
static int clip_x1=0, clip_y1=0, clip_x2=W, clip_y2=H;

static inline uint8_t plane_get(const uint8_t *p,int x,int y){
    int idx = y*W + x; uint8_t b = p[idx>>1];
    return (idx&1) ? (b>>4) : (b&0xf);
}
static inline void plane_set(uint8_t *p,int x,int y,uint8_t c){
    int idx = y*W + x; uint8_t *b = &p[idx>>1];
    if(idx&1) *b = (uint8_t)((*b & 0x0f) | (c<<4));
    else      *b = (uint8_t)((*b & 0xf0) | (c&0xf));
}

/* faithful set_pixel: clip + fillp pattern + 4bpp packed write (bit_mask=0 path). */
static inline void set_pixel(int x,int y,uint32_t color_bits){
    if(x<clip_x1 || x>=clip_x2 || y<clip_y1 || y>=clip_y2) return;
    uint8_t color = (color_bits>>16)&0xf;
    if((color_bits >> (15 - (x&3) - 4*(y&3))) & 0x1){
        if(color_bits & 0x01000000) return;          /* fillp transparency bit */
        color = (color_bits>>20)&0xf;
    }
    plane_set(fb,x,y,color);
}

/* faithful api_spr inner loop (8x8, no flip), with palette + palt transparency. */
static void spr(int n,int x,int y,uint32_t fillp){
    int i,j;
    for(j=0;j<8;j++) for(i=0;i<8;i++){
        int gx = (n%16)*8 + i, gy = (n/16)*8 + j;
        if(gx<0||gx>=W||gy<0||gy>=H) continue;
        uint8_t col = plane_get(gfx,gx,gy);
        if((draw_palette[col] & 0xf0)==0){           /* not transparent */
            uint32_t color_bits = ((uint32_t)(draw_palette[col]&0xf)<<16) | fillp;
            set_pixel(x+i, y+j, color_bits);
        }
    }
}

static uint32_t fnv1a(const void*p,long n){ const uint8_t*b=p; uint32_t h=2166136261u; long i; for(i=0;i<n;i++){h^=b[i];h*=16777619u;} return h; }

int main(int argc,char**argv){
    int r,k;
    int fillp_on = (argc>1 && strcmp(argv[1],"fillp")==0);
    /* fillp pattern lives in bits [15:0] of color_bits (set_pixel's pattern test
     * indexes `>> (15 - (x&3) - 4*(y&3))`); a checkerboard (0x5a5a) exercises the
     * fillp branch when requested. */
    uint32_t fillp = fillp_on ? 0x00005a5au : 0u;

    int i; for(i=0;i<16;i++) draw_palette[i]=(uint8_t)i;     /* identity palette */
    draw_palette[0]=0x10;                                    /* color 0 transparent (palt default) */
    for(i=0;i<W*H/2;i++){ gfx[i]=(uint8_t)(i*7+1); fb[i]=0; }

    /* Blit a full screen's worth of 8x8 sprites: 16x16 = 256 sprites = 16384 px.
     * This is one "sprite-saturated" frame. */
    const int SPX=16, SPY=16, NSPR=SPX*SPY, NPX=NSPR*64;
    int NF=600;  /* 600 frames = 10s @60 */

    printf("== blit_bench (PICO-8, A9) api_spr -> set_pixel, 4bpp 128x128 fb%s ==\n", fillp_on?" (fillp on)":"");
    /* warm + correctness/golden */
    { int sx,sy; for(sy=0;sy<SPY;sy++) for(sx=0;sx<SPX;sx++) spr((sy*SPX+sx)&0xff, sx*8, sy*8, fillp); }
    uint32_t golden = fnv1a(fb,sizeof fb);

    double best=1e30;
    for(r=0;r<3;r++){
        double t0=now_ns();
        for(k=0;k<NF;k++){ int sx,sy; for(sy=0;sy<SPY;sy++) for(sx=0;sx<SPX;sx++) spr((sy*SPX+sx)&0xff, sx*8, sy*8, fillp); }
        double dt=now_ns()-t0; if(dt<best)best=dt;
    }
    double per_frame = best/NF;
    printf("blit: %.3f ns/sprite-px  (%d sprites = %d px/frame)\n", per_frame/NPX, NSPR, NPX);
    printf("one sprite-saturated frame: %.4f ms  =  %.2f%% of a 60fps frame (16.67ms)\n",
           per_frame/1e6, per_frame/1e6/16.67*100.0);
    printf("=> A9 can blit ~%.0f such full-screen sprite passes per frame before 60fps slips\n", 16.67e6/per_frame);
    printf("golden fb hash: 0x%08X\n", golden);
    return 0;
}
