#pragma once

// Disk images of the unit's drives (the SD card, the internal eMMC), read
// and written with FatFs: make a blank one, list, add, extract and delete
// files. Plain C++17, no JUCE, so tools can use it too.
//
// Images are raw: a FAT32 or exFAT volume, with or without an MBR (real SD
// cards have one; mkdisk.py's images start at sector 0). Only one image is
// open at a time (FatFs is set up for a single drive); Volume enforces it.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fatimg
{
    enum class Format { fat32, exfat };

    struct Entry
    {
        std::string name;           // UTF-8
        bool dir = false;
        bool hidden = false;        // hidden or system (e.g. "System Volume Information")
        uint64_t size = 0;
        int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    };

    // Make a blank image of `bytes` (sparse where the file system allows):
    // an MBR with one partition (partitioned) or a bare volume, formatted,
    // labelled, with `dirs` made. Returns an error, empty on success.
    std::string create (const std::filesystem::path& file, uint64_t bytes, Format, const std::string& label,
                        bool partitioned, const std::vector<std::string>& dirs = {});

    class Volume
    {
    public:
        Volume() = default;
        ~Volume();
        Volume (const Volume&) = delete;
        Volume& operator= (const Volume&) = delete;

        // Errors come back as text, empty on success.
        std::string open (const std::filesystem::path& file, bool readOnly);
        void close();
        bool isOpen() const { return opened; }

        std::string list (const std::string& dir, std::vector<Entry>& out);
        std::string makeDir (const std::string& path);
        // Copies a host file or folder (recursively) into `dir`. `progress`
        // gets each file's image path; returning false stops the copy.
        std::string add (const std::filesystem::path& host, const std::string& dir,
                         const std::function<bool (const std::string&)>& progress = {});
        // Copies an image file or folder (recursively) out to `hostDir`.
        std::string extract (const std::string& path, const std::filesystem::path& hostDir,
                             const std::function<bool (const std::string&)>& progress = {});
        // Deletes a file, or a folder and everything in it.
        std::string remove (const std::string& path);

        uint64_t totalBytes() const;
        uint64_t freeBytes() const;
        std::string label() const;
        Format format() const;

    private:
        bool opened = false;
        void* fs = nullptr;         // FATFS
    };

    // Joins an image path ("/IMPORT" + "kick.wav").
    std::string join (const std::string& dir, const std::string& name);
}
