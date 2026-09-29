#!/bin/sh
# screen.sh [SECONDS] [NAME]: boot for SECONDS, save the OLED as
# build/logs/NAME.png (default: screen).
root=$(cd "$(dirname "$0")/.." && pwd)
name=${2:-screen}
python "$root/tools/mon.py" -t "${1:-10}" "screendump build/logs/$name.ppm" > /dev/null 2>&1
python "$root/tools/ppm2png.py" "$root/build/logs/$name.ppm" "$root/build/logs/$name.png"
