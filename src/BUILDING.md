# Building MiSTer_PICO-8 from Source

## Build for MiSTer (GitHub Actions ONLY)

**ARM binaries are built exclusively through `.github/workflows/build.yml`.** Push to `main` → CI compiles the ARM binary in an `arm32v7/debian:bullseye-slim` container via QEMU → CI commits the binary back to the repo (`games/PICO-8/PICO-8`).

There is no supported local-ARM build path. If you want a binary, you push and wait. Build time is ~17 minutes per run.

The CI pipeline:

1. Builds **SDL 1.2.15** static (used as a no-op shim — the binary sets `SDL_VIDEODRIVER=dummy` at runtime; the real video and audio paths bypass SDL entirely).
2. Compiles MiSTer_PICO-8 in the ARM container.
3. Commits the new `games/PICO-8/PICO-8` binary back to `main`.

## Architecture (hybrid FPGA+ARM core — what makes this work)

PICO-8 ships as a hybrid core: the ARM CPU runs the emulator (zepto8) while the FPGA handles **video and audio output natively**. SDL is linked but isn't doing the work — it exists only as a shim because some upstream code paths reach into SDL state.

- **Video**: zepto8 renders 128×128 RGBA frames → converted to RGB565 → written to DDR3 via `NativeVideoWriter_WriteFrame` (`src/native_video_writer.c`) → FPGA reads DDR3 and outputs native VGA/HDMI at NES-exact timing.
- **Audio**: zepto8 produces 22050 Hz mono samples → `audio_thread` upsamples to 48 kHz stereo → writes to DDR3 ring buffer via `NativeVideoWriter_WriteAudio` → FPGA reads at 48 kHz and outputs I2S/SPDIF/DAC. **No ALSA.** The dlopen-libasound runtime that earlier MiSTer ports used isn't present — audio_out.v in the FPGA framework does the final hardware conversion.
- **Input**: USB controller → MiSTer Main → hps_io → DDR3 → ARM polls DDR3 each frame (NOT SDL). SDL joystick is broken on MiSTer (Main_MiSTer calls EVIOCGRAB on the device).
- **Cart loading**: OSD file browser → hps_io ioctl → DDR3 → ARM reads via `NativeVideoWriter_ReadCart`. Cart path lives at `/media/fat/config/PICO-8.s0`.

The C++ in this repo bridges all of those — it's the ARM-side glue between zepto8 and the FPGA DDR3 interface.

## Project Structure

```
src/
├── mister_main.cpp         MiSTer frontend — audio thread, input poll, cart loading,
│                           main loop, DDR3 init, FPGA bridge
├── native_video_writer.c   DDR3 ring-buffer interface to FPGA (video + audio + cart + joystick)
├── pico8/                  zepto8 emulator core (VM, graphics, SFX, Lua API)
│   ├── vm.cpp/h            virtual machine + memory + sandbox
│   ├── gfx.cpp             pixel-level draw primitives (line, rect, spr, print, pal...)
│   ├── sfx.cpp             SFX/music synth + PCM streaming via serial(0x808)
│   ├── render.cpp          per-pixel palette lookup, raster effects
│   ├── cart.cpp            .p8/.p8.png parser, code preprocessing
│   └── bios.p8             BIOS image (Lua, runs inside sandbox)
├── 3rdparty/
│   ├── z8lua/              PICO-8-flavoured Lua (fix32 numbers, $/%/@ shorthands)
│   ├── lodepng/            PNG decoder (cart label, .p8.png extraction)
│   └── quickjs/            JS engine (used by zepto8 for some cart preprocessing)
├── synth.cpp/h             waveform synthesis (8 instruments + filters)
├── filter.cpp/h            biquad audio filters
└── lol_shim/               drop-in replacements for lolengine framework calls
                            (zepto8 originally targeted the lolengine SDK)
```

## Key Technical Notes

- **MiSTer runs Buildroot Linux** with glibc 2.31, kernel 5.15.1 — NOT Debian. Don't link against newer glibc.
- **Compiler flags**: `-mcpu=cortex-a9 -mfloat-abi=hard -mfpu=neon -Ofast`. NEON is on the Cyclone V Cortex-A9; use it.
- **SDL 1.2.15** statically linked, runtime `SDL_VIDEODRIVER=dummy`, no fbcon/X11/ALSA. SDL exists because upstream zepto8 reaches into it — we don't add new SDL code.
- **Audio thread** writes to DDR3 ring buffer via `NativeVideoWriter_WriteAudio`. **Never** add `usleep` to the audio thread — ring-buffer back-pressure paces it correctly. See `mister_main.cpp:92-`.
- **60 fps tick rate** required by PICO-8 carts. Main loop in `mister_main.cpp` spin-waits to keep frame cadence (`get_now_ms` based) — must NOT fall behind, or `_update60`/`_draw` desync.
- **Logs** go to `/media/fat/logs/PICO-8/pico8.log` (stderr `dup2`'d in `mister_main.cpp:374-379`). Never write log files to the games directory.
- **BIOS** loaded at runtime from `/media/fat/games/PICO-8/bios.p8`. Edit `src/pico8/bios.p8` in the repo, push, CI deploys via `update_all`. **Never** rename to `boot.rom` (downloader hardcodes `overwrite: False` on that filename → users get stuck on stale BIOS forever).
- **Save states** go to `/media/fat/savestates/PICO-8/<cart>_<slot>.ss`. NES-style v6 (4 slots per cart, OSD-driven, F1-F4 keyboard).

## What you can do locally (no MiSTer required)

x86 build for testing logic that doesn't touch the FPGA bridge — fix32 math, parser, Lua bindings, render LUTs, etc. Run from CMake:

```bash
sudo apt install libsdl1.2-dev
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The x86 binary uses SDL for video and audio (since there's no FPGA bridge); it's only useful for testing PICO-8 internals. It will NOT exercise the DDR3 paths, the cart-loading-via-hps_io flow, or the input-via-DDR3 polling. **An x86 build that runs is not evidence the ARM build will work** — push to CI and check.

## Reference: CI artifact

`games/PICO-8/PICO-8` is the committed binary. Every CI run produces a fresh build with a new mtime; the binary md5 changes per build (because of embedded build timestamps) even when no source changed. The DB manifest (MiSTer_Frontier) picks up the new md5 on its daily rebuild or via manual `gh workflow run "Build Custom Database" -R MiSTerOrganize/MiSTer_Frontier`.
