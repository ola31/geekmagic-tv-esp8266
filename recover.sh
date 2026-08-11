#!/bin/bash
# Recovers a SmallTV Ultra that no longer boots, over the UART pads.
#
#   ./recover.sh stock    - back to GeekMagic V9.0.50
#   ./recover.sh custom   - the dashboard firmware built here
#
# Wiring (adapter <-> board), 3.3 V adapter only, leave its VCC unconnected:
#   GND  <-> GND
#   RX   <-> TXD0
#   TX   <-> RXD0
#   GPIO0 <-> GND, but only while power is applied, to enter flash mode.

set -euo pipefail
cd "$(dirname "$0")"

TARGET="${1:-}"
case "$TARGET" in
    stock)  IMAGE="stock-backup/FW-Smalltv-Ultra-V9.0.50.bin" ;;
    custom) IMAGE="firmware-custom.bin" ;;
    *)      echo "usage: $0 {stock|custom}"; exit 1 ;;
esac

[ -f "$IMAGE" ] || { echo "missing image: $IMAGE"; exit 1; }

ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
[ -f "$ESPTOOL" ] || { echo "esptool not found at $ESPTOOL"; exit 1; }

PORT="${PORT:-}"
if [ -z "$PORT" ]; then
    PORT=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1 || true)
fi
[ -n "$PORT" ] || { echo "no serial port found - is the adapter plugged in?"; exit 1; }

echo "port   : $PORT"
echo "image  : $IMAGE ($(stat -c%s "$IMAGE") bytes)"
echo

echo "== chip =="
python3 "$ESPTOOL" --port "$PORT" --baud 115200 chip_id

echo
echo "== erase =="
python3 "$ESPTOOL" --port "$PORT" --baud 115200 erase_flash

echo
echo "== write =="
# DIO at 40 MHz with a 4 MB map, matching the header of the stock image.
python3 "$ESPTOOL" --port "$PORT" --baud 115200 \
    --after hard_reset write_flash \
    --flash_mode dio --flash_freq 40m --flash_size 4MB \
    0x0 "$IMAGE"

echo
echo "Done. Remove the GPIO0-to-GND link and power cycle the device."
