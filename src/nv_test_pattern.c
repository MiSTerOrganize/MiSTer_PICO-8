//
//  nv_test_pattern — Standalone DDR3 test pattern for PICO-8 FPGA video
//
//  Writes animated 128x128 RGB565 test patterns to DDR3 at 0x3A000000
//  to verify the FPGA native video reader independently of zepto8.
//
//  Build (on MiSTer or in Docker):
//    gcc -O2 -o nv_test_pattern nv_test_pattern.c -lrt
//
//  Run (on MiSTer with PICO-8 RBF loaded):
//    ./nv_test_pattern
//
//  What you should see:
//    - PICO-8's 16 colors in horizontal bars, scrolling down slowly
//    - 256×256 output on CRT (2× scaled by FPGA)
//    - Ctrl+C to quit (clears DDR3 control word → FPGA blanks output)
//
//  Copyright (C) 2026 MiSTer Organize — GPL-3.0
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define NV_DDR_PHYS_BASE    0x3A000000u
#define NV_DDR_REGION_SIZE  0x00020000u
#define NV_CTRL_OFFSET      0x00000000u
#define NV_BUF0_OFFSET      0x00000100u
#define NV_BUF1_OFFSET      0x00008100u
#define NV_WIDTH            128
#define NV_HEIGHT           128
#define NV_FRAME_BYTES      (NV_WIDTH * NV_HEIGHT * 2)

// PICO-8 palette in RGB565 (all 16 colors)
static const uint16_t pico8_palette[16] = {
    0x0000,  //  0: black        #000000
    0x194A,  //  1: dark blue    #1D2B53
    0x792A,  //  2: dark purple  #7E2553
    0x042A,  //  3: dark green   #008751
    0xAB86,  //  4: brown        #AB5236
    0x5AA9,  //  5: dark grey    #5F574F
    0xC618,  //  6: light grey   #C2C3C7
    0xFFF1,  //  7: white        #FFF1E8
    0xF809,  //  8: red          #FF004D
    0xFD00,  //  9: orange       #FFA300
    0xFFE4,  // 10: yellow       #FFEC27
    0x07E0,  // 11: green        #00E436
    0x2B7F,  // 12: blue         #29ADFF
    0x83B3,  // 13: indigo       #83769C
    0xFBB5,  // 14: pink         #FF77A8
    0xFDD8,  // 15: peach        #FFCCAA
};

static volatile int running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

int main(void)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        fprintf(stderr, "Run as root on MiSTer with PICO-8 RBF loaded.\n");
        return 1;
    }

    volatile uint8_t *base = (volatile uint8_t *)mmap(
        NULL, NV_DDR_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, NV_DDR_PHYS_BASE);
    if (base == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    // Clear both buffers
    memset((void *)(base + NV_BUF0_OFFSET), 0, NV_FRAME_BYTES);
    memset((void *)(base + NV_BUF1_OFFSET), 0, NV_FRAME_BYTES);

    volatile uint32_t *ctrl = (volatile uint32_t *)(base + NV_CTRL_OFFSET);
    *ctrl = 0;

    printf("nv_test_pattern: writing PICO-8 palette bars to DDR3 at 0x%08X\n", NV_DDR_PHYS_BASE);
    printf("  Frame: %dx%d RGB565 (%d bytes)\n", NV_WIDTH, NV_HEIGHT, NV_FRAME_BYTES);
    printf("  Press Ctrl+C to quit.\n");

    uint32_t frame_counter = 0;
    int active_buf = 0;
    int scroll_offset = 0;
    const uint64_t frame_ns = 1000000000ULL / 60;
    uint64_t next_frame = get_time_ns();

    while (running) {
        // Wait for next frame
        uint64_t now = get_time_ns();
        if (now < next_frame) {
            uint64_t wait = next_frame - now;
            if (wait > 2000000)
                usleep((unsigned int)((wait - 1000000) / 1000));
            while (get_time_ns() < next_frame) {}
        }
        next_frame += frame_ns;

        // Generate test pattern: 16 horizontal color bars, scrolling
        uint32_t buf_offset = (active_buf == 0) ? NV_BUF0_OFFSET : NV_BUF1_OFFSET;
        volatile uint16_t *dst = (volatile uint16_t *)(base + buf_offset);

        for (int y = 0; y < NV_HEIGHT; y++) {
            int color_idx = ((y + scroll_offset) / 8) % 16;
            uint16_t color = pico8_palette[color_idx];
            for (int x = 0; x < NV_WIDTH; x++) {
                dst[y * NV_WIDTH + x] = color;
            }
        }

        // Flip
        frame_counter++;
        *ctrl = (frame_counter << 2) | (active_buf & 1);
        active_buf ^= 1;

        // Scroll slowly (1 pixel every 4 frames)
        if ((frame_counter & 3) == 0)
            scroll_offset++;
    }

    printf("\nShutting down — clearing control word.\n");
    *ctrl = 0;
    munmap((void *)base, NV_DDR_REGION_SIZE);
    close(fd);
    return 0;
}
