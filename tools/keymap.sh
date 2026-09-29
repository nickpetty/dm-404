#!/bin/sh
# keymap.sh: boot fresh for each matrix key the firmware maps (columns 0-3,
# all rows; column 4 row 0), press it, and save screenshots
# build/logs/km_R_C_{down,up}.png, for naming the panel buttons.
root=$(cd "$(dirname "$0")/.." && pwd)
# Run from a copy so the core can be rebuilt meanwhile (Windows locks .exe).
cp "$root/build/qemu/qemu-system-arm.exe" "$root/build/qemu/qemu-keymap.exe"
export LINK_QEMU_EXE="$root/build/qemu/qemu-keymap.exe"
for r in 0 1 2 3 4 5 6 7; do
  for c in 0 1 2 3 4; do
    [ "$c" = 4 ] && [ "$r" != 0 ] && continue
    python "$root/tools/link.py" -t 22 shot:km_base_${r}_${c} hold:$r,$c wait:1.0 shot:km_${r}_${c}_down up:$r,$c wait:1.0 shot:km_${r}_${c}_up > /dev/null 2>&1
  done
done
echo done
