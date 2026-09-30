#!/bin/sh
# Builds build/fatimg.exe (tools/fatimg.cpp): the frontend's disk image code
# on the command line. MSYS2 UCRT64 g++, static.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/build"
gcc -O2 -c "$root/frontend/ThirdParty/fatfs/ff.c" -o "$root/build/ff.o"
gcc -O2 -c "$root/frontend/ThirdParty/fatfs/ffunicode.c" -o "$root/build/ffunicode.o"
gcc -O2 -c "$root/frontend/ThirdParty/fatfs/ffsystem.c" -o "$root/build/ffsystem.o"
g++ -std=c++20 -O2 -static "$root/tools/fatimg.cpp" "$root/frontend/Source/FatImage.cpp" \
    "$root/build/ff.o" "$root/build/ffunicode.o" "$root/build/ffsystem.o" -o "$root/build/fatimg.exe"
echo "built $root/build/fatimg.exe"
