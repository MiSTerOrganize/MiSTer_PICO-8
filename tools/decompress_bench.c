/*
 * decompress_bench.c -- PICO-8 (zepto8) cart-load decompress throughput on the A9.
 *
 * A .p8.png cart's Lua is stored PXA-compressed; loading it runs code::decompress
 * -> pxa_decompress (code.cpp) before the z8lua compile. This isolates that
 * decompress kernel's CPU throughput. (PICO-8 carts are <=32KB code so this is a
 * small one-time cost -- the bench confirms it and guards a regression.)
 *
 * pxa_decompress + move_to_front are ported VERBATIM from src/pico8/code.cpp. The
 * 322-line pxa_compress is not portable in a self-contained bench, so a small
 * correct pxa-stream EMITTER (matching the exact bit format pxa_decompress reads:
 * LSB-first bits, variable-nbits MTF literals, offset/length backrefs) builds the
 * test stream from synthetic repetitive Lua-like text. The roundtrip is verified
 * (decompress output == the text we encoded) -- a runtime FAIL means the bit
 * format drifted, so this doubles as a decompressor regression check.
 *
 * Build (CI): arm-linux-gnueabihf-gcc -O2 -static -mcpu=cortex-a9 -mfpu=neon
 *             -mfloat-abi=hard tools/decompress_bench.c -o decompress_bench -lrt
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static double now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (double)t.tv_sec*1e9+(double)t.tv_nsec; }

/* ---- move_to_front, ported from code.cpp ---- */
typedef struct { uint8_t s[256]; } mtf_t;
static void mtf_reset(mtf_t *m){ int n; for(n=0;n<256;n++) m->s[n]=(uint8_t)n; }
static uint8_t mtf_get(mtf_t *m,int n){            /* nth byte, move to front */
    uint8_t v=m->s[n]; int i; for(i=n;i>0;i--) m->s[i]=m->s[i-1]; m->s[0]=v; return v;
}
static int mtf_find(mtf_t *m,uint8_t ch){ int i; for(i=0;i<256;i++) if(m->s[i]==ch) return i; return -1; }

/* ---- pxa_decompress, ported VERBATIM from code.cpp (no TRACE, C buffer out) ---- */
static long pxa_decompress(const uint8_t *input, uint8_t *out, long out_cap){
    size_t length = (size_t)input[4]*256 + input[5];
    size_t compressed = (size_t)input[6]*256 + input[7];
    size_t pos = (size_t)8*8;
    long n_out = 0;
    /* get_bits: LSB-first within each byte */
    #define GETB(cnt) ({ uint32_t _n=0; size_t _i; for(_i=0;_i<(size_t)(cnt) && pos<compressed*8;_i++,pos++) _n |= ((uint32_t)((input[pos>>3]>>(pos&7))&1))<<_i; _n; })
    mtf_t mtf; mtf_reset(&mtf);
    while((size_t)n_out < length && pos < compressed*8 && n_out < out_cap){
        if(GETB(1)){
            int nbits=4; while(GETB(1)) ++nbits;
            int n = (int)GETB(nbits) + (1<<nbits) - 16;
            uint8_t ch = mtf_get(&mtf, n);
            if(!ch) break;
            out[n_out++]=ch;
        } else {
            int nbits = GETB(1) ? (GETB(1) ? 5 : 10) : 15;
            int offset = (int)GETB(nbits) + 1;
            if(nbits==10 && offset==1){
                uint8_t ch=(uint8_t)GETB(8);
                while(ch){ if(n_out<out_cap) out[n_out++]=ch; ch=(uint8_t)GETB(8); }
            } else {
                int n, len=3; do { len += (n=(int)GETB(3)); } while(n==7);
                int i; for(i=0;i<len && n_out<out_cap;i++){ out[n_out]=out[n_out-offset]; n_out++; }
            }
        }
    }
    #undef GETB
    return n_out;
}

/* ---- bit-writer (LSB-first) for the test stream ---- */
static uint8_t *bw; static size_t bw_bitpos;
static void put_bit(int v){ if(v) bw[bw_bitpos>>3] |= (uint8_t)(1u<<(bw_bitpos&7)); bw_bitpos++; }
static void put_bits(uint32_t v,int cnt){ int i; for(i=0;i<cnt;i++) put_bit((v>>i)&1); }
/* emit one MTF literal with index n (0..255), variable nbits matching decompress */
static void emit_literal(int n){
    int k=4; while(!(n >= (1<<k)-16 && n <= (1<<(k+1))-17)) k++;   /* find nbits k */
    int v = n - ((1<<k)-16);
    put_bit(1);                       /* literal flag */
    int j; for(j=0;j<k-4;j++) put_bit(1);  /* raise nbits */
    put_bit(0);                       /* stop */
    put_bits((uint32_t)v, k);
}
static void emit_backref(int offset,int len){
    put_bit(0);                       /* backref flag */
    put_bit(1); put_bit(1);           /* nbits=5 (offset<=32) */
    put_bits((uint32_t)(offset-1), 5);
    int d=len-3; while(d>=7){ put_bits(7,3); d-=7; } put_bits((uint32_t)d,3);
}

int main(void){
    /* synthetic repetitive Lua-like text: a 32-byte block repeated to ~16KB */
    const char *base = "if(t>0)x+=spd t-=1 end --pico8\n";   /* 31 chars */
    int blen=(int)strlen(base);
    int reps=520;                                   /* ~16KB output */
    long outlen=(long)blen*reps;

    /* build the pxa stream: header(8) + bitstream. First block as MTF literals,
     * each subsequent block as a backref(offset=blen,len=blen). */
    size_t cap=outlen+4096; bw=calloc(cap,1); bw_bitpos=8*8;  /* skip 8-byte header */
    mtf_t em; mtf_reset(&em);
    int i; for(i=0;i<blen;i++){ uint8_t ch=(uint8_t)base[i]; int n=mtf_find(&em,ch); emit_literal(n); mtf_get(&em,n); }
    int r; for(r=1;r<reps;r++) emit_backref(blen, blen);
    size_t comp_bytes=(bw_bitpos+7)/8;
    bw[4]=(uint8_t)((outlen>>8)&0xff); bw[5]=(uint8_t)(outlen&0xff);
    bw[6]=(uint8_t)((comp_bytes>>8)&0xff); bw[7]=(uint8_t)(comp_bytes&0xff);

    uint8_t *out=malloc(outlen+16);
    /* correctness: decompress once, compare to expected */
    long got=pxa_decompress(bw,out,outlen+16);
    int ok = (got==outlen);
    if(ok) for(i=0;i<outlen;i++) if(out[i]!=base[i%blen]){ ok=0; break; }
    printf("== decompress_bench (PICO-8, A9) pxa_decompress, %ld B out from %ld B stream ==\n", outlen, (long)comp_bytes);
    if(!ok){ printf("** SELF-TEST FAIL ** (got %ld want %ld) -- pxa bit format drifted\n", got, outlen); free(bw);free(out); return 1; }
    printf("self-test: OK (roundtrip exact)\n");

    int REP=200; double best=1e30;
    for(r=0;r<3;r++){ double t0=now_ns(); int k; for(k=0;k<REP;k++) pxa_decompress(bw,out,outlen+16); double dt=now_ns()-t0; if(dt<best)best=dt; }
    double per=best/REP;
    printf("decompress: %.3f ns/out-byte  =  %.4f ms for this %ldB cart  (%.1f MB/s)\n",
           per/outlen, per/1e6, outlen, (double)outlen/per*1000.0);
    printf("(a full 32KB cart ~ %.3f ms -- one-time at load; cf z8lua compile after.)\n", per/outlen*32768.0/1e6);
    free(bw); free(out); return 0;
}
