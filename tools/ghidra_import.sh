#!/bin/sh
# Builds the Ghidra project build/ghidra/sp404.gpr from the unpacked regions:
# SDRAM code (0x80000000) and ITCM code (0x400) as Cortex-M Thumb, with the
# decompressed data regions added as memory blocks by ImportRegions.java.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
tools=${TOOLS:-/c/Users/nick/tools}
export JAVA_HOME=$(ls -d "$tools"/jdk-21*)
ghidra=$(ls -d "$tools"/ghidra_*_PUBLIC)
proj=$root/build/ghidra
python "$root/tools/unpack.py" > /dev/null
mkdir -p "$proj"
"$ghidra/support/analyzeHeadless.bat" "$(cygpath -w "$proj")" sp404 \
    -import "$(cygpath -w "$root/build/regions/r_80000000_copy.bin")" \
    -processor ARM:LE:32:Cortex -loader BinaryLoader -loader-baseAddr 0x80000000 \
    -scriptPath "$(cygpath -w "$root/tools/ghidra")" \
    -preScript ImportRegions.java "$(cygpath -w "$root/build/regions")" \
    -max-cpu 8
