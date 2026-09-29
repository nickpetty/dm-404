#!/bin/sh
# gdec.sh ADDR... : decompile the firmware functions containing ADDRs.
root=$(cd "$(dirname "$0")/.." && pwd)
tools=${TOOLS:-/c/Users/nick/tools}
export JAVA_HOME=$(ls -d "$tools"/jdk-21*)
ghidra=$(ls -d "$tools"/ghidra_*_PUBLIC)
"$ghidra/support/analyzeHeadless.bat" "$(cygpath -w "$root/build/ghidra")" sp404 \
    -process r_80000000_copy.bin -noanalysis \
    -scriptPath "$(cygpath -w "$root/tools/ghidra")" -postScript Decomp.java "$@" 2>&1 |
    sed -n '/^==== /,/^==== end/p'
