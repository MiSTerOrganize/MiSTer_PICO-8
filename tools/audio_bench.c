/*
 * audio_bench.c -- PICO-8 audio glue resample cost (mister_main.cpp::
 * upsample_mono_to_stereo). zepto8 renders 22050 Hz mono; the glue
 * LINEAR-interpolates 22050 -> 48000 and duplicates to stereo (L=R) into the
 * DDR3 ring. (This matches the engine kernel character: sfx.cpp::get_audio
 * does linear PCM interpolation, so the wrapper is linear, not ZOH -- the
 * opposite of OpenBOR, whose engine mixes NN so its glue is ZOH.)
 *
 * Audio runs off the render path, so the question is just "what fraction of a
 * core does it cost?" -- measured here vs the 48000 stereo-frames/sec budget.
 *
 * Build (CI): arm-linux-gnueabihf-gcc -O2 -static -mcpu=cortex-a9 -mfpu=neon
 *             -mfloat-abi=hard tools/audio_bench.c -o audio_bench -lrt
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define SRC_RATE 22050   /* zepto8 native */
#define DST_RATE 48000   /* FPGA audio out */

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (double)t.tv_sec*1e9+(double)t.tv_nsec; }

/* Faithful port of upsample_mono_to_stereo: linear interp + L=R stereo.
 * STEP uses the 64-bit intermediate (the negative-STEP overflow trap fix):
 *   (22050 << 16) / 48000 = 30106.  s0 + ((s1-s0)*frac)>>16, frac in 16.16. */
static int resample_lin(const int16_t *mono, int in_samples, int16_t *stereo_out, int out_cap){
    const uint32_t step = (uint32_t)(((uint64_t)SRC_RATE << 16) / DST_RATE);
    uint32_t accum = 0; int o = 0;
    while(o < out_cap){
        uint32_t src_idx = accum >> 16;
        if((int)src_idx >= in_samples - 1) break;
        uint32_t fr = accum & 0xFFFF;
        int32_t s0 = mono[src_idx];
        int32_t s1 = mono[src_idx + 1];
        int32_t sum = s0 + (((s1 - s0) * (int32_t)fr) >> 16);
        int16_t v = (int16_t)sum;
        stereo_out[2*o]   = v;   /* L */
        stereo_out[2*o+1] = v;   /* R = L (PICO-8 is mono-source) */
        o++;
        accum += step;
    }
    return o;
}

int main(void){
    int IN = SRC_RATE;                 /* 1s of source mono */
    int OUTCAP = DST_RATE + 16;        /* ~1s of output stereo */
    int REP = 10, r;                   /* 10s of audio per measure */
    int16_t *mono = malloc((long)IN*2);
    int16_t *out  = malloc((long)OUTCAP*2*2);
    int i; for(i=0;i<IN;i++) mono[i]=(int16_t)((i*131)&0x7fff);

    printf("== audio_bench (PICO-8, A9) LINEAR resample %d->%d, mono->stereo (L=R) ==\n", SRC_RATE, DST_RATE);
    int produced = resample_lin(mono,IN,out,OUTCAP);
    long out_frames = (long)produced*REP;
    double best=1e30; for(r=0;r<3;r++){ double t0=now_ns(); int k; for(k=0;k<REP;k++) resample_lin(mono,IN,out,OUTCAP); double dt=now_ns()-t0; if(dt<best)best=dt; }
    double ns_per_frame = best/out_frames;
    double ns_per_sec_audio = ns_per_frame * DST_RATE;
    printf("resample: %.2f ns per stereo frame  (%d frames/s produced)\n", ns_per_frame, produced);
    printf("cost to produce 1s of audio: %.3f ms  =  %.3f%% of one 800MHz core\n",
           ns_per_sec_audio/1e6, ns_per_sec_audio/1e9*100.0);
    printf("(verdict: audio is negligible unless this is a large %%.)\n");
    free(mono); free(out); return 0;
}
