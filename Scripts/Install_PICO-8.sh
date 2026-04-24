#!/bin/bash
# Install_PICO-8.sh — Downloads and installs PICO-8 for MiSTer
#
# Run from MiSTer Scripts menu. Downloads all files from GitHub
# and sets up auto-launch. After install, just load the PICO-8
# core from the console menu.
#

REPO="MiSTerOrganize/MiSTer_PICO-8"
BRANCH="main"
BASE_URL="https://raw.githubusercontent.com/$REPO/$BRANCH"

echo "=== PICO-8 Installer for MiSTer ==="
echo ""

# ── Kill ALL existing PICO-8 processes and daemons ─────────────────
# This is critical — old daemons spawn duplicate processes
killall PICO-8 2>/dev/null
killall pico8_daemon.sh 2>/dev/null
kill $(cat /tmp/pico8_arm.pid 2>/dev/null) 2>/dev/null
rm -f /tmp/pico8_arm.pid /tmp/pico8_next_cart.txt
rm -rf /tmp/pico8_daemon.lock
sleep 1

# ── Download files from GitHub repo ───────────────────────────────
echo "Downloading PICO-8..."

mkdir -p /media/fat/_Other
mkdir -p /media/fat/games/PICO-8/Carts
mkdir -p /media/fat/logs/PICO-8
mkdir -p /media/fat/saves/PICO-8
mkdir -p /media/fat/config
mkdir -p /media/fat/docs/PICO-8

# Remove old log folders from games directory
rm -rf /media/fat/games/PICO-8/.Logs /media/fat/games/PICO-8/Logs

# Migrate old config to new location (zepto8.cfg, not PICO-8.cfg — that name is used by MiSTer OSD)
if [ -f /media/fat/games/PICO-8/config.txt ] && [ ! -f /media/fat/config/zepto8.cfg ]; then
    mv /media/fat/games/PICO-8/config.txt /media/fat/config/zepto8.cfg
    echo "Migrated config to /media/fat/config/zepto8.cfg"
fi

FAIL=0

# Read current RBF name from version.txt manifest
RBF_NAME=$(wget -q -O - "$BASE_URL/version.txt" | tr -d '\r\n')
if [ -z "$RBF_NAME" ]; then
    echo "Error: Could not fetch version.txt"
    exit 1
fi

echo "  Downloading FPGA core ($RBF_NAME)..."
# Remove old RBFs from both _Other and legacy _Console location
rm -f /media/fat/_Other/PICO-8_*.rbf /media/fat/_Other/PICO-8.rbf
rm -f /media/fat/_Console/PICO-8_*.rbf /media/fat/_Console/PICO-8.rbf
wget -q --show-progress -O "/media/fat/_Other/$RBF_NAME" "$BASE_URL/_Other/$RBF_NAME" || FAIL=1

echo "  Downloading ARM binary..."
wget -q --show-progress -O /media/fat/games/PICO-8/PICO-8 "$BASE_URL/games/PICO-8/PICO-8" || FAIL=1

echo "  Downloading BIOS..."
wget -q --show-progress -O /media/fat/games/PICO-8/boot.rom "$BASE_URL/games/PICO-8/boot.rom" || FAIL=1

echo "  Downloading daemon..."
wget -q --show-progress -O /media/fat/games/PICO-8/pico8_daemon.sh "$BASE_URL/games/PICO-8/pico8_daemon.sh" || FAIL=1

echo "  Downloading README..."
wget -q --show-progress -O /media/fat/docs/PICO-8/README.md "$BASE_URL/docs/PICO-8/README.md" || FAIL=1

if [ "$FAIL" -ne 0 ]; then
    echo ""
    echo "Error: One or more downloads failed. Check your internet connection."
    exit 1
fi

# Make files executable
chmod +x /media/fat/games/PICO-8/PICO-8
chmod +x /media/fat/games/PICO-8/pico8_daemon.sh

# Remove old binary location if it exists
rm -rf /media/fat/PICO-8

# ── Migrate saves from old location ─────────────────────────────
OLD_SAVES="/media/fat/games/PICO-8/Saves"
NEW_SAVES="/media/fat/saves/PICO-8"
if [ -d "$OLD_SAVES" ]; then
    if [ "$(ls -A "$OLD_SAVES" 2>/dev/null)" ]; then
        echo "Migrating saves to $NEW_SAVES..."
        cp -n "$OLD_SAVES"/* "$NEW_SAVES"/ 2>/dev/null
    fi
    rm -rf "$OLD_SAVES"
    echo "Removed old Saves folder from games/PICO-8/."
fi

# ── Install daemon into user-startup.sh ───────────────────────────
STARTUP=/media/fat/linux/user-startup.sh

# Remove ALL old PICO-8 daemon entries (inline blocks and launcher lines)
if [ -f "$STARTUP" ]; then
    # Remove old inline daemon blocks
    sed -i '/pico8_autolaunch/,/^) \&$/d' "$STARTUP"
    # Remove old daemon launcher lines
    sed -i '/pico8_daemon\.sh/d' "$STARTUP"
    # Remove old comment lines
    sed -i '/PICO-8 auto-launch/d' "$STARTUP"
fi

# Add single launcher line
echo "" >> "$STARTUP"
echo "# PICO-8 auto-launch daemon" >> "$STARTUP"
echo "/media/fat/games/PICO-8/pico8_daemon.sh &" >> "$STARTUP"

echo "Auto-launcher installed."

# ── Start daemon now ──────────────────────────────────────────────
/media/fat/games/PICO-8/pico8_daemon.sh &

echo ""
echo "=== PICO-8 installed successfully! ==="
echo ""
echo "Load the PICO-8 core from the console menu to play."
echo "Use the MiSTer OSD to load carts."
echo "Place .p8 and .p8.png carts in: games/PICO-8/Carts/"
echo ""
