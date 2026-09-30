// fatimg: the frontend's disk image code (frontend/Source/FatImage.cpp) on
// the command line, for making and inspecting SD card and eMMC images.
//
//   fatimg create IMG SIZE_MB fat32|exfat part|bare [LABEL [DIR...]]
//   fatimg info IMG
//   fatimg ls IMG [DIR]
//   fatimg add IMG HOSTPATH [DIR]
//   fatimg get IMG PATH HOSTDIR
//   fatimg rm IMG PATH
//
// Build: sh tools/build_fatimg.sh (build/fatimg.exe).
#include "../frontend/Source/FatImage.h"

#include <cstdio>
#include <string>

static int fail (const std::string& e)
{
    std::fprintf (stderr, "fatimg: %s\n", e.c_str());
    return 1;
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: fatimg create|info|ls|add|get|rm IMG ...\n");
        return 2;
    }
    const std::string cmd = argv[1];
    const std::filesystem::path img = argv[2];

    if (cmd == "create")
    {
        if (argc < 6)
            return fail ("create IMG SIZE_MB fat32|exfat part|bare [LABEL [DIR...]]");
        std::vector<std::string> dirs;
        for (int i = 7; i < argc; ++i)
            dirs.push_back (argv[i]);
        const auto e = fatimg::create (img, std::stoull (argv[3]) << 20,
                                       std::string (argv[4]) == "exfat" ? fatimg::Format::exfat : fatimg::Format::fat32,
                                       argc > 6 ? argv[6] : "", std::string (argv[5]) == "part", dirs);
        return e.empty() ? 0 : fail (e);
    }

    fatimg::Volume v;
    if (auto e = v.open (img, cmd == "info" || cmd == "ls" || cmd == "get"); ! e.empty())
        return fail (e);
    std::string e;
    if (cmd == "info")
        std::printf ("%s  label \"%s\"  %llu MB free of %llu MB\n", v.format() == fatimg::Format::exfat ? "exFAT" : "FAT32",
                     v.label().c_str(), (unsigned long long) (v.freeBytes() >> 20), (unsigned long long) (v.totalBytes() >> 20));
    else if (cmd == "ls")
    {
        std::vector<fatimg::Entry> list;
        e = v.list (argc > 3 ? argv[3] : "/", list);
        for (auto& x : list)
            std::printf ("%04d-%02d-%02d %02d:%02d %12s  %s\n", x.year, x.month, x.day, x.hour, x.minute,
                         x.dir ? "<DIR>" : std::to_string (x.size).c_str(), x.name.c_str());
    }
    else if (cmd == "add" && argc > 3)
        e = v.add (argv[3], argc > 4 ? argv[4] : "/", [] (const std::string& p) { std::printf ("  %s\n", p.c_str()); return true; });
    else if (cmd == "get" && argc > 4)
        e = v.extract (argv[3], argv[4]);
    else if (cmd == "rm" && argc > 3)
        e = v.remove (argv[3]);
    else
        e = "unknown command or missing arguments";
    return e.empty() ? 0 : fail (e);
}
