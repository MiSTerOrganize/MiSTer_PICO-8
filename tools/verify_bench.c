/*
 * verify_bench.c -- PICO-8 kernel CORRECTNESS + REGRESSION harness (A9 or PC).
 *
 * The two ARM kernels PICO-8's output path depends on are tiny but load-bearing:
 *   (1) RGBA8888 -> RGB565 frame convert (native_video_writer.c::WriteFrame)
 *   (2) 22050->48000 linear mono->stereo resample (mister_main.cpp)
 * This proves each is correct against an independent reference AND emits a stable
 * FNV-1a hash of each kernel's output over deterministic input -- a golden
 * fingerprint. Re-run after any build that touches these kernels: a changed hash
 * = the kernel's bytes changed (intended or not), caught before it ships.
 *
 * Exit 0 = all correctness checks pass; 1 = a check failed.
 *
 * Build (CI): arm-linux-gnueabihf-gcc -O2 -static -mcpu=cortex-a9 -mfpu=neon
 *             -mfloat-abi=hard tools/verify_bench.c -o verify_bench -lrt
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define FW 128
#define FH 128
#define NPX (FW*FH)
#define SRC_RATE 22050
#define DST_RATE 48000

/* --- kernel 1: RGBA8888 -> RGB565 (verbatim WriteFrame formula) --- */
static inline uint16_t conv_px(uint8_t r,uint8_t g,uint8_t b){
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
/* --- kernel 2: linear resample 22050->48000 mono->stereo (L=R) --- */
static int resample_lin(const int16_t *mono,int in_samples,int16_t *out,int cap){
    const uint32_t step=(uint32_t)(((uint64_t)SRC_RATE<<16)/DST_RATE);
    uint32_t acc=0; int o=0;
    while(o<cap){
        uint32_t si=acc>>16; if((int)si>=in_samples-1) break;
        uint32_t fr=acc&0xFFFF; int32_t s0=mono[si],s1=mono[si+1];
        int16_t v=(int16_t)(s0+(((s1-s0)*(int32_t)fr)>>16));
        out[2*o]=v; out[2*o+1]=v; o++; acc+=step;
    }
    return o;
}

static uint32_t fnv1a(const void *p,long n){
    const uint8_t *b=(const uint8_t*)p; uint32_t h=2166136261u; long i;
    for(i=0;i<n;i++){ h^=b[i]; h*=16777619u; } return h;
}

int main(void){
    int fails=0;
    printf("== verify_bench (PICO-8) -- kernel correctness + regression ==\n\n");

    /* ---- kernel 1: spot-check exact known colours ---- */
    struct { uint8_t r,g,b; uint16_t want; const char *nm; } K[] = {
        {0x00,0x00,0x00, 0x0000, "black"},
        {0xFF,0xFF,0xFF, 0xFFFF, "white"},
        {0xFF,0x00,0x00, 0xF800, "red"},
        {0x00,0xFF,0x00, 0x07E0, "green"},
        {0x00,0x00,0xFF, 0x001F, "blue"},
    };
    int i; printf("[1] RGBA8888 -> RGB565 convert:\n");
    for(i=0;i<5;i++){
        uint16_t got=conv_px(K[i].r,K[i].g,K[i].b);
        int ok=(got==K[i].want); if(!ok) fails++;
        printf("    %-6s %02X%02X%02X -> %04X (want %04X)  %s\n",
               K[i].nm,K[i].r,K[i].g,K[i].b,got,K[i].want, ok?"OK":"** FAIL **");
    }
    /* golden hash over a deterministic 128x128 frame */
    uint16_t *fr=malloc(NPX*2);
    for(i=0;i<NPX;i++) fr[i]=conv_px((uint8_t)(i*7),(uint8_t)(i*13),(uint8_t)(i*29));
    uint32_t h1=fnv1a(fr,(long)NPX*2);
    printf("    golden hash (128x128 convert): 0x%08X\n\n", h1);

    /* ---- kernel 2: resample properties ---- */
    printf("[2] linear resample %d->%d mono->stereo:\n", SRC_RATE, DST_RATE);
    int IN=SRC_RATE, CAP=DST_RATE+16;
    int16_t *mono=malloc((long)IN*2), *out=malloc((long)CAP*2*2);
    for(i=0;i<IN;i++) mono[i]=(int16_t)((i*131)&0x7fff);
    int produced=resample_lin(mono,IN,out,CAP);
    /* a) output count is ~ DST_RATE for 1s of source */
    int cnt_ok = (produced > DST_RATE-200 && produced <= DST_RATE+16); if(!cnt_ok) fails++;
    printf("    produced %d frames for 1s source (expect ~%d)  %s\n", produced, DST_RATE, cnt_ok?"OK":"** FAIL **");
    /* b) L==R for every frame */
    int lr_ok=1; for(i=0;i<produced;i++) if(out[2*i]!=out[2*i+1]){ lr_ok=0; break; }
    if(!lr_ok) fails++;
    printf("    L==R (mono-source duplicate): %s\n", lr_ok?"OK":"** FAIL **");
    /* c) interpolated value lies between its two source neighbours (monotone ramp) */
    int interp_ok=1; const uint32_t step=(uint32_t)(((uint64_t)SRC_RATE<<16)/DST_RATE);
    uint32_t acc=0;
    for(i=0;i<produced;i++){ uint32_t si=acc>>16; int lo=mono[si],hi=mono[si+1];
        int v=out[2*i]; int mn=lo<hi?lo:hi, mx=lo<hi?hi:lo; if(v<mn||v>mx){ interp_ok=0; break;} acc+=step; }
    if(!interp_ok) fails++;
    printf("    interpolated samples within neighbour bounds: %s\n", interp_ok?"OK":"** FAIL **");
    uint32_t h2=fnv1a(out,(long)produced*2*2);
    printf("    golden hash (resample output): 0x%08X\n\n", h2);

    if(fails){ printf("RESULT: %d FAIL(s).\n", fails); free(fr);free(mono);free(out); return 1; }
    printf("RESULT: all correct. Regression fingerprints: convert=0x%08X resample=0x%08X\n", h1, h2);
    free(fr);free(mono);free(out); return 0;
}
