#!/bin/sh
# Builds the JUCE frontend: build/frontend/Doom404_artefacts/Release/Doom-404.exe
# and the DAW plugin, build/frontend/Doom404Link_artefacts/Release/VST3/.
# Uses JUCE_DIR if set (an existing JUCE 9 checkout), else CMake fetches it.
# INSTALL_PLUGIN=1 also copies the VST3 to the per-user VST3 folder
# (%LOCALAPPDATA%\Programs\Common\VST3), where DAWs look for plugins.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
if [ ! -f "$root/build/frontend/CMakeCache.txt" ]; then
    cmake -S "$root/frontend" -B "$root/build/frontend" -G "Visual Studio 18 2026" -A x64 \
        ${JUCE_DIR:+-DJUCE_DIR="$JUCE_DIR"}
fi
cmake --build "$root/build/frontend" --config Release --parallel
if [ -n "$INSTALL_PLUGIN" ]; then
    dest=$(cygpath -u "$LOCALAPPDATA")/Programs/Common/VST3
    mkdir -p "$dest"
    rm -rf "$dest/Doom-404 Link.vst3"
    cp -r "$root/build/frontend/Doom404Link_artefacts/Release/VST3/Doom-404 Link.vst3" "$dest/"
    echo "installed $dest/Doom-404 Link.vst3"
fi
