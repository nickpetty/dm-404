#!/bin/sh
# Builds the emulator core: syncs our QEMU sources, configures once, runs
# ninja. Output: build/qemu/qemu-system-arm.exe
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
sh "$root/tools/qemu_sync.sh"
export MSYSTEM=UCRT64
/c/msys64/usr/bin/bash -lc "
set -e
mkdir -p '$root/build/qemu' && cd '$root/build/qemu'
if [ ! -f build.ninja ]; then
    ../../third_party/qemu/configure --target-list=arm-softmmu \
        --disable-docs --disable-werror --disable-tools --disable-guest-agent \
        --disable-sdl --disable-gtk --disable-vnc --enable-debug-info
fi
ninja qemu-system-arm.exe
"
