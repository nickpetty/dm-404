#include "WinMidi.h"

#include <atomic>
#include <chrono>
#include <mutex>

// What the port is doing, shared with its thread (which may outlive it).
struct WinMidiPortState
{
    std::atomic<WinMidiPort::State> state { WinMidiPort::State::starting };
    std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    mutable std::mutex lock;
    std::string reason;

    void fail (const std::string& r)
    {
        {
            std::lock_guard<std::mutex> l (lock);
            reason = r;
        }
        state = WinMidiPort::State::failed;
    }
    std::string why() const
    {
        std::lock_guard<std::mutex> l (lock);
        return reason;
    }
};

#if ! DM404_WINMIDI

struct WinMidiPort::Impl : WinMidiPortState
{
};

WinMidiPort::WinMidiPort (const std::wstring&, const std::wstring&, Receive) : impl (std::make_shared<Impl>())
{
    impl->fail ("built without Windows MIDI Services");
}

WinMidiPort::~WinMidiPort() = default;
void WinMidiPort::send (const uint8_t*, size_t) {}
bool WinMidiPort::available() { return false; }

#else

 #define WIN32_LEAN_AND_MEAN
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <roapi.h>
 #include <winstring.h>
 #include <condition_variable>
 #include <cstdio>
 #include <thread>
 #include <vector>

 #include <winrt/Windows.Foundation.h>
 #include <winrt/Windows.Foundation.Collections.h>
 #include <winrt/Windows.Devices.Midi2.h>
 #include <winrt/Windows.Devices.Midi2.Enumeration.h>
 #include <winrt/Windows.Devices.Midi2.Transports.Virtual.h>

namespace midi2 = winrt::Windows::Devices::Midi2;
namespace enumeration = winrt::Windows::Devices::Midi2::Enumeration;
namespace virt = winrt::Windows::Devices::Midi2::Transports::Virtual;

//==============================================================================
// Activation: Windows' own registration first (in-box, or installed); for the
// SDK's classes, a Windows.Devices.Midi2.dll the user put in the data folder.
namespace
{
    HMODULE userSdk = nullptr;
    using GetFactory = HRESULT (__stdcall*) (HSTRING, void**);
    GetFactory userGetFactory = nullptr;

    int32_t __stdcall activate (void* classId, winrt::guid const& iid, void** factory) noexcept
    {
        HRESULT hr = RoGetActivationFactory (static_cast<HSTRING> (classId), reinterpret_cast<GUID const&> (iid), factory);
        if (SUCCEEDED (hr) || userGetFactory == nullptr)
            return hr;
        UINT32 len = 0;
        const wchar_t* name = WindowsGetStringRawBuffer (static_cast<HSTRING> (classId), &len);
        if (std::wstring_view (name, len).rfind (L"Windows.Devices.Midi2.", 0) != 0)
            return hr;
        void* raw = nullptr;
        const HRESULT hr2 = userGetFactory (static_cast<HSTRING> (classId), &raw);
        if (FAILED (hr2) || raw == nullptr)
            return hr;
        auto* unknown = static_cast<::IUnknown*> (raw);
        const HRESULT hr3 = unknown->QueryInterface (reinterpret_cast<GUID const&> (iid), factory);
        unknown->Release();
        return hr3;
    }

    // MIDI 1.0 bytes -> UMP words (group 0), for sending.
    template <typename Fn>
    void toUmp (const uint8_t* d, size_t n, Fn&& out)
    {
        if (n == 0)
            return;
        const uint8_t s = d[0];
        if (s == 0xf0)
        {
            // SysEx: 7-bit packets of up to 6 bytes, without the F0/F7.
            const uint8_t* p = d + 1;
            size_t left = n - 1;
            if (left && p[left - 1] == 0xf7)
                --left;
            bool first = true;
            do
            {
                const size_t k = left < 6 ? left : 6;
                const bool last = left <= 6;
                const uint32_t status = first && last ? 0 : first ? 1 : last ? 3 : 2;
                uint8_t b[6] {};
                for (size_t i = 0; i < k; ++i)
                    b[i] = p[i];
                const uint32_t w0 = 0x30000000u | (status << 20) | ((uint32_t) k << 16) | ((uint32_t) b[0] << 8) | b[1];
                const uint32_t w1 = ((uint32_t) b[2] << 24) | ((uint32_t) b[3] << 16) | ((uint32_t) b[4] << 8) | b[5];
                out (w0, w1, 2);
                p += k;
                left -= k;
                first = false;
            } while (left > 0);
            return;
        }
        const uint32_t d1 = n > 1 ? d[1] : 0, d2 = n > 2 ? d[2] : 0;
        const uint32_t mt = s >= 0xf0 ? 0x1u : 0x2u;
        out ((mt << 28) | ((uint32_t) s << 16) | (d1 << 8) | d2, 0u, 1);
    }

    int systemLength (uint8_t s)
    {
        switch (s)
        {
            case 0xf1: case 0xf3: return 2;
            case 0xf2: return 3;
            default: return 1;
        }
    }

    std::string hex (uint32_t code)
    {
        char b[16];
        snprintf (b, sizeof (b), "0x%08x", code);
        return b;
    }
}

//==============================================================================
struct WinMidiPort::Impl : WinMidiPortState, std::enable_shared_from_this<WinMidiPort::Impl>
{
    std::wstring name, dllDir;
    std::mutex receiveLock;
    Receive receive;            // cleared when the port goes
    std::thread thread;
    std::condition_variable wake;
    bool quit = false;

    midi2::MidiSession session { nullptr };
    midi2::MidiEndpointConnection connection { nullptr };
    virt::MidiVirtualDevice device { nullptr };
    winrt::event_token token {};
    std::vector<uint8_t> sysex;
    uint64_t now = 0;           // the timestamp meaning "send at once"

    void begin()
    {
        thread = std::thread ([self = shared_from_this()] { self->run(); });
    }

    // The port is going: stop the thread, or leave it if it is stuck in
    // the service (it keeps this alive until it gets out).
    void end()
    {
        {
            std::lock_guard<std::mutex> l (receiveLock);
            receive = nullptr;
        }
        {
            std::lock_guard<std::mutex> l (lock);
            quit = true;
        }
        wake.notify_all();
        if (! thread.joinable())
            return;
        if (state == State::starting)
            thread.detach();
        else
            thread.join();
    }

    void deliver (const uint8_t* b, size_t n)
    {
        std::lock_guard<std::mutex> l (receiveLock);
        if (receive)
            receive (b, n);
    }

    void run()
    {
        // This thread holds the process's MTA for as long as the port lives
        // (sends come from other threads, which then share it).
        winrt::init_apartment (winrt::apartment_type::multi_threaded);
        try
        {
            start();
        }
        catch (winrt::hresult_error const& e)
        {
            const auto code = (uint32_t) e.code();
            if (code == (uint32_t) REGDB_E_CLASSNOTREG || code == (uint32_t) CLASS_E_CLASSNOTAVAILABLE)
                fail ("needs Windows MIDI Services");
            else
                fail (winrt::to_string (e.message()) + " (" + hex (code) + ")");
        }
        catch (...)
        {
            fail ("could not start");
        }

        std::unique_lock<std::mutex> l (lock);
        wake.wait (l, [this] { return quit; });
        l.unlock();

        try
        {
            if (connection != nullptr && token)
                connection.MessageReceived (token);
            if (session != nullptr)
                session.Close();
        }
        catch (...)
        {
        }
        connection = nullptr;
        device = nullptr;
        session = nullptr;
        winrt::uninit_apartment();
    }

    void start()
    {
        // The SDK: Windows' own, else the user's copy in the data folder.
        if (userSdk == nullptr)
        {
            const std::wstring dll = dllDir + L"\\Windows.Devices.Midi2.dll";
            if (GetFileAttributesW (dll.c_str()) != INVALID_FILE_ATTRIBUTES)
                if ((userSdk = LoadLibraryExW (dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) != nullptr)
                    userGetFactory = reinterpret_cast<GetFactory> (GetProcAddress (userSdk, "DllGetActivationFactory"));
        }
        winrt_activation_handler = activate;

        if (! midi2::MidiApi::EnsureServiceAvailable())
        {
            fail ("the Windows MIDI service is not running");
            return;
        }

        enumeration::MidiDeclaredEndpointInfo info;
        info.Name (name);
        info.ProductInstanceId (L"dm-404");
        info.HasStaticFunctionBlocks (true);
        info.SupportsMidi10Protocol (true);
        info.SupportsMidi20Protocol (false);
        info.SupportsReceivingJitterReductionTimestamps (false);
        info.SupportsSendingJitterReductionTimestamps (false);
        info.SpecificationVersionMajor (1);
        info.SpecificationVersionMinor (1);

        virt::MidiVirtualDeviceCreationConfig config (name, L"The DM-404 emulator's USB MIDI", L"dm-404", info);
        enumeration::MidiFunctionBlock block;
        block.Number (0);
        block.IsActive (true);
        block.Name (name);
        block.FirstGroup (midi2::MidiGroup (static_cast<uint8_t> (0)));
        block.GroupCount (1);
        block.Direction (enumeration::MidiFunctionBlockDirection::Bidirectional);
        block.RepresentsMidi10Connection (enumeration::MidiFunctionBlockRepresentsMidi10Connection::YesBandwidthUnrestricted);
        config.FunctionBlocks().Append (block);

        device = virt::MidiVirtualDeviceManager::CreateVirtualDevice (config);
        if (device == nullptr)
        {
            fail ("Windows MIDI Services would not make the device");
            return;
        }
        session = midi2::MidiSession::Create (name);
        if (session == nullptr)
        {
            fail ("no MIDI session");
            return;
        }
        connection = session.CreateEndpointConnection (device.DeviceEndpointDeviceId());
        if (connection == nullptr)
        {
            fail ("could not connect to the device");
            return;
        }
        std::weak_ptr<Impl> weak = shared_from_this();
        token = connection.MessageReceived ([weak] (midi2::IMidiMessageReceivedEventSource const&,
                                                    midi2::MidiMessageReceivedEventArgs const& args)
        {
            if (auto self = weak.lock())
            {
                uint32_t w[4] {};
                args.FillWords (w[0], w[1], w[2], w[3]);
                self->fromUmp (w);
            }
        });
        if (connection.AddMessageProcessingPlugin (device) != midi2::MidiMessageProcessingPluginAddResult::Succeeded)
        {
            fail ("could not attach the device");
            return;
        }
        connection.Open();
        now = midi2::MidiClock::TimestampConstantSendImmediately();
        state = State::ready;
    }

    void fromUmp (const uint32_t* w)
    {
        const uint32_t mt = w[0] >> 28;
        if (mt == 0x2 || mt == 0x1)
        {
            // MIDI 1.0 channel voice / system messages.
            const uint8_t b[3] = { (uint8_t) (w[0] >> 16), (uint8_t) ((w[0] >> 8) & 0x7f), (uint8_t) (w[0] & 0x7f) };
            int n = 3;
            if (mt == 0x1)
                n = systemLength (b[0]);
            else if ((b[0] & 0xf0) == 0xc0 || (b[0] & 0xf0) == 0xd0)
                n = 2;
            deliver (b, (size_t) n);
        }
        else if (mt == 0x3)
        {
            // SysEx, 6 bytes a packet.
            const uint32_t status = (w[0] >> 20) & 0xf, count = (w[0] >> 16) & 0xf;
            const uint8_t bytes[6] = { (uint8_t) (w[0] >> 8), (uint8_t) w[0], (uint8_t) (w[1] >> 24),
                                       (uint8_t) (w[1] >> 16), (uint8_t) (w[1] >> 8), (uint8_t) w[1] };
            if (status == 0 || status == 1)
                sysex.assign (1, 0xf0);
            for (uint32_t i = 0; i < count && i < 6; ++i)
                sysex.push_back (bytes[i] & 0x7f);
            if (status == 0 || status == 3)
            {
                sysex.push_back (0xf7);
                deliver (sysex.data(), sysex.size());
                sysex.clear();
            }
        }
        else if (mt == 0x4)
        {
            // MIDI 2.0 channel voice from a MIDI 2.0 client: the common ones.
            const uint8_t s = (uint8_t) (w[0] >> 16), op = s & 0xf0;
            uint8_t b[3] = { s, (uint8_t) ((w[0] >> 8) & 0x7f), 0 };
            int n = 3;
            if (op == 0x80 || op == 0x90)
            {
                b[2] = (uint8_t) (w[1] >> 25);
                if (op == 0x90 && b[2] == 0)
                    b[2] = 1;
            }
            else if (op == 0xa0 || op == 0xb0)
                b[2] = (uint8_t) (w[1] >> 25);
            else if (op == 0xc0)
            {
                b[1] = (uint8_t) ((w[1] >> 24) & 0x7f);
                n = 2;
            }
            else if (op == 0xd0)
            {
                b[1] = (uint8_t) (w[1] >> 25);
                n = 2;
            }
            else if (op == 0xe0)
            {
                const uint32_t v = w[1] >> 18;
                b[1] = (uint8_t) (v & 0x7f);
                b[2] = (uint8_t) ((v >> 7) & 0x7f);
            }
            else
                return;
            deliver (b, (size_t) n);
        }
    }

    void send (const uint8_t* bytes, size_t n)
    {
        if (state != State::ready || connection == nullptr)
            return;
        try
        {
            toUmp (bytes, n, [this] (uint32_t w0, uint32_t w1, int words)
            {
                if (words == 1)
                    connection.SendSingleMessageWords (now, w0);
                else
                    connection.SendSingleMessageWords (now, w0, w1);
            });
        }
        catch (...)
        {
        }
    }
};

WinMidiPort::WinMidiPort (const std::wstring& name, const std::wstring& dllDir, Receive receive)
    : impl (std::make_shared<Impl>())
{
    impl->name = name;
    impl->dllDir = dllDir;
    impl->receive = std::move (receive);
    impl->begin();
}

WinMidiPort::~WinMidiPort()
{
    impl->end();
}

void WinMidiPort::send (const uint8_t* bytes, size_t n)
{
    impl->send (bytes, n);
}

bool WinMidiPort::available() { return true; }

#endif

WinMidiPort::State WinMidiPort::state() const { return impl->state.load(); }
std::string WinMidiPort::why() const { return impl->why(); }

double WinMidiPort::secondsStarting() const
{
    return std::chrono::duration<double> (std::chrono::steady_clock::now() - impl->began).count();
}
