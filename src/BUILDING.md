# Building MiSTer_PICO-8 from Source

## Requirements

- Docker with QEMU binfmt support (for ARM cross-compilation)
- Or a native ARM build environment with GCC 10+, CMake, libsdl1.2-dev

## Build for MiSTer (GitHub Actions)

Push to `main` or create a tag (`v*`) to trigger automated ARM builds. The CI pipeline:

1. Builds SDL 1.2.15 as a static library (fbcon video, ALSA audio)
2. Compiles MiSTer_PICO-8 in an `arm32v7/debian:bullseye-slim` Docker container via QEMU
3. Packages the release artifact

The ARM binary appears in GitHub Actions → Artifacts → `MiSTer-PICO-8-release`.

## Build Locally for x86 (Testing Only)

```bash
sudo apt install libsdl1.2-dev
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

This builds an x86 binary for local testing. It won't run on MiSTer — use the GitHub Actions CI for ARM builds.

## Build Locally for ARM (Docker)

```bash
docker run --rm --platform linux/arm/v7 \
  -v $(pwd):/build -w /build \
  arm32v7/debian:bullseye-slim bash -c "
    apt-get update && apt-get install -y build-essential cmake wget
    # Build SDL 1.2.15 static
    cd /tmp && wget -q https://www.libsdl.org/release/SDL-1.2.15.tar.gz
    tar xzf SDL-1.2.15.tar.gz && cd SDL-1.2.15
    ./configure --prefix=/tmp/sdl12 --disable-shared --enable-static \
      --enable-video-fbcon --disable-video-x11 --enable-alsa --quiet
    make -j\$(nproc) --quiet && make install --quiet
    # Build MiSTer_PICO-8
    cd /build && mkdir -p build-arm && cd build-arm
    cmake .. -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS='-mcpu=cortex-a9 -mtune=cortex-a9 -mfloat-abi=hard -mfpu=neon' \
      -DCMAKE_CXX_FLAGS='-mcpu=cortex-a9 -mtune=cortex-a9 -mfloat-abi=hard -mfpu=neon' \
      -DSDL_STATIC_PREFIX=/tmp/sdl12
    make -j\$(nproc)
  "
```

The ARM binary appears at `build-arm/PICO-8`.

## Project Structure

```
src/
├── mister_main.cpp     ← MiSTer frontend (video, audio, input, cart browser)
├── cart_browser.h      ← Visual cart browser with genre folder navigation
├── pico8/              ← zepto8 emulator core (VM, graphics, SFX, Lua API)
├── lol_shim/           ← Drop-in replacements for lolengine framework
├── 3rdparty/           ← z8lua (PICO-8 Lua), PEGTL parser, lodepng, quickjs
├── synth.cpp/h         ← Waveform synthesis (8 instruments + filters)
├── filter.cpp/h        ← Biquad audio filters
└── bindings/           ← Lua-to-C++ API bindings
```

## Key Technical Notes

- **MiSTer runs Buildroot Linux** with glibc 2.31, kernel 5.15.1 — NOT Debian
- **Compiler flags**: `-mcpu=cortex-a9 -mfloat-abi=hard -mfpu=neon -Ofast`
- **SDL 1.2.15** built as a static library (MiSTer doesn't have SDL installed)
- **ALSA** loaded at runtime via `dlopen("/usr/lib/libasound.so.2")`
- **Input** uses SDL state polling every frame (`SDL_GetKeyState`, `SDL_JoystickGetHat`, `SDL_JoystickGetAxis`)
- **Audio thread** uses `snd_pcm_writei` blocking mode — do NOT add `usleep`
- **60fps tick rate** required by the PICO-8 BIOS
