#!/usr/bin/env bash
# Install Tilt-a-tron on a Waveshare ESP32-S3-Touch-AMOLED-1.75C over USB (macOS / Linux).
# No ESP-IDF needed: flashes the prebuilt firmware in firmware/ with esptool.
#
#   ./install.sh                install (keeps settings, Wi-Fi and the theme drive)
#   ./install.sh --erase        wipe the whole flash first
#   ./install.sh --port /dev/ttyACM0
#
# Needs python3; esptool is installed automatically the first time.
set -e
cd "$(dirname "$0")"
PORT=""
ERASE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --erase) ERASE=1 ;;
        --port) PORT="$2"; shift ;;
        *) echo "unknown option $1"; exit 1 ;;
    esac
    shift
done

command -v python3 >/dev/null || { echo "python3 is needed (https://python.org)"; exit 1; }
python3 -m esptool version >/dev/null 2>&1 || {
    echo "Installing esptool (one time) ..."
    python3 -m pip install --user --quiet esptool || python3 -m pip install --user --quiet --break-system-packages esptool
}

read_manifest() { python3 -c "import json,sys; m=json.load(open('firmware/manifest.json')); print($1)"; }
VERSION=$(read_manifest "m['version']")
SHA=$(read_manifest "m['sha']")
echo "Tilt-a-tron installer - firmware $VERSION (build $SHA)"

find_port() {
    ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -n 1
}
if [ -z "$PORT" ]; then PORT=$(find_port); fi
if [ -z "$PORT" ]; then
    echo
    echo "Put the watch in install mode:"
    echo "  1. Unplug it."
    echo "  2. Hold the small BOOT button next to the USB port."
    echo "  3. Plug the USB cable into this computer, keep holding for 2 seconds, then let go."
    echo "Waiting for it ..."
    for _ in $(seq 1 240); do
        PORT=$(find_port)
        [ -n "$PORT" ] && break
        sleep 0.5
    done
    [ -n "$PORT" ] || { echo "No watch found. Check the cable (it must carry data) and try again."; exit 1; }
fi
echo "Found the watch on $PORT"

CHIP=$(read_manifest "m['chip']")
MODE=$(read_manifest "m['flash']['mode']")
FREQ=$(read_manifest "m['flash']['freq']")
SIZE=$(read_manifest "m['flash']['size']")
FILES=$(read_manifest "' '.join(f['offset']+' firmware/'+f['file'] for f in m['files'])")

# esptool 5 spells its commands and options with dashes; 4 used underscores.
MAJOR=$(python3 -m esptool version 2>/dev/null | head -n 1 | sed 's/[^0-9.]//g' | cut -d. -f1)
opt() { if [ "${MAJOR:-4}" -ge 5 ]; then echo "$1" | tr '_' '-'; else echo "$1"; fi; }

COMMON="--chip $CHIP --port $PORT --baud 921600 --before $(opt default_reset) --after $(opt hard_reset)"
if [ "$ERASE" = 1 ]; then
    echo "Erasing flash ..."
    python3 -m esptool $COMMON "$(opt erase_flash)"
fi
echo "Writing firmware ..."
# shellcheck disable=SC2086
python3 -m esptool $COMMON "$(opt write_flash)" "$(opt --flash_mode)" "$MODE" "$(opt --flash_freq)" "$FREQ" "$(opt --flash_size)" "$SIZE" $FILES
echo
echo "Done! The watch is restarting into Tilt-a-tron."
echo "Tips: swipe the home screen to pick a game, tap to play, BOOT goes home, double-click PWR to sleep."
