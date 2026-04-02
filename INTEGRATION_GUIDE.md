# PICO-8 FPGA Native Video — Integration Guide

## File Placement

```
MiSTer_PICO-8/
├── src/
│   ├── mister_main.cpp              ← MODIFY (add native video calls)
│   ├── native_video_writer.h        ← NEW (ARM-side DDR3 writer header)
│   └── native_video_writer.c        ← NEW (ARM-side DDR3 writer impl)
│
└── fpga/                            ← NEW DIRECTORY (Quartus project)
    ├── rtl/
    │   ├── pico8_video_timing.sv    ← NEW (256×256 timing generator)
    │   ├── pico8_video_reader.sv    ← NEW (DDR3 reader + 2× scaling)
    │   └── pico8_video_top.sv       ← NEW (wrapper)
    ├── menu.sv                      ← FORK from 3SX vendor/Menu_MiSTer
    ├── sys/                         ← COPY from 3SX vendor/Menu_MiSTer/sys
    └── menu.qsf / menu.qpf         ← FORK from 3SX (add new .sv files)
```

## ARM-Side Integration (mister_main.cpp)

### 1. Add include

```cpp
#include "native_video_writer.h"
```

### 2. Add to CMakeLists.txt

```cmake
# After the PICO-8 executable target:
target_sources(PICO-8 PRIVATE src/native_video_writer.c)
```

### 3. Init native video after SDL init

In `main()`, after the SDL_SetVideoMode section, add:

```cpp
// ── Init native video DDR3 writer (optional, for FPGA native output) ──
bool have_native_video = NativeVideoWriter_Init();
if (have_native_video)
    fprintf(stderr, "Native video: DDR3 writer active (128x128 → 256x256)\n");
else
    fprintf(stderr, "Native video: not available, using SDL fbcon\n");
```

### 4. Replace the render section in the game loop

Find this block:

```cpp
// Render video
g_vm->render(rgba_buf);
blit_stretched(screen, rgba_buf);
SDL_UpdateRect(screen, 0, 0, SCREEN_W, SCREEN_H);
```

Replace with:

```cpp
// Render video
g_vm->render(rgba_buf);

if (have_native_video) {
    // Write 128×128 RGBA8 → DDR3 as RGB565 (FPGA reads & displays)
    NativeVideoWriter_WriteFrame(rgba_buf, PICO8_W, PICO8_H);
} else {
    // Fallback: SDL framebuffer path (HDMI via scaler)
    blit_stretched(screen, rgba_buf);
    SDL_UpdateRect(screen, 0, 0, SCREEN_W, SCREEN_H);
}
```

### 5. Shutdown

In the shutdown section at the end of `main()`:

```cpp
NativeVideoWriter_Shutdown();
```

### 6. Cart browser

The cart browser currently renders to the SDL surface. For native video mode, you'd
also need to adapt `run_cart_browser()` to write to DDR3. For v1, you can fall back
to SDL for the browser and only use native video during gameplay. Or keep both paths
active (SDL for the Linux FB, native video for the CRT).

## FPGA-Side Integration (menu.sv)

Fork 3SX's `vendor/Menu_MiSTer/menu.sv` and make these changes:

### 1. Replace native_video_top instantiation

Change `native_video_top` → `pico8_video_top` and add the three new .sv files to
the Quartus project (.qsf).

### 2. VIDEO_ARX / VIDEO_ARY

Change from 4:3 to 1:1 for PICO-8's square display:

```verilog
assign VIDEO_ARX = 13'd1;
assign VIDEO_ARY = 13'd1;
```

### 3. CONF_STR

The OSD menu string should reference PICO-8 instead of MENU/3SX:

```verilog
localparam CONF_STR = {
    "PICO8;;",
    "-;",
    "O9,Native Video,Off,On;",
    "-;",
    "V,v", `BUILD_DATE
};
```

### 4. PLL

The PLL is identical to 3SX's (50 MHz → 100/20/31.25 MHz). Copy as-is.

### 5. DDR3 mux

The DDR3 priority mux pattern is identical to 3SX. The native video reader
takes priority over the existing ddram module when enabled.

## Modeline: 256×256 @ 59.64 Hz

```
CLK_VIDEO = 31.25 MHz (PLL: 50 × 5/8)
Pixel clock = 31.25 / 4 = 7.8125 MHz
H: 256 active + 52 FP + 38 sync + 154 BP = 500 total
V: 256 active +  2 FP +  3 sync +   1 BP = 262 total

H_freq = 7,812,500 / 500 = 15,625 Hz  ← good for NTSC CRT
Refresh = 15,625 / 262   = 59.637 Hz  ← close to 60 Hz

Image: 128×128 PICO-8 doubled to 256×256 with square pixels
Blanking: generous horizontal (244 dots), tight vertical (6 lines)
```

### CRT Compatibility Note

The 6-line vertical blanking is tighter than standard NTSC (~20 lines). If a specific
CRT drops sync, increase V_BP from 1 to ~16 and reduce V_ACTIVE from 256 to 240.
This loses 8 PICO-8 rows top and bottom — not ideal but workable. All HDMI displays
and VGA-to-HDMI converters will work fine with 6-line blanking.

## DDR3 Memory Map

```
Physical address: 0x3A000000 (same region as 3SX)

Offset  Size     Purpose
0x000   4 bytes  Control word: [31:2]=frame_counter, [1:0]=active_buffer
0x100   32,768   Buffer 0 (128×128 RGB565, row-major, little-endian)
0x8100  32,768   Buffer 1

Total: ~65 KB (well within the available DDR3 space above Linux's 512MB)
```

## Build Pipeline

### ARM binary

Existing GitHub Actions CI (arm32v7/debian:bullseye-slim). Just add
`native_video_writer.c` to CMakeLists.txt. The new file uses only standard
POSIX APIs (open, mmap, memcpy) — no new dependencies.

### FPGA RBF

Requires Intel Quartus 17.0 Lite (free). 3SX's `tools/mister-wrapper/` directory
contains a Dockerized Quartus build that could be adapted. Output is `PICO-8.rbf`,
placed on the MiSTer SD card root (or `_Console/PICO-8/`).

The RBF replaces the Menu core while PICO-8 is active. It loads when the user
selects "PICO-8" from the MiSTer menu, and the ARM launcher script
(`pico-8.sh`) starts the zepto8 binary.

## Performance Budget

```
Frame period:         16.67 ms (60 fps)
zepto8 VM step:       ~3.5 ms
RGBA→RGB565 convert:  ~0.2 ms (128×128 = 16K pixels)
DDR3 write (32KB):    ~0.3 ms (uncached sequential @ ~100 MB/s)
Total render path:    ~4.0 ms
Headroom:             ~12.7 ms (76% idle — room for complex carts)
```

## What This Gives You

| Feature | Current (SDL fbcon) | With FPGA native video |
|---------|-------------------|------------------------|
| CRT analog output | No | **Yes (zero-lag)** |
| Scanlines | No | **Yes** |
| Shadow masks | No | **Yes** |
| Video filters | No | **Yes** |
| VGA direct | No | **Yes** |
| Direct video (HDMI→VGA) | No | **Yes** |
| vsync_adjust | No | **Yes** |
| OSD overlay | No | **Yes (automatic)** |
| HDMI output | Yes (via Linux FB) | Yes (via scaler) |
| Display latency | 1-2 frames (scaler) | <1 scanline (analog) |
