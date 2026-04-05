#!/bin/bash
# Install_PICO-8.sh — Downloads and installs PICO-8 for MiSTer
#
# Run from MiSTer Scripts menu. Downloads the latest release,
# installs all files, and sets up auto-launch.
# After install, just load the PICO-8 core from the console menu.
#

REPO="MiSTerOrganize/MiSTer_PICO-8_NativeVideo"
RELEASE_URL="https://github.com/$REPO/releases/latest/download/MiSTer-PICO-8-release.zip"
TMP_ZIP="/tmp/pico8_install.zip"
TMP_DIR="/tmp/pico8_install"
STARTUP=/media/fat/linux/user-startup.sh
DAEMON_TAG="pico8_autolaunch"

echo "=== PICO-8 Installer for MiSTer ==="
echo ""

# Download latest release
echo "Downloading PICO-8..."
wget -q --show-progress -O "$TMP_ZIP" "$RELEASE_URL"
if [ $? -ne 0 ]; then
    echo "Error: Download failed. Check your internet connection."
    rm -f "$TMP_ZIP"
    exit 1
fi

# Extract to SD card root
echo "Installing files..."
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"
unzip -q -o "$TMP_ZIP" -d "$TMP_DIR"

# Copy files to correct locations
cp -r "$TMP_DIR"/_Console /media/fat/
cp -r "$TMP_DIR"/PICO-8 /media/fat/
cp -r "$TMP_DIR"/games /media/fat/
cp -r "$TMP_DIR"/config /media/fat/

# Make binary executable
chmod +x /media/fat/PICO-8/PICO-8

# Create Carts and Saves folders
mkdir -p /media/fat/games/PICO-8/Carts /media/fat/games/PICO-8/Saves

# Clean up
rm -f "$TMP_ZIP"
rm -rf "$TMP_DIR"

# Check required files
if [ ! -f /media/fat/PICO-8/PICO-8 ]; then
    echo "Error: Binary not found after install"
    exit 1
fi
if [ ! -f /media/fat/games/PICO-8/boot.rom ]; then
    echo "Error: boot.rom not found after install"
    exit 1
fi

# Install auto-launcher into user-startup.sh
if ! grep -q "$DAEMON_TAG" "$STARTUP" 2>/dev/null; then
    cat >> "$STARTUP" << 'DAEMON'

# pico8_autolaunch — auto-start PICO-8 emulator when core loads
(
LAST_CORE=""
while true; do
    CUR=$(cat /tmp/CORENAME 2>/dev/null)
    if [ "$CUR" = "PICO-8" ] && [ "$LAST_CORE" != "PICO-8" ]; then
        sleep 2
        taskset 03 /media/fat/PICO-8/PICO-8 -nativevideo -data /media/fat/games/PICO-8/ > /dev/null 2>&1 &
        echo $! > /tmp/pico8_arm.pid
    elif [ "$CUR" != "PICO-8" ] && [ "$LAST_CORE" = "PICO-8" ]; then
        kill $(cat /tmp/pico8_arm.pid 2>/dev/null) 2>/dev/null
        rm -f /tmp/pico8_arm.pid
    fi
    LAST_CORE="$CUR"
    sleep 1
done
) &
DAEMON
fi

# Start the daemon now so reboot isn't needed
pkill -f "PICO-8.*nativevideo" 2>/dev/null
(
LAST_CORE=""
while true; do
    CUR=$(cat /tmp/CORENAME 2>/dev/null)
    if [ "$CUR" = "PICO-8" ] && [ "$LAST_CORE" != "PICO-8" ]; then
        sleep 2
        taskset 03 /media/fat/PICO-8/PICO-8 -nativevideo -data /media/fat/games/PICO-8/ > /dev/null 2>&1 &
        echo $! > /tmp/pico8_arm.pid
    elif [ "$CUR" != "PICO-8" ] && [ "$LAST_CORE" = "PICO-8" ]; then
        kill $(cat /tmp/pico8_arm.pid 2>/dev/null) 2>/dev/null
        rm -f /tmp/pico8_arm.pid
    fi
    LAST_CORE="$CUR"
    sleep 1
done
) &

echo ""
echo "=== PICO-8 installed successfully! ==="
echo ""
echo "Load the PICO-8 core from the console menu to play."
echo "Use the MiSTer OSD to load carts."
echo "Place .p8 and .p8.png carts in: games/PICO-8/Carts/"
echo ""
