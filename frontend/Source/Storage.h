#pragma once

#include <JuceHeader.h>
#include <filesystem>

// Where the emulated unit keeps its things, and getting them in place on
// the first run. Nothing of Roland's ships with the app: the user points it
// at the System Program they downloaded, and gets blank drives.
//
// The data folder (%LOCALAPPDATA%\Doom-404 on Windows) holds:
//   firmware/SP404MKII_APP1.bin   the System Program, copied in at setup
//   system.bin                    the unit's flash (settings; the firmware writes it)
//   internal.img                  the internal drive (B:), exFAT like the unit's
//   sdcard.img                    the SD card (A:), FAT32 with a partition table
// The drives are sparse files: they only take the space that is used.
namespace Storage
{
    juce::File dataDir();
    juce::File firmware();
    juce::File systemFlash();
    juce::File internalDrive();
    juce::File sdCard();

    // The emulator core: next to the app (qemu/ in a release), else the
    // development build (build/qemu under the repo).
    juce::File qemu();

    // The repo checkout the app runs from, if any (it holds firmware/).
    juce::File devRoot();

    // The System Program 5.52 checksum; other versions are untested.
    constexpr const char* firmwareSha256 = "4a3d67711e14dcc97d50249a4eee7dd6df0251a2f37757cbbe2556c233730d80";
    juce::String sha256 (const juce::File&);

    // Copies SP404MKII_APP1.bin in, from the .bin itself or Roland's zip.
    // Returns an error, empty on success; `wrongVersion` says whether the
    // file was not 5.52 (copied anyway).
    juce::String importFirmware (const juce::File& binOrZip, bool& wrongVersion);

    // A development checkout's firmware, flash and eMMC image (build/) are
    // brought over the first time. Returns what was copied, for a message.
    juce::StringArray adoptDevFiles();

    // Makes whichever drive images are missing. Returns an error or empty.
    juce::String ensureDrives();

    // Card sizes: FAT32 up to 32 GB (SDHC), exFAT above (SDXC).
    juce::String formatSdCard (const juce::File& image, juce::int64 bytes);
    constexpr juce::int64 defaultSdBytes = (juce::int64) 16 << 30;
    constexpr juce::int64 internalBytes = (juce::int64) 16 << 30;

    // A file as a std::filesystem path (wide on Windows: any user name).
    inline std::filesystem::path toPath (const juce::File& f)
    {
#if JUCE_WINDOWS
        return std::filesystem::path (f.getFullPathName().toWideCharPointer());
#else
        return std::filesystem::path (f.getFullPathName().toStdString());
#endif
    }
}
