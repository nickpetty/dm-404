#!/bin/sh
# gxref.sh ADDR... : list references to ADDRs and decompile the referencing functions.
root=$(cd "$(dirname "$0")/.." && pwd)
tools=${TOOLS:-/c/Users/nick/tools}
export JAVA_HOME=$(ls -d "$tools"/jdk-21*)
ghidra=$(ls -d "$tools"/ghidra_*_PUBLIC)
"$ghidra/support/analyzeHeadless.bat" "$(cygpath -w "$root/build/ghidra")" sp404 \
    -process r_80000000_copy.bin -noanalysis \
    -scriptPath "$(cygpath -w "$root/tools/ghidra")" -postScript Xrefs.java "$@" 2>&1 |
    sed -n '/^==== /,/^==== end/p'
