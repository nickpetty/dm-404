#include "EmulatorLink.h"

EmulatorLink::Paths EmulatorLink::defaultPaths()
{
    // Walk up from the executable to the repo root (the folder holding
    // firmware/ and build/).
    auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    while (dir.exists() && ! dir.getChildFile ("firmware").isDirectory())
    {
        auto parent = dir.getParentDirectory();
        if (parent == dir)
            break;
        dir = parent;
    }

    Paths p;
    p.qemu = dir.getChildFile ("build/qemu/qemu-system-arm.exe");
    p.firmware = dir.getChildFile ("firmware/SP404MKII_APP1.bin");
    p.flash = dir.getChildFile ("build/flash.bin");
    p.emmc = dir.getChildFile ("build/emmc.img");
    p.sd = dir.getChildFile ("build/sd.img");
    return p;
}

EmulatorLink::EmulatorLink() : juce::Thread ("emulator link") {}

EmulatorLink::~EmulatorLink()
{
    stop();
}

juce::String EmulatorLink::start (const Paths& p)
{
    stop();

    if (! p.qemu.existsAsFile())
        return "Emulator core not built: " + p.qemu.getFullPathName();
    if (! p.firmware.existsAsFile())
        return "Roland firmware missing: " + p.firmware.getFullPathName();

    juce::StringArray args;
    args.add (p.qemu.getFullPathName());
    args.add ("-M");
    args.add ("sp404mk2,flash=" + p.flash.getFullPathName() + ",link=link");
    args.add ("-bios");
    args.add (p.firmware.getFullPathName());
    args.add ("-chardev");
    args.add ("socket,id=link,host=127.0.0.1,port=" + juce::String (port) + ",server=on,wait=on");
    if (p.sd.existsAsFile())
    {
        args.add ("-drive");
        args.add ("if=sd,index=0,format=raw,file=" + p.sd.getFullPathName());
    }
    if (p.emmc.existsAsFile())
    {
        args.add ("-drive");
        args.add ("if=sd,index=1,format=raw,file=" + p.emmc.getFullPathName());
    }
    args.add ("-nographic");
    args.add ("-monitor");
    args.add ("none");
    args.add ("-serial");
    args.add ("none");

    if (! process.start (args, 0))
        return "Could not start " + p.qemu.getFullPathName();

    startThread (juce::Thread::Priority::high);
    return {};
}

void EmulatorLink::stop()
{
    signalThreadShouldExit();
    {
        const juce::ScopedLock sl (sendLock);
        if (socket != nullptr)
            socket->close();
    }
    stopThread (2000);
    if (process.isRunning())
        process.kill();
    connected = false;
}

void EmulatorLink::run()
{
    // QEMU waits for us before it starts the machine; keep trying while it
    // comes up.
    auto s = std::make_unique<juce::StreamingSocket>();
    for (int i = 0; i < 100 && ! threadShouldExit(); ++i)
    {
        if (s->connect ("127.0.0.1", port, 200))
            break;
        wait (100);
    }
    if (! s->isConnected())
        return;
    {
        const juce::ScopedLock sl (sendLock);
        socket = std::move (s);
    }
    connected = true;

    std::vector<uint8_t> buf;
    uint8_t chunk[8192];
    while (! threadShouldExit())
    {
        const int ready = socket->waitUntilReady (true, 100);
        if (ready < 0)
            break;
        if (ready == 0)
            continue;
        const int n = socket->read (chunk, sizeof (chunk), false);
        if (n <= 0)
            break;
        buf.insert (buf.end(), chunk, chunk + n);

        size_t pos = 0;
        while (buf.size() - pos >= 4)
        {
            const int len = buf[pos + 2] | (buf[pos + 3] << 8);
            if (buf.size() - pos < (size_t) (4 + len))
                break;
            handleMessage (buf[pos], buf.data() + pos + 4, len);
            pos += 4 + (size_t) len;
        }
        buf.erase (buf.begin(), buf.begin() + (ptrdiff_t) pos);
    }
    connected = false;
}

void EmulatorLink::handleMessage (uint8_t type, const uint8_t* data, int len)
{
    switch (type)
    {
        case 0x01:
            if (len == (int) screen.size())
            {
                const juce::SpinLock::ScopedLockType sl (screenLock);
                std::memcpy (screen.data(), data, screen.size());
                ++screenCount;
            }
            break;

        case 0x02:
        {
            const int frames = len / 4;
            int start1, size1, start2, size2;
            audioFifo.prepareToWrite (frames, start1, size1, start2, size2);
            auto copy = [&] (int start, int size, int from)
            {
                for (int i = 0; i < size; ++i)
                {
                    auto* f = data + (from + i) * 4;
                    audioBuf[(size_t) (start + i) * 2] = (int16_t) (f[0] | (f[1] << 8));
                    audioBuf[(size_t) (start + i) * 2 + 1] = (int16_t) (f[2] | (f[3] << 8));
                }
            };
            copy (start1, size1, 0);
            copy (start2, size2, size1);
            audioFifo.finishedWrite (size1 + size2);   // excess is dropped
            break;
        }

        case 0x03:
            if (len == 4 && onBmcPacket)
                onBmcPacket (data);
            break;

        default:
            break;
    }
}

int EmulatorLink::readAudio (float* left, float* right, int frames)
{
    int start1, size1, start2, size2;
    audioFifo.prepareToRead (frames, start1, size1, start2, size2);
    auto copy = [&] (int start, int size, int to)
    {
        for (int i = 0; i < size; ++i)
        {
            left[to + i] = audioBuf[(size_t) (start + i) * 2] / 32768.0f;
            right[to + i] = audioBuf[(size_t) (start + i) * 2 + 1] / 32768.0f;
        }
    };
    copy (start1, size1, 0);
    copy (start2, size2, size1);
    audioFifo.finishedRead (size1 + size2);
    return size1 + size2;
}

EmulatorLink::Screen EmulatorLink::getScreen() const
{
    const juce::SpinLock::ScopedLockType sl (screenLock);
    return screen;
}

void EmulatorLink::send (uint8_t type, const void* data, int len)
{
    const juce::ScopedLock sl (sendLock);
    if (socket == nullptr || ! connected)
        return;
    uint8_t hdr[4] = { type, 0, (uint8_t) (len & 0xff), (uint8_t) (len >> 8) };
    socket->write (hdr, 4);
    socket->write (data, len);
}

void EmulatorLink::sendKey (int row, int col, bool pressed)
{
    const uint8_t p[3] = { (uint8_t) row, (uint8_t) col, (uint8_t) (pressed ? 1 : 0) };
    send (0x81, p, 3);
}

void EmulatorLink::sendKnob (int adc, int channel, int mux, int value)
{
    const uint8_t p[5] = { (uint8_t) adc, (uint8_t) channel, (uint8_t) mux,
                           (uint8_t) (value & 0xff), (uint8_t) ((value >> 8) & 0x0f) };
    send (0x82, p, 5);
}

void EmulatorLink::sendBmc (const uint8_t packet[4])
{
    send (0x83, packet, 4);
}
