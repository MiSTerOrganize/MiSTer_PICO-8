/*
 * present_bench.c -- PICO-8 frame-present cost (native_video_writer.c::WriteFrame).
 *
 * PICO-8's per-frame ARM kernel is just the 128x128 RGBA8888 -> RGB565 convert
 * + write to the DDR3 ring. There is NO ARM-side scale: the FPGA does the
 * 128->256x224 upscale (2x horizontal + Bresenham 4/7 vertical) in the video
 * reader, so OpenBOR's squish/blend benches have no ARM counterpart here.
 * zepto8's framebuffer is delivered as lol::u8vec4 {r,g,b,a} (4 B/px); only 16
 * colours are ever used, so the convert is a plain shift-pack (no palette LUT).
 *
 * This bench answers "is WriteFrame ever a per-frame cost worth caring about?"
 * (PICO-8 is vsync-locked at 60, so the expected answer is "no, it's a sliver"
 * -- this confirms it and guards against a regression that makes it not so).
 *
 * Mode 1 (default, safe any state): cached convert compute of one 128x128 frame.
 *   Measures PRESENT COMPUTE + the 64KB RGBA source read (fits L2).
 * Mode 2 (ddr3 <hexaddr>): mmap /dev/mem, time the uncached write of one
 *   128x128x2 = 32KB frame to <hexaddr>. *** MENU ONLY *** -- writing the live
 *   FPGA ring (0x3A000100 / 0x3A008100) corrupts the display; pass a scratch
 *   DDR3 phys address you trust.
 *
 * Build (CI): arm-linux-gnueabihf-gcc -O2 -static -mcpu=cortex-a9 -mfpu=neon
 *             -mfloat-abi=hard tools/present_bench.c -o present_bench -lrt
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define FW 128
#define FH 128
#define NPX (FW * FH)            /* 16384 */
#define FBYTES (NPX * 2)         /* 32768 */

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (double)t.tv_sec*1e9+(double)t.tv_nsec; }

/* RGBA8888 -> RGB565, exactly as native_video_writer.c::WriteFrame:
 *   dst = ((r>>3)<<11) | ((g>>2)<<5) | (b>>3)   (alpha ignored) */
static void present_convert(const uint8_t *rgba, uint16_t *dst){
    int i;
    for(i=0;i<NPX;i++){
        uint8_t r = rgba[i*4+0], g = rgba[i*4+1], b = rgba[i*4+2];
        dst[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

int main(int argc,char**argv){
    int NF=600, i,r;   /* 600 frames per measure = 10s @60fps */
    /* Mode 2: ddr3 <hexaddr> */
    if(argc>1 && strcmp(argv[1],"ddr3")==0){
        if(argc<3){ printf("usage: present_bench ddr3 <hexaddr>\n"); return 2; }
        unsigned long pa = strtoul(argv[2],0,16);
        int fd = open("/dev/mem", O_RDWR|O_SYNC);
        if(fd<0){ perror("open /dev/mem"); return 1; }
        size_t pg = sysconf(_SC_PAGESIZE), off = pa & (pg-1);
        void *m = mmap(0, FBYTES+off, PROT_READ|PROT_WRITE, MAP_SHARED, fd, pa-off);
        if(m==MAP_FAILED){ perror("mmap"); close(fd); return 1; }
        volatile uint16_t *ring = (uint16_t*)((char*)m+off);
        uint16_t *fr = malloc(FBYTES); for(i=0;i<NPX;i++) fr[i]=(uint16_t)(i*0x1234);
        printf("== present_bench (PICO-8) DDR3-write (MENU ONLY) @0x%lx, %d frames of %dx%d ==\n", pa, NF, FW, FH);
        double best=1e30; for(r=0;r<3;r++){ double t0=now_ns(); int f; for(f=0;f<NF;f++) memcpy((void*)ring, fr, FBYTES); double dt=now_ns()-t0; if(dt<best)best=dt; }
        double perfr = best/NF;
        printf("uncached DDR3 write: %.4f ms/frame (%.2f GB/s)\n", perfr/1e6, (double)FBYTES/perfr);
        munmap(m,FBYTES+off); close(fd); free(fr); return 0;
    }
    /* Mode 1: cached convert compute */
    uint8_t  *rgba = malloc((long)NPX*4);
    uint16_t *dst  = malloc(FBYTES);
    for(i=0;i<NPX;i++){ rgba[i*4+0]=(uint8_t)(i*7); rgba[i*4+1]=(uint8_t)(i*13); rgba[i*4+2]=(uint8_t)(i*29); rgba[i*4+3]=0xFF; }
    printf("== present_bench (PICO-8, A9) RGBA8888 -> RGB565 convert, %dx%d (no ARM scale; FPGA upscales) ==\n", FW, FH);
    present_convert(rgba,dst);
    double best=1e30; for(r=0;r<3;r++){ double t0=now_ns(); int f; for(f=0;f<NF;f++) present_convert(rgba,dst); double dt=now_ns()-t0; if(dt<best)best=dt; }
    double perfr=best/NF;
    printf("present compute: %.3f ns/px  =  %.4f ms/frame  =  %.3f%% of one 60fps frame (16.67ms)\n",
           perfr/NPX, perfr/1e6, perfr/1e6/16.67*100.0);
    printf("(source read = %dx%dx4 = %dKB/frame (fits L2); real present also writes\n", FW,FH,(NPX*4)/1024);
    printf(" %dKB to the uncached DDR3 ring -- run 'present_bench ddr3 <addr>' from MENU.)\n", FBYTES/1024);
    free(rgba); free(dst); return 0;
}
