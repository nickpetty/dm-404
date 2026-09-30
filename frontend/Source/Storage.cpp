#include "Storage.h"
#include "FatImage.h"

namespace Storage
{
    juce::File dataDir()
    {
#if JUCE_WINDOWS
        // Local, not roaming: the drive images are large.
        auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
#else
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
#endif
        auto dir = base.getChildFile ("DM-404");
        // The app was called Doom-404 until 0.1.1: its folder moves over
        // once (a rename on the same drive, so the sparse images stay sparse).
        // If it cannot move (the old app still has it open), it is used
        // where it is, and the move is tried again next time.
        if (! dir.exists())
            if (auto old = base.getChildFile ("Doom-404"); old.isDirectory() && ! old.moveFileTo (dir))
                return old;
        dir.createDirectory();
        return dir;
    }

    juce::File firmware() { return dataDir().getChildFile ("firmware/SP404MKII_APP1.bin"); }
    juce::File systemFlash() { return dataDir().getChildFile ("system.bin"); }
    juce::File internalDrive() { return dataDir().getChildFile ("internal.img"); }
    juce::File sdCard() { return dataDir().getChildFile ("sdcard.img"); }

    juce::File devRoot()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
        while (! dir.getChildFile ("firmware").isDirectory() || ! dir.getChildFile ("core").isDirectory())
        {
            auto parent = dir.getParentDirectory();
            if (parent == dir)
                return {};
            dir = parent;
        }
        return dir;
    }

    juce::File qemu()
    {
#if JUCE_WINDOWS
        const juce::String exe = "qemu-system-arm.exe";
#else
        const juce::String exe = "qemu-system-arm";
#endif
        auto here = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
        for (auto f : { here.getChildFile ("qemu").getChildFile (exe), here.getChildFile (exe) })
            if (f.existsAsFile())
                return f;
        if (auto root = devRoot(); root != juce::File())
            return root.getChildFile ("build/qemu").getChildFile (exe);
        return here.getChildFile ("qemu").getChildFile (exe);
    }

    juce::String sha256 (const juce::File& f)
    {
        juce::FileInputStream in (f);
        return in.openedOk() ? juce::SHA256 (in).toHexString() : juce::String();
    }

    juce::String importFirmware (const juce::File& source, bool& wrongVersion)
    {
        wrongVersion = false;
        juce::MemoryBlock data;
        if (source.hasFileExtension ("zip"))
        {
            // Roland's download: the program somewhere inside the zip.
            juce::ZipFile zip (source);
            for (int i = 0; i < zip.getNumEntries(); ++i)
                if (auto* e = zip.getEntry (i); e != nullptr
                    && e->filename.fromLastOccurrenceOf ("/", false, false).equalsIgnoreCase ("SP404MKII_APP1.bin"))
                {
                    std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
                    if (in != nullptr)
                        in->readIntoMemoryBlock (data);
                    break;
                }
            if (data.isEmpty())
                return "That zip has no SP404MKII_APP1.bin in it.";
        }
        else if (! source.loadFileAsData (data) || data.isEmpty())
            return "Could not read " + source.getFullPathName();

        wrongVersion = juce::SHA256 (data).toHexString() != firmwareSha256;
        auto dest = firmware();
        dest.getParentDirectory().createDirectory();
        if (! dest.replaceWithData (data.getData(), data.getSize()))
            return "Could not write " + dest.getFullPathName();
        return {};
    }

    juce::StringArray adoptDevFiles()
    {
        juce::StringArray copied;
        auto root = devRoot();
        if (root == juce::File())
            return copied;
        auto adopt = [&] (const juce::File& from, const juce::File& to, const char* what)
        {
            if (from.existsAsFile() && ! to.existsAsFile())
            {
                to.getParentDirectory().createDirectory();
                if (from.copyFileTo (to))
                    copied.add (what);
            }
        };
        adopt (root.getChildFile ("firmware/SP404MKII_APP1.bin"), firmware(), "the firmware");
        adopt (root.getChildFile ("build/flash.bin"), systemFlash(), "the unit's settings (build/flash.bin)");
        adopt (root.getChildFile ("build/emmc.img"), internalDrive(), "the internal drive (build/emmc.img)");
        return copied;
    }

    juce::String formatSdCard (const juce::File& image, juce::int64 bytes)
    {
        const auto fmt = bytes > ((juce::int64) 32 << 30) ? fatimg::Format::exfat : fatimg::Format::fat32;
        const auto e = fatimg::create (toPath (image), (uint64_t) bytes, fmt,
                                       "SP-404MKII", true, { "IMPORT", "EXPORT" });
        return juce::String (e);
    }

    juce::String ensureDrives()
    {
        if (! internalDrive().existsAsFile())
            if (auto e = fatimg::create (toPath (internalDrive()), (uint64_t) internalBytes,
                                         fatimg::Format::exfat, "SP-404MKII", false);
                ! e.empty())
                return "Could not make the internal drive: " + juce::String (e);
        if (! sdCard().existsAsFile())
            if (auto e = formatSdCard (sdCard(), defaultSdBytes); e.isNotEmpty())
                return "Could not make the SD card: " + e;
        return {};
    }
}
