#!/bin/bash
# pico-8-nv.sh — PICO-8 with FPGA native video (for CRT output)
#
# Place in: /media/fat/Scripts/pico-8-nv.sh
#
# REQUIRES: PICO8 core loaded first (select PICO8 from console menu)
# Then run this script via SSH. Use MiSTer OSD to load carts.
#
# Folder structure on SD card:
#   /media/fat/games/PICO8/boot.rom     (BIOS, renamed from bios.p8)
#   /media/fat/games/PICO8/Carts/       (cart files, .p8 and .p8.png)
#   /media/fat/games/PICO8/Saves/       (game saves)
#

PICO8_DIR=/media/fat/games/PICO8
BINARY=/media/fat/PICO-8/PICO-8

# Ensure directories exist
mkdir -p "$PICO8_DIR/Carts" "$PICO8_DIR/Saves"

echo "Starting PICO-8 with FPGA native video..."
echo "Open MiSTer OSD to load carts."
echo ""

taskset 03 "$BINARY" -nativevideo -data "$PICO8_DIR/" 2>&1

echo ""
echo "PICO-8 exited."
