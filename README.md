# MiSTer PICO-8 — Native Video FPGA Core

A PICO-8 fantasy console emulator for the MiSTer FPGA platform with native video output. The FPGA handles video timing and output directly, bypassing the MiSTer scaler for zero-lag CRT support with scanlines and shadow masks.

## Features

- **Native FPGA video output** — 256×256 @ 59.64Hz through MiSTer's native video pipeline
- **CRT support** — scanlines, shadow masks, and analog video output for CRT displays
- **MiSTer OSD integration** — load .p8 and .p8.png carts from the file browser
- **Hot-swap carts** — load a new cart from the OSD while a game is playing
- **Controller support** — d-pad, analog stick, and button mapping through MiSTer's input system
- **Auto-launch** — the emulator starts automatically when the PICO-8 core is loaded

## Quick Install

1. Copy `Scripts/Install_PICO-8.sh` to `/media/fat/Scripts/` on your MiSTer SD card
2. From the MiSTer main menu, go to Scripts and run **Install_PICO-8**
3. Done — load **PICO-8** from the console menu to play

The install script downloads and installs everything: the FPGA core, ARM binary, BIOS, and controller mapping.

## Manual Install

Extract the release zip to the root of your MiSTer SD card (`/media/fat/`). The folder structure mirrors the SD card layout:

```
/media/fat/
├── _Console/
│   └── PICO-8_YYYYMMDD.rbf                FPGA core (dated build)
├── config/
│   └── inputs/
│       └── PICO-8_input_045e_0b12_v3.map   Xbox controller map (analog stick)
├── docs/
│   └── PICO-8/
│       └── README.md                       Documentation
├── games/
│   └── PICO-8/
│       ├── PICO-8                          ARM binary (emulator)
│       ├── pico8_daemon.sh                 Auto-launch daemon
│       ├── boot.rom                        BIOS
│       └── Carts/                          Place your .p8 and .p8.png carts here
├── saves/
│   └── PICO-8/                             Game saves (created automatically)
└── Scripts/
    └── Install_PICO-8.sh                   Install script
```

After copying the files, run **Install_PICO-8** from the Scripts menu once to set up auto-launch.

## Usage

1. Load **PICO-8** from the MiSTer console menu
2. The emulator starts automatically
3. Press the **menu button** (Xbox button) to open the MiSTer OSD
4. Select **Load Cart** to browse and load .p8 or .p8.png cart files
5. You can load a different cart from the OSD at any time during gameplay

## Controls

| Xbox Controller | PICO-8 |
|----------------|--------|
| D-pad / Analog stick | Movement |
| A | O (confirm/jump) |
| X | X (shoot/action) |
| Start | Pause |
| Xbox button | MiSTer OSD menu |

## Architecture

This is a hybrid core: the FPGA handles video output and controller input, while the ARM CPU runs the PICO-8 emulator (zepto8).

- **ARM** renders 128×128 RGBA frames and writes them as RGB565 to DDR3
- **FPGA** reads frames from DDR3, doubles them to 256×256, and outputs native video
- **Controller input** flows from USB → Main_MiSTer → hps_io → FPGA → DDR3 → ARM
- **Cart loading** flows from OSD file browser → hps_io ioctl → FPGA → DDR3 → ARM

## Building from Source

### ARM Binary (GitHub Actions)

The ARM binary is built automatically by GitHub Actions using QEMU ARM emulation with `arm32v7/debian:bullseye-slim`. Push to the `main` branch to trigger a build.

### FPGA Core (Quartus)

Requires Intel Quartus Prime Lite 17.0. The Quartus project is in the `fpga/` folder with RTL source in `fpga/rtl/`. The `sys/` framework folder is not included — use the MiSTer Template framework.

### Key FPGA modules

- `PICO8.sv` — top-level emu module with CONF_STR, hps_io, DDR3 mux
- `pico8_video_timing.sv` — 256×256 @ 59.64Hz video timing generator
- `pico8_video_reader.sv` — DDR3 reader, pixel scaling, joystick write, cart loading
- `pico8_video_top.sv` — wrapper connecting timing and reader

## Credits

- **zepto8** — PICO-8 emulator by Sam Hocevar (WTFPL license)
- **3SX MiSTer** — reference architecture for ARM-to-FPGA native video (kimchiman52/3sx-mister)
- **MiSTer FPGA** — open-source FPGA retro platform

## License

GPL-3.0
