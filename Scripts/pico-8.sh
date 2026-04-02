#!/bin/bash
# pico-8.sh — Zepto8 (PICO-8 emulator) launcher for MiSTer FPGA
# Place this file in: /media/fat/Scripts/pico-8.sh

PICO8_DIR=/media/fat/PICO-8
BINARY="$PICO8_DIR/PICO-8"

export SDL_VIDEODRIVER=fbcon
export SDL_FBDEV=/dev/fb0

# Set framebuffer to 320x240 rgb16
vmode -r 320 240 rgb16 > /dev/null 2>&1

# Ensure directories exist
mkdir -p "$PICO8_DIR/Carts" "$PICO8_DIR/Saves"

# Hide framebuffer cursor
echo -e '\033[?17;0;0c' > /dev/tty1 2>/dev/null || true

# cd to binary directory for bios.p8 resolution
cd "$PICO8_DIR"

# Launch — built-in cart browser shows if no cart specified
taskset 03 "$BINARY" -data "$PICO8_DIR/" "$@" > /dev/null 2>&1

# Restore cursor on exit
echo -e '\033[?0c' > /dev/tty1 2>/dev/null || true
