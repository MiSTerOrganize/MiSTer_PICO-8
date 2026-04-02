# MiSTer_PICO-8

A [PICO-8](https://www.lexaloffle.com/pico-8.php) emulator for **MiSTer FPGA**, powered by [zepto8](https://github.com/samhocevar/zepto8).

Includes a built-in cart browser with genre folder navigation, full controller support, and accurate audio synthesis.

## Installation

### Option A — Automatic (Recommended)

1. Copy `Scripts/Install_pico-8.sh` to `/media/fat/Scripts/` on your MiSTer SD card
2. On MiSTer: **F12 → Scripts → Install_pico-8**
3. The script downloads and installs everything automatically

### Option B — Manual

This repo mirrors the MiSTer SD card layout. Copy the two folders directly:

```
PICO-8/   →   /media/fat/PICO-8/
Scripts/  →   /media/fat/Scripts/
```

Then create a `Carts/` folder inside `/media/fat/PICO-8/` for your games.

### Adding Games

Place your `.p8` and `.p8.png` cart files in `/media/fat/PICO-8/Carts/`. You can organize them into genre subfolders:

```
/media/fat/PICO-8/Carts/
├── [Action-Adventure]/
├── [Platform]/
├── [Puzzle]/
├── [Hacks]/
│   └── Celeste Hacks/
└── ...
```

The cart browser scans folders live — add or remove carts anytime.

### Running

**F12 → Scripts → pico-8**

## Cart Browser

The cart browser appears on launch. Navigate your folders and carts to pick a game.

| Controller | Action |
|-----------|--------|
| D-pad Up/Down | Scroll through items |
| D-pad Right | Enter folder |
| D-pad Left | Go back |
| A | Enter folder / Launch cart |
| X | Go back |
| Back / Guide | Quit |

## In-Game Controls

| Controller | PICO-8 |
|-----------|--------|
| D-pad / Analog stick | Directions |
| A | O button (confirm/jump) |
| X | X button (shoot/action) |
| Start | Pause menu |
| Back / Guide | Quit |

## Save Data

Games that support saving do so automatically. Save files are stored in `/media/fat/PICO-8/Saves/` and load automatically the next time you play.

## MiSTer SD Card Layout

```
/media/fat/
├── PICO-8/
│   ├── PICO-8              ← emulator binary
│   ├── pico8/bios.p8       ← system BIOS
│   ├── Carts/              ← your games
│   ├── Saves/              ← save data (auto-created)
│   └── config.txt          ← settings (auto-created)
└── Scripts/
    ├── pico-8.sh           ← launcher
    └── Install_pico-8.sh   ← installer
```

## Credits

- **[zepto8](https://github.com/samhocevar/zepto8)** by Sam Hocevar — emulator core
- **[z8lua](https://github.com/samhocevar/z8lua)** — Modified Lua 5.3 with PICO-8 extensions
- **[PICO-8](https://www.lexaloffle.com/pico-8.php)** by Lexaloffle — the fantasy console

## License

[GNU General Public License v3.0](LICENSE)

## Contributing

Developers: see [Building from source](src/BUILDING.md) for build instructions and project structure.

## Support

Thank you to all my [Patreon](https://www.patreon.com/MiSTer_Organize) supporters for making projects like this possible. If you enjoy MiSTer_PICO-8 and want to support future MiSTer projects, consider joining:

[![Patreon](https://img.shields.io/badge/Patreon-Support-orange?style=flat&logo=patreon)](https://www.patreon.com/MiSTer_Organize)
