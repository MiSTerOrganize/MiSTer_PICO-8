/*
 * synth_bench.c -- PICO-8 (zepto8) audio-synth cost on the A9.
 *
 * *** COST-SHAPE MODEL, not a verbatim port. *** zepto8's vm::get_audio
 * (sfx.cpp:227) is entangled with sfx/music/channel engine state + instrument
 * data, so it can't be lifted into a self-contained bench. This models its
 * DOMINANT per-sample arithmetic: 4 channels x {phase advance, oscillator
 * waveform eval (the 8 PICO-8 instrument shapes), volume, mix} at 22050 Hz,
 * plus the 5512->22050 PCM linear-interp path. Use it to answer "is per-sample
 * multi-channel synth compute- or memory-bound on the A9?" -- then cross-check
 * the ABSOLUTE number against the in-engine audio cost (the audio thread / [FPS]
 * profiler). audio_bench covers the 22050->48000 glue resample separately.
 *
 * Build (CI): arm-linux-gnueabihf-gcc -O2 -static -mcpu=cortex-a9 -mfpu=neon
 *             -mfloat-abi=hard tools/synth_bench.c -o synth_bench -lrt
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define RATE 22050
#define NCHAN 4

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (double)t.tv_sec*1e9+(double)t.tv_nsec; }

/* libm-free helpers (bench.yml links -lrt only). Phases here are all >= 0. */
static inline float fabs_(float x){ return x<0.f?-x:x; }
static inline float frac_(float x){ return x-(float)((int)x); }   /* x >= 0 */

/* The 8 PICO-8 oscillator shapes over phase in [0,1). Representative formulas
 * matching the PICO-8 instrument set (triangle/tilted-saw/saw/square/pulse/
 * organ/noise/phaser) -- the point is the per-sample arithmetic mix, not exact
 * sample equality. */
static float osc(int wav, float p, uint32_t *rng){
    switch(wav & 7){
        case 0: return fabs_(p*2.f-1.f)*2.f-1.f;                 /* triangle */
        case 1: { float t=0.9f; return (p<t? p*2.f/t : (1.f-p)*2.f/(1.f-t)) - 0.5f; } /* tilted saw */
        case 2: return p*2.f-1.f;                                /* saw */
        case 3: return p<0.5f? 1.f:-1.f;                         /* square */
        case 4: return p<0.33f? 1.f:-1.f;                        /* pulse */
        case 5: return fabs_(p*4.f-2.f)-1.f + (fabs_(frac_(p*2.f)*2.f-1.f)-0.5f)*0.5f; /* organ */
        case 6: { *rng = (*rng)*1103515245u+12345u; return ((float)((*rng>>16)&0xffff)/32768.f)-1.f; } /* noise */
        default:{ float ph=frac_(p*1.005f); return (fabs_(p*2.f-1.f)+fabs_(ph*2.f-1.f))-1.f; } /* phaser */
    }
}

int main(void){
    int N = RATE;          /* 1 second of output */
    int REP = 10, r;       /* 10s per measure */
    float *out = malloc((long)N*sizeof(float));

    /* per-channel synth state */
    float phase[NCHAN]={0,0,0,0};
    float pinc[NCHAN]; int wav[NCHAN]; float vol[NCHAN];
    int c; for(c=0;c<NCHAN;c++){ pinc[c]=(110.f*(c+1))/RATE; wav[c]=c*2; vol[c]=0.6f-0.1f*c; }
    uint32_t rng=0x1234567u;

    /* PCM ring (5512Hz) for the linear-interp path, like sfx.cpp:434 */
    uint8_t pcm[5512]; int i; for(i=0;i<5512;i++) pcm[i]=(uint8_t)(i*131);
    const uint32_t pcm_step=(uint32_t)(((uint64_t)5512<<16)/RATE); uint32_t pcm_phase=0;

    printf("== synth_bench (PICO-8, A9) -- COST-SHAPE: %d-channel per-sample synth @%dHz + 5512->%d PCM linear ==\n", NCHAN, RATE, RATE);
    double best=1e30;
    for(r=0;r<3;r++){
        double t0=now_ns(); int k;
        for(k=0;k<REP;k++){
            for(c=0;c<NCHAN;c++) phase[c]=0;
            pcm_phase=0;
            for(i=0;i<N;i++){
                float mix=0.f;
                for(c=0;c<NCHAN;c++){ mix += osc(wav[c],phase[c],&rng)*vol[c]; phase[c]+=pinc[c]; if(phase[c]>=1.f)phase[c]-=1.f; }
                /* PCM channel: 5512->22050 linear interp */
                uint32_t si=pcm_phase>>16; float fr=(pcm_phase&0xffff)*(1.f/65536.f);
                float s0=(float)pcm[si]-128.f, s1=(float)pcm[(si+1)%5512]-128.f;
                mix += (s0+(s1-s0)*fr)*(1.f/128.f)*0.3f;
                pcm_phase+=pcm_step; if((pcm_phase>>16)>=5512) pcm_phase-=(uint32_t)5512<<16;
                out[i]=mix*0.25f;
            }
        }
        double dt=now_ns()-t0; if(dt<best)best=dt;
    }
    double per_sec = best/REP;                 /* ns to synth 1s of audio */
    printf("synth: %.2f ns/sample  (%d ch + PCM)\n", per_sec/N, NCHAN);
    printf("cost to synth 1s of audio: %.3f ms  =  %.3f%% of one 800MHz core\n", per_sec/1e6, per_sec/1e9*100.0);
    printf("(COST-SHAPE only -- cross-check the absolute %% vs the in-engine audio thread; audio_bench covers the 22050->48000 glue.)\n");
    free(out); return 0;
}
