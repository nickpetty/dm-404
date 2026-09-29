#!/bin/sh
# Builds the JUCE frontend: build/frontend/Doom404_artefacts/Release/Doom-404.exe
# Uses JUCE_DIR if set (an existing JUCE 9 checkout), else CMake fetches it.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
if [ ! -f "$root/build/frontend/CMakeCache.txt" ]; then
    cmake -S "$root/frontend" -B "$root/build/frontend" -G "Visual Studio 18 2026" -A x64 \
        ${JUCE_DIR:+-DJUCE_DIR="$JUCE_DIR"}
fi
cmake --build "$root/build/frontend" --config Release --parallel
