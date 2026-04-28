# MiSTer PICO-8

A PICO-8 fantasy console emulator for MiSTer FPGA with native video and audio output. The FPGA handles video timing and output directly, bypassing the MiSTer scaler for zero-lag CRT support.

## Features

- **Native FPGA video output** — 256×256 @ 60.10Hz with exact NES pixel clock (5.369 MHz from NTSC colorburst crystal). 128×128 doubled to 256×256, no borders. CRT image width matches NES/SNES/Genesis exactly (47.68 µs active time)
- **Native FPGA audio output** — 48 kHz signed 16-bit stereo via DDR3 ring buffer and dual-clock DCFIFO (same audio path as NES/SNES/Genesis cores)
- **CRT support** — scanlines, shadow masks, and analog video output for CRT displays
- **MiSTer OSD integration** — load .p8 and .p8.png carts from the file browser
- **Hot-swap carts** — load a new cart from the OSD while a game is playing
- **Controller support** — d-pad, analog stick, and button mapping through MiSTer's input system
- **Auto-launch** — the emulator starts automatically when the core is loaded

## Quick Install

1. Copy `Scripts/Install_PICO-8.sh` to `/media/fat/Scripts/` on your MiSTer SD card
2. From the MiSTer main menu, go to Scripts and run **Install_PICO-8**
3. Place your `.p8` or `.p8.png` carts in `games/PICO-8/Carts/`
4. Load **PICO-8** from the console menu to play

The install script downloads and installs everything: the FPGA core, ARM binary, BIOS, and daemon.

## Manual Install

Extract the release zip to the root of your MiSTer SD card (`/media/fat/`):

```
/media/fat/
├── _Other/
│   └── PICO-8_YYYYMMDD.rbf               FPGA core (dated build)
├── docs/
│   └── PICO-8/
│       └── README.md                      Documentation
├── games/
│   └── PICO-8/
│       ├── PICO-8                         ARM binary (emulator)
│       ├── pico8_daemon.sh                Auto-launch daemon
│       ├── boot.rom                       BIOS
│       └── Carts/                         Place your .p8 and .p8.png carts here
├── logs/
│   └── PICO-8/                            Debug logs
├── saves/
│   └── PICO-8/                            Game saves (created automatically)
└── Scripts/
    └── Install_PICO-8.sh                  Install script
```

## Usage

1. Load **PICO-8** from the MiSTer console menu
2. The emulator starts automatically
3. Press the **menu button** to open the MiSTer OSD
4. Select **Load Cart** to browse and load a cart
5. Load a different cart from the OSD at any time during gameplay

## Controls

| Button              | PICO-8      |
|---------------------|-------------|
| D-pad / Analog stick | Movement   |
| A                   | O (confirm) |
| X                   | X (action)  |
| Start               | Pause       |
| Menu button         | MiSTer OSD  |

## CRT Display Notes

PICO-8 outputs **256 active vertical lines** (128 doubled) — more than the ~240 lines that consumer NTSC CRTs are calibrated to display. On a typical SCART or composite CRT, this means the top and bottom 8–16 lines of the image may be cropped by the TV's overscan. PVMs and broadcast monitors (designed for tighter overscan) usually show the full image.

**If you're losing pixels at the top/bottom of your CRT**, in order of effort:

1. **Use the OSD `H Position (CRT)` and `V Position (CRT)` options** — these shift the active area within the FPGA's timing porches by ±3 pixels. Already in the OSD; no .ini editing required.
2. **Set `vscale_border=N` in `MiSTer.ini`** (1–399 pixels) — adds a black border around the entire image to push content inside the CRT's safe area. **Only works through the MiSTer scaler** (HDMI users, Direct Video adapter users, or VGA users with `vga_scaler=1`). Won't help pure 15kHz analog out of the FPGA.
3. **Adjust your CRT's vertical size knob** — most CRTs have a V-Size service-menu setting that can compress the image vertically to fit 256 lines without cropping.
4. **Accept some edge cropping** — PICO-8 games rarely put critical UI in the outermost 8 rows, so this is often a non-issue in practice.

A future "CRT Safe Mode" toggle that renders the game at 1.75× scale (224×224 with built-in 16-pixel borders) is on the roadmap if there's demand from CRT users without scaler-path access. For now, the workarounds above cover all the realistic configurations.

## Architecture

Hybrid core: FPGA handles video/audio output and controller input, ARM CPU runs the PICO-8 emulator (zepto8).

- **ARM** renders 128×128 RGBA frames → RGB565 → DDR3
- **FPGA** reads DDR3, doubles to 256×256, outputs native video (15,746 Hz horizontal, exact NES timing)
- **Audio** — ARM writes 48 kHz S16 stereo to DDR3 ring buffer, FPGA reads and outputs via I2S/SPDIF/DAC
- **Controller** — USB → Main_MiSTer → hps_io → FPGA → DDR3 → ARM
- **Cart loading** — OSD file browser → hps_io ioctl → FPGA → DDR3 → ARM

## Building from Source

### ARM Binary (GitHub Actions)

Built automatically by CI using QEMU ARM emulation with `arm32v7/debian:bullseye-slim`. Push to `main` to trigger.

### FPGA Core (Quartus)

Requires Intel Quartus Prime Lite 17.0. Project in `fpga/`, RTL in `fpga/rtl/`, framework in `fpga/sys/` (DO NOT MODIFY sys/).

## Credits

- **zepto8** — PICO-8 emulator by Sam Hocevar (WTFPL license)
- **3SX MiSTer** — reference architecture for ARM-to-FPGA native video
- **MiSTer FPGA** — open-source FPGA retro platform

## License

GPL-3.0
