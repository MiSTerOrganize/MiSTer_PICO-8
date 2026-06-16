/*
 * mem_bench.c -- A9 memory-bandwidth baseline. The FLOOR that bounds every
 * other kernel: read / write / copy throughput across sizes that span
 * L1 (32K) -> L2 (512K) -> DDR3 (multi-MB). When blend floors at 94 ns/px or
 * copy at 24 ns/px, this tells us how close that is to the memory ceiling --
 * i.e. whether more optimisation is even physically possible.
 *
 * NEON 128-bit ld/st to hit peak. Each size processes a fixed ~256 MB of total
 * traffic (iter count scales down with size), best-of-3.
 *
 * Build (CI): arm-linux-gnueabihf-gcc -O2 -static -mcpu=cortex-a9 -mfpu=neon
 *             -mfloat-abi=hard tools/mem_bench.c -o mem_bench -lrt
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <arm_neon.h>

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (double)t.tv_sec*1e9+(double)t.tv_nsec; }

/* NEON sequential read: accumulate (forces the loads to retire) */
static uint32_t bw_read(const uint8_t *b, size_t n){
    uint32x4_t acc = vdupq_n_u32(0); size_t i;
    for(i=0; i+64<=n; i+=64){
        acc = vaddq_u32(acc, vreinterpretq_u32_u8(vld1q_u8(b+i)));
        acc = vaddq_u32(acc, vreinterpretq_u32_u8(vld1q_u8(b+i+16)));
        acc = vaddq_u32(acc, vreinterpretq_u32_u8(vld1q_u8(b+i+32)));
        acc = vaddq_u32(acc, vreinterpretq_u32_u8(vld1q_u8(b+i+48)));
    }
    return vgetq_lane_u32(acc,0)+vgetq_lane_u32(acc,1)+vgetq_lane_u32(acc,2)+vgetq_lane_u32(acc,3);
}
/* NEON sequential write */
static void bw_write(uint8_t *b, size_t n){
    uint8x16_t v = vdupq_n_u8(0xA5); size_t i;
    for(i=0; i+64<=n; i+=64){ vst1q_u8(b+i,v); vst1q_u8(b+i+16,v); vst1q_u8(b+i+32,v); vst1q_u8(b+i+48,v); }
}
/* NEON sequential copy */
static void bw_copy(uint8_t *d, const uint8_t *s, size_t n){
    size_t i;
    for(i=0; i+64<=n; i+=64){ vst1q_u8(d+i,vld1q_u8(s+i)); vst1q_u8(d+i+16,vld1q_u8(s+i+16)); vst1q_u8(d+i+32,vld1q_u8(s+i+32)); vst1q_u8(d+i+48,vld1q_u8(s+i+48)); }
}

int main(void){
    size_t sizes[] = {4096, 16384, 65536, 262144, 1048576, 4194304, 16777216};
    const char *labels[] = {"4K (L1)","16K (L1)","64K (L2)","256K (L2)","1M (DDR3)","4M (DDR3)","16M (DDR3)"};
    int ns = sizeof(sizes)/sizeof(sizes[0]);
    const double TARGET = 256.0*1024*1024;  /* ~256 MB traffic per measurement */
    printf("== mem_bench (A9, NEON) -- read/write/copy bandwidth (GB/s); higher = faster ==\n");
    printf("%-12s %10s %10s %10s\n","size","read","write","copy");
    int i,r;
    for(i=0;i<ns;i++){
        size_t n = sizes[i];
        uint8_t *a = (uint8_t*)malloc(n), *b = (uint8_t*)malloc(n);
        size_t k; for(k=0;k<n;k++){ a[k]=(uint8_t)(k*7+1); b[k]=0; }
        int iters = (int)(TARGET / (double)n); if(iters<1) iters=1;
        double br=0,bw=0,bc=0,t0,dt; volatile uint32_t sink=0;
        /* read */
        bw_read(a,n); { double best=1e30; for(r=0;r<3;r++){ t0=now_ns(); int it; for(it=0;it<iters;it++) sink+=bw_read(a,n); dt=now_ns()-t0; if(dt<best)best=dt; } br=(double)n*iters/best; }
        /* write */
        bw_write(b,n); { double best=1e30; for(r=0;r<3;r++){ t0=now_ns(); int it; for(it=0;it<iters;it++) bw_write(b,n); dt=now_ns()-t0; if(dt<best)best=dt; } bw=(double)n*iters/best; }
        /* copy (counts bytes moved one-way) */
        bw_copy(b,a,n); { double best=1e30; for(r=0;r<3;r++){ t0=now_ns(); int it; for(it=0;it<iters;it++) bw_copy(b,a,n); dt=now_ns()-t0; if(dt<best)best=dt; } bc=(double)n*iters/best; }
        printf("%-12s %8.2f   %8.2f   %8.2f   (GB/s)\n", labels[i], br, bw, bc);
        (void)sink; free(a); free(b);
    }
    printf("\n(GB/s = bytes/ns. e.g. blend dest is 16-bit: 2B read + 2B write per px;\n");
    printf(" at DDR3 copy bandwidth X GB/s, the 4B/px floor is ~%s.)\n","4/X ns/px");
    return 0;
}
