#include "FatImage.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>

#ifdef _WIN32
 #define WIN32_LEAN_AND_MEAN
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <winioctl.h>
#else
 #include <fcntl.h>
 #include <unistd.h>
#endif

extern "C"
{
#include "../ThirdParty/fatfs/ff.h"
#include "../ThirdParty/fatfs/diskio.h"
}

namespace fs = std::filesystem;

//==============================================================================
// The one drive FatFs sees: an image file, 512-byte sectors.
namespace
{
    struct Disk
    {
        std::FILE* f = nullptr;
        uint64_t sectors = 0;
        bool readOnly = false;
    } disk;

    std::string utf8 (const fs::path& p)
    {
        const auto s = p.u8string();
        return std::string (s.begin(), s.end());
    }

    fs::path fromUtf8 (const std::string& s)
    {
#if defined(__cpp_char8_t)
        return fs::path (std::u8string (s.begin(), s.end()));
#else
        return fs::u8path (s);
#endif
    }

    // FatFs keeps its state in globals: one image at a time, program-wide.
    std::recursive_mutex& lock()
    {
        static std::recursive_mutex m;
        return m;
    }

    bool seek (uint64_t at)
    {
#ifdef _WIN32
        return _fseeki64 (disk.f, (long long) at, SEEK_SET) == 0;
#else
        return fseeko (disk.f, (off_t) at, SEEK_SET) == 0;
#endif
    }

    std::FILE* openFile (const fs::path& p, bool write)
    {
#ifdef _WIN32
        return _wfopen (p.c_str(), write ? L"r+b" : L"rb");
#else
        return std::fopen (p.c_str(), write ? "r+b" : "rb");
#endif
    }

    bool attach (const fs::path& p, bool readOnly)
    {
        disk.f = openFile (p, ! readOnly);
        if (disk.f == nullptr)
            return false;
        std::error_code ec;
        disk.sectors = fs::file_size (p, ec) / 512;
        disk.readOnly = readOnly;
        return ! ec;
    }

    void detach()
    {
        if (disk.f != nullptr)
            std::fclose (disk.f);
        disk = {};
    }

    std::string describe (FRESULT r)
    {
        static const char* names[] = {
            "ok", "disk error", "internal error", "drive not ready", "no such file", "no such path",
            "invalid name", "access denied (or the folder is not empty)", "already exists", "invalid object",
            "write protected", "invalid drive", "not enabled", "no FAT or exFAT volume found",
            "format failed", "timeout", "locked", "out of memory", "too many open files", "invalid parameter" };
        return (size_t) r < std::size (names) ? names[r] : "error " + std::to_string ((int) r);
    }

    // Image paths are UTF-8 and FatFs takes them as TCHAR (char, UTF-8).
    std::string drivePath (const std::string& p)
    {
        return "0:" + (p.empty() || p[0] != '/' ? "/" + p : p);
    }
}

extern "C"
{
    DSTATUS disk_status (BYTE pdrv)
    {
        if (pdrv != 0 || disk.f == nullptr)
            return STA_NOINIT;
        return disk.readOnly ? STA_PROTECT : 0;
    }

    DSTATUS disk_initialize (BYTE pdrv)
    {
        return disk_status (pdrv);
    }

    DRESULT disk_read (BYTE pdrv, BYTE* buff, LBA_t sector, UINT count)
    {
        if (pdrv != 0 || disk.f == nullptr)
            return RES_NOTRDY;
        if (! seek ((uint64_t) sector * 512))
            return RES_ERROR;
        // Past the end of a short file reads as zeros (sparse tails).
        const size_t got = std::fread (buff, 1, (size_t) count * 512, disk.f);
        std::memset (buff + got, 0, (size_t) count * 512 - got);
        return RES_OK;
    }

    DRESULT disk_write (BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count)
    {
        if (pdrv != 0 || disk.f == nullptr)
            return RES_NOTRDY;
        if (disk.readOnly)
            return RES_WRPRT;
        if (! seek ((uint64_t) sector * 512) || std::fwrite (buff, 512, count, disk.f) != count)
            return RES_ERROR;
        return RES_OK;
    }

    DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void* buff)
    {
        if (pdrv != 0 || disk.f == nullptr)
            return RES_NOTRDY;
        switch (cmd)
        {
            case CTRL_SYNC:        return std::fflush (disk.f) == 0 ? RES_OK : RES_ERROR;
            case GET_SECTOR_COUNT: *(LBA_t*) buff = (LBA_t) disk.sectors; return RES_OK;
            case GET_SECTOR_SIZE:  *(WORD*) buff = 512; return RES_OK;
            case GET_BLOCK_SIZE:   *(DWORD*) buff = 8192; return RES_OK;     // 4 MB, as SD formatters align
            default:               return RES_PARERR;
        }
    }

    DWORD get_fattime (void)
    {
        const std::time_t t = std::time (nullptr);
        std::tm tm {};
#ifdef _WIN32
        localtime_s (&tm, &t);
#else
        localtime_r (&t, &tm);
#endif
        return (DWORD) (tm.tm_year - 80) << 25 | (DWORD) (tm.tm_mon + 1) << 21 | (DWORD) tm.tm_mday << 16
             | (DWORD) tm.tm_hour << 11 | (DWORD) tm.tm_min << 5 | (DWORD) (tm.tm_sec / 2);
    }
}

namespace fatimg
{
    std::string join (const std::string& dir, const std::string& name)
    {
        if (dir.empty() || dir == "/")
            return "/" + name;
        return dir.back() == '/' ? dir + name : dir + "/" + name;
    }

    //==========================================================================
    std::string create (const fs::path& file, uint64_t bytes, Format format, const std::string& label,
                        bool partitioned, const std::vector<std::string>& dirs)
    {
        std::lock_guard<std::recursive_mutex> sl (lock());
        if (disk.f != nullptr)
            return "another image is open";

        // An empty file of the full size, sparse so that only what gets
        // written takes space on the host.
#ifdef _WIN32
        HANDLE h = CreateFileW (file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return "cannot create " + utf8 (file);
        DWORD ret = 0;
        DeviceIoControl (h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
        LARGE_INTEGER size;
        size.QuadPart = (LONGLONG) bytes;
        const bool sized = SetFilePointerEx (h, size, nullptr, FILE_BEGIN) && SetEndOfFile (h);
        CloseHandle (h);
        if (! sized)
            return "cannot make " + utf8 (file) + " that large";
#else
        {
            std::ofstream (file, std::ios::binary | std::ios::trunc);
        }
        std::error_code ec;
        fs::resize_file (file, bytes, ec);
        if (ec)
            return "cannot make " + utf8 (file) + " that large: " + ec.message();
#endif
        if (! attach (file, false))
            return "cannot open " + utf8 (file);

        MKFS_PARM opt {};
        opt.fmt = (BYTE) ((format == Format::exfat ? FM_EXFAT : FM_FAT32) | (partitioned ? 0 : FM_SFD));
        std::vector<BYTE> work (1 << 20);
        FRESULT r = f_mkfs ("0:", &opt, work.data(), (UINT) work.size());
        std::string err;
        if (r != FR_OK)
            err = "format: " + describe (r);
        else
        {
            FATFS vol {};
            r = f_mount (&vol, "0:", 1);
            if (r == FR_OK && ! label.empty())
                r = f_setlabel (("0:" + label).c_str());
            for (auto& d : dirs)
                if (r == FR_OK)
                    r = f_mkdir (drivePath (d).c_str());
            if (r != FR_OK)
                err = describe (r);
            f_unmount ("0:");
        }
        detach();
        return err;
    }

    //==========================================================================
    Volume::~Volume()
    {
        close();
    }

    std::string Volume::open (const fs::path& file, bool readOnly)
    {
        close();
        std::lock_guard<std::recursive_mutex> sl (lock());
        if (disk.f != nullptr)
            return "another image is open";
        if (! attach (file, readOnly))
            return "cannot open " + utf8 (file);
        auto* vol = new FATFS {};
        const FRESULT r = f_mount (vol, "0:", 1);
        if (r != FR_OK)
        {
            delete vol;
            detach();
            return describe (r);
        }
        fs = vol;
        opened = true;
        return {};
    }

    void Volume::close()
    {
        if (! opened)
            return;
        std::lock_guard<std::recursive_mutex> sl (lock());
        f_unmount ("0:");
        delete (FATFS*) fs;
        fs = nullptr;
        detach();
        opened = false;
    }

    std::string Volume::list (const std::string& dir, std::vector<Entry>& out)
    {
        out.clear();
        DIR d;
        FRESULT r = f_opendir (&d, drivePath (dir).c_str());
        if (r != FR_OK)
            return describe (r);
        FILINFO fi;
        while ((r = f_readdir (&d, &fi)) == FR_OK && fi.fname[0] != 0)
        {
            Entry e;
            e.name = fi.fname;
            e.dir = (fi.fattrib & AM_DIR) != 0;
            e.hidden = (fi.fattrib & (AM_HID | AM_SYS)) != 0;
            e.size = fi.fsize;
            e.year = 1980 + (fi.fdate >> 9);
            e.month = (fi.fdate >> 5) & 15;
            e.day = fi.fdate & 31;
            e.hour = fi.ftime >> 11;
            e.minute = (fi.ftime >> 5) & 63;
            out.push_back (e);
        }
        f_closedir (&d);
        return r == FR_OK ? std::string() : describe (r);
    }

    std::string Volume::makeDir (const std::string& path)
    {
        const FRESULT r = f_mkdir (drivePath (path).c_str());
        return r == FR_OK || r == FR_EXIST ? std::string() : describe (r);
    }

    std::string Volume::add (const fs::path& host, const std::string& dir,
                             const std::function<bool (const std::string&)>& progress)
    {
        const std::string target = join (dir, utf8 (host.filename()));
        std::error_code ec;
        if (fs::is_directory (host, ec))
        {
            if (auto e = makeDir (target); ! e.empty())
                return target + ": " + e;
            for (auto& child : fs::directory_iterator (host, ec))
                if (auto e = add (child.path(), target, progress); ! e.empty())
                    return e;
            return ec ? utf8 (host) + ": " + ec.message() : std::string();
        }
        if (progress && ! progress (target))
            return "stopped";

        std::FILE* in = openFile (host, false);
        if (in == nullptr)
            return "cannot read " + utf8 (host);
        std::unique_ptr<std::FILE, int (*) (std::FILE*)> inGuard (in, std::fclose);
        FIL f;
        FRESULT r = f_open (&f, drivePath (target).c_str(), FA_WRITE | FA_CREATE_ALWAYS);
        if (r != FR_OK)
            return target + ": " + describe (r);
        std::vector<char> buf (1 << 20);
        size_t n;
        while (r == FR_OK && (n = std::fread (buf.data(), 1, buf.size(), in)) > 0)
        {
            UINT wrote = 0;
            r = f_write (&f, buf.data(), (UINT) n, &wrote);
            if (r == FR_OK && wrote != n)
                return f_close (&f), target + ": the image is full";
        }
        f_close (&f);
        if (r != FR_OK)
            return target + ": " + describe (r);

        // Keep the file's date.
        const auto mtime = fs::last_write_time (host, ec);
        if (! ec)
        {
            const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration> (
                mtime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::time_t t = std::chrono::system_clock::to_time_t (sys);
            std::tm tm {};
#ifdef _WIN32
            localtime_s (&tm, &t);
#else
            localtime_r (&t, &tm);
#endif
            if (tm.tm_year >= 80)
            {
                FILINFO fi {};
                fi.fdate = (WORD) ((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
                fi.ftime = (WORD) (tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
                f_utime (drivePath (target).c_str(), &fi);
            }
        }
        return {};
    }

    std::string Volume::extract (const std::string& path, const fs::path& hostDir,
                                 const std::function<bool (const std::string&)>& progress)
    {
        FILINFO fi;
        FRESULT r = f_stat (drivePath (path).c_str(), &fi);
        if (r != FR_OK)
            return path + ": " + describe (r);
        const fs::path out = hostDir / fromUtf8 (fi.fname);
        std::error_code ec;
        if (fi.fattrib & AM_DIR)
        {
            fs::create_directories (out, ec);
            if (ec)
                return utf8 (out) + ": " + ec.message();
            std::vector<Entry> children;
            if (auto e = list (path, children); ! e.empty())
                return path + ": " + e;
            for (auto& c : children)
                if (auto e = extract (join (path, c.name), out, progress); ! e.empty())
                    return e;
            return {};
        }
        if (progress && ! progress (path))
            return "stopped";
        FIL f;
        r = f_open (&f, drivePath (path).c_str(), FA_READ);
        if (r != FR_OK)
            return path + ": " + describe (r);
        std::ofstream os (out, std::ios::binary | std::ios::trunc);
        if (! os)
            return f_close (&f), "cannot write " + utf8 (out);
        std::vector<char> buf (1 << 20);
        UINT got = 0;
        while ((r = f_read (&f, buf.data(), (UINT) buf.size(), &got)) == FR_OK && got > 0)
            os.write (buf.data(), got);
        f_close (&f);
        if (r != FR_OK)
            return path + ": " + describe (r);
        return os ? std::string() : "cannot write " + utf8 (out);
    }

    std::string Volume::remove (const std::string& path)
    {
        FILINFO fi;
        FRESULT r = f_stat (drivePath (path).c_str(), &fi);
        if (r != FR_OK)
            return path + ": " + describe (r);
        if (fi.fattrib & AM_DIR)
        {
            DIR d;
            if ((r = f_opendir (&d, drivePath (path).c_str())) != FR_OK)
                return path + ": " + describe (r);
            std::vector<std::string> names;
            FILINFO c;
            while (f_readdir (&d, &c) == FR_OK && c.fname[0] != 0)
                names.push_back (c.fname);
            f_closedir (&d);
            for (auto& n : names)
                if (auto e = remove (join (path, n)); ! e.empty())
                    return e;
        }
        if (fi.fattrib & AM_RDO)
            f_chmod (drivePath (path).c_str(), 0, AM_RDO);
        r = f_unlink (drivePath (path).c_str());
        return r == FR_OK ? std::string() : path + ": " + describe (r);
    }

    uint64_t Volume::totalBytes() const
    {
        auto* v = (FATFS*) fs;
        return v != nullptr ? (uint64_t) (v->n_fatent - 2) * v->csize * 512 : 0;
    }

    uint64_t Volume::freeBytes() const
    {
        DWORD clusters = 0;
        FATFS* v = nullptr;
        if (f_getfree ("0:", &clusters, &v) != FR_OK || v == nullptr)
            return 0;
        return (uint64_t) clusters * v->csize * 512;
    }

    std::string Volume::label() const
    {
        char text[40] {};
        f_getlabel ("0:", text, nullptr);
        return text;
    }

    Format Volume::format() const
    {
        auto* v = (FATFS*) fs;
        return v != nullptr && v->fs_type == FS_EXFAT ? Format::exfat : Format::fat32;
    }
}
