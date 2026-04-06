#!/bin/bash
# Install_PICO-8.sh — Downloads and installs PICO-8 for MiSTer
#
# Run from MiSTer Scripts menu. Downloads all files from GitHub
# and sets up auto-launch. After install, just load the PICO-8
# core from the console menu.
#

REPO="MiSTerOrganize/MiSTer_PICO-8_NativeVideo"
BRANCH="main"
BASE_URL="https://raw.githubusercontent.com/$REPO/$BRANCH"

echo "=== PICO-8 Installer for MiSTer ==="
echo ""

# Download files from GitHub repo
echo "Downloading PICO-8..."

mkdir -p /media/fat/_Console
mkdir -p /media/fat/PICO-8
mkdir -p /media/fat/games/PICO-8/Carts
mkdir -p /media/fat/games/PICO-8/Saves
mkdir -p /media/fat/config/inputs
mkdir -p /media/fat/docs/PICO-8

FAIL=0

echo "  Downloading FPGA core..."
wget -q --show-progress -O /media/fat/_Console/PICO-8.rbf "$BASE_URL/_Console/PICO-8.rbf" || FAIL=1

echo "  Downloading ARM binary..."
wget -q --show-progress -O /media/fat/PICO-8/PICO-8 "$BASE_URL/PICO-8/PICO-8" || FAIL=1

echo "  Downloading BIOS..."
wget -q --show-progress -O /media/fat/games/PICO-8/boot.rom "$BASE_URL/games/PICO-8/boot.rom" || FAIL=1

echo "  Downloading controller map..."
wget -q --show-progress -O /media/fat/config/inputs/PICO-8_input_045e_0b12_v3.map "$BASE_URL/config/inputs/PICO-8_input_045e_0b12_v3.map" || FAIL=1

echo "  Downloading README..."
wget -q --show-progress -O /media/fat/docs/PICO-8/README.md "$BASE_URL/docs/PICO-8/README.md" || FAIL=1

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "Error: One or more downloads failed. Check your internet connection."
    exit 1
fi

# Make binary executable
chmod +x /media/fat/PICO-8/PICO-8

# Install auto-launcher into user-startup.sh
STARTUP=/media/fat/linux/user-startup.sh
DAEMON_TAG="pico8_autolaunch"

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
    echo "Auto-launcher installed."
else
    echo "Auto-launcher already installed."
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
