#!/bin/bash
# Install_pico-8.sh — PICO-8 emulator installer for MiSTer FPGA
#
# Place this file in: /media/fat/Scripts/Install_pico-8.sh
# Run from MiSTer OSD: F12 -> Scripts -> Install_pico-8

REPO_URL="https://github.com/MiSTerOrganize/MiSTer_PICO-8"
WORKING=/tmp/MiSTer_PICO-8

echo "=============================="
echo " PICO-8 for MiSTer FPGA"
echo "=============================="
echo ""

# Clean up any previous download
rm -rf $WORKING > /dev/null 2>&1
rm -f /tmp/MiSTer_PICO-8.zip > /dev/null 2>&1

# Download repo zip
echo "Downloading..."
wget --no-check-certificate -q -O /tmp/MiSTer_PICO-8.zip "${REPO_URL}/archive/refs/heads/main.zip"

if [ ! -f /tmp/MiSTer_PICO-8.zip ] || [ ! -s /tmp/MiSTer_PICO-8.zip ]; then
    echo "Trying master branch..."
    wget --no-check-certificate -q -O /tmp/MiSTer_PICO-8.zip "${REPO_URL}/archive/refs/heads/master.zip"
fi

if [ ! -f /tmp/MiSTer_PICO-8.zip ] || [ ! -s /tmp/MiSTer_PICO-8.zip ]; then
    echo "ERROR: Download failed."
    exit 1
fi

echo "Download OK ($(du -h /tmp/MiSTer_PICO-8.zip | cut -f1))"

# Extract
echo "Extracting..."
cd /tmp
unzip -o MiSTer_PICO-8.zip > /dev/null 2>&1

# Find extracted folder
for dir in MiSTer_PICO-8-main MiSTer_PICO-8-master; do
    if [ -d "/tmp/$dir" ]; then
        mv "/tmp/$dir" $WORKING
        break
    fi
done

if [ ! -d "$WORKING" ]; then
    echo "ERROR: Extraction failed."
    exit 1
fi

# Install PICO-8 folder and its contents to root of SD card
echo "Installing..."
cp -a $WORKING/PICO-8 /media/fat/
chmod +x /media/fat/PICO-8/PICO-8

# Create empty Carts and Saves folders inside PICO-8
mkdir -p /media/fat/PICO-8/Carts
mkdir -p /media/fat/PICO-8/Saves

# Install README into PICO-8 folder
cp $WORKING/README.md /media/fat/PICO-8/

# Install pico-8.sh into Scripts folder at root of SD card
cp $WORKING/Scripts/pico-8.sh /media/fat/Scripts/
chmod +x /media/fat/Scripts/pico-8.sh

# Clean up
rm -rf $WORKING
rm -f /tmp/MiSTer_PICO-8.zip

echo ""
echo "=============================="
echo " Installation complete!"
echo "=============================="
echo ""
echo "  Put your .p8 and .p8.png carts in:"
echo "    /media/fat/PICO-8/Carts/"
echo ""
echo "  To play: F12 -> Scripts -> pico-8"
echo ""
