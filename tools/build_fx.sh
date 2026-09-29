#!/bin/sh
# Builds the effects engine (core/fx) with DaisySP: build/fx/sp404fx.dll,
# copied next to build/qemu/qemu-system-arm.exe, where the emulator loads it.
# Fetches DaisySP (and its LGPL half) into third_party/ the first time.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
daisy=$root/third_party/DaisySP
if [ ! -d "$daisy/Source" ]; then
    git clone --depth 1 https://github.com/electro-smith/DaisySP.git "$daisy"
fi
if [ ! -f "$daisy/DaisySP-LGPL/Source/daisysp-lgpl.h" ]; then
    git -C "$daisy" submodule update --init --depth 1
fi
export MSYSTEM=UCRT64
/c/msys64/usr/bin/bash -lc "
set -e
cd '$root'
mkdir -p build/fx/obj build/qemu
objs=''
# DaisySP's sources include each other by bare name: every directory.
incs=\$(find third_party/DaisySP/Source third_party/DaisySP/DaisySP-LGPL/Source -type d | sed 's/^/-I/')
for src in \$(ls core/fx/*.cpp) \$(find third_party/DaisySP/Source third_party/DaisySP/DaisySP-LGPL/Source -name '*.cpp'); do
    obj=build/fx/obj/\$(echo \$src | tr '/' '_' | sed 's/\\.cpp\$/.o/')
    if [ ! -f \$obj ] || [ \$src -nt \$obj ] || [ -n \"\$(find core/fx -name '*.h' -newer \$obj)\" ]; then
        g++ -std=c++17 -O2 -ffast-math -DSP404FX_BUILD -D_USE_MATH_DEFINES \
            -Icore/fx \$incs \
            -c \$src -o \$obj
    fi
    objs=\"\$objs \$obj\"
done
# Fully static (winpthread too): the DLL needs nothing but Windows.
g++ -shared -static -o build/fx/sp404fx.dll \$objs
cp build/fx/sp404fx.dll build/qemu/
"
echo "built build/fx/sp404fx.dll"
