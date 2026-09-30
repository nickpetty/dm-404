#include "EmulatorLink.h"
#include "Storage.h"

EmulatorLink::Paths EmulatorLink::defaultPaths()
{
    Paths p;
    p.qemu = Storage::qemu();
    p.firmware = Storage::firmware();
    p.flash = Storage::systemFlash();
    p.emmc = Storage::internalDrive();
    p.sd = Storage::sdCard();
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
    args.add ("sp404mk2,flash=" + p.flash.getFullPathName().replace (",", ",,") + ",link=link");
    args.add ("-bios");
    args.add (p.firmware.getFullPathName());
    args.add ("-chardev");
    args.add ("socket,id=link,host=127.0.0.1,port=" + juce::String (port) + ",server=on,wait=on");
    // QEMU option values double their commas.
    auto file = [] (const juce::File& f) { return f.getFullPathName().replace (",", ",,"); };
    // The SD slot is always there, so a card can go in later; empty is a
    // drive without a file.
    args.add ("-drive");
    args.add (p.sdInserted && p.sd.existsAsFile() ? "if=sd,index=0,format=raw,file=" + file (p.sd)
                                                  : juce::String ("if=sd,index=0"));
    if (p.emmc.existsAsFile())
    {
        args.add ("-drive");
        args.add ("if=sd,index=1,format=raw,file=" + file (p.emmc));
    }
    args.add ("-nographic");
    args.add ("-monitor");
    args.add ("none");
    args.add ("-serial");
    args.add ("none");

    if (! process.start (args, 0))
        return "Could not start " + p.qemu.getFullPathName();

    startThread (juce::Thread::Priority::high);
    inputSender.startThread();
    return {};
}

void EmulatorLink::stop()
{
    inputSender.signalThreadShouldExit();
    inReady.signal();
    inputSender.stopThread (1000);
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
    unitFrames = 0;             // the emulator counts from its connection too
    ++startCount;
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
            unitFrames += (uint32_t) frames;
            if (onAudioOut)
            {
                int16_t lr[1024 * 2];
                const int n = juce::jmin (frames, 1024);
                for (int i = 0; i < n * 2; ++i)
                    lr[i] = (int16_t) (data[i * 2] | (data[i * 2 + 1] << 8));
                onAudioOut (lr, n, unitFrames);
            }
            break;
        }

        case 0x03:
            if (len == 4 && onBmcPacket)
                onBmcPacket (data);
            break;

        case 0x06:
            if (onBuses)
            {
                int16_t b[1024 * 6];
                const int n = juce::jmin (len / 12, 1024);
                for (int i = 0; i < n * 6; ++i)
                    b[i] = (int16_t) (data[i * 2] | (data[i * 2 + 1] << 8));
                onBuses (b, n);
            }
            break;

        case 0x05:
            if (len == 8 && onUnitMidi)
                onUnitMidi ((uint32_t) (data[0] | data[1] << 8 | data[2] << 16 | (uint32_t) data[3] << 24), data + 4);
            break;

        case 0x04:
            if (len >= 1 && onSdCard)
                onSdCard (data[0] != 0, juce::String::fromUTF8 ((const char*) data + 1, len - 1));
            break;

        default:
            break;
    }
}

int EmulatorLink::readAudio (float* left, float* right, int frames)
{
    const float gain = outputGain.load() / 32768.0f;
    int start1, size1, start2, size2;
    audioFifo.prepareToRead (frames, start1, size1, start2, size2);
    auto copy = [&] (int start, int size, int to)
    {
        for (int i = 0; i < size; ++i)
        {
            left[to + i] = juce::jlimit (-1.0f, 1.0f, audioBuf[(size_t) (start + i) * 2] * gain);
            right[to + i] = juce::jlimit (-1.0f, 1.0f, audioBuf[(size_t) (start + i) * 2 + 1] * gain);
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

void EmulatorLink::sendAudioIn (const float* left, const float* right, int frames)
{
    // Audio thread: no locks, no socket. Frames that do not fit are dropped.
    int start1, size1, start2, size2;
    inFifo.prepareToWrite (frames, start1, size1, start2, size2);
    auto put = [&] (int start, int size, int from)
    {
        for (int i = 0; i < size; ++i)
        {
            inBuf[(size_t) (start + i) * 2] = (int16_t) juce::jlimit (-32768.0f, 32767.0f, left[from + i] * 32767.0f);
            inBuf[(size_t) (start + i) * 2 + 1] = (int16_t) juce::jlimit (-32768.0f, 32767.0f, right[from + i] * 32767.0f);
        }
    };
    put (start1, size1, 0);
    put (start2, size2, size1);
    inFifo.finishedWrite (size1 + size2);
    inReady.signal();
}

void EmulatorLink::InputSender::run()
{
    uint8_t buf[256 * 4];
    while (! threadShouldExit())
    {
        link.inReady.wait (20);
        while (link.inFifo.getNumReady() > 0 && ! threadShouldExit())
        {
            int start1, size1, start2, size2;
            link.inFifo.prepareToRead (256, start1, size1, start2, size2);
            int n = 0;
            auto take = [&] (int start, int size)
            {
                for (int i = 0; i < size; ++i, ++n)
                {
                    const int16_t l = link.inBuf[(size_t) (start + i) * 2];
                    const int16_t r = link.inBuf[(size_t) (start + i) * 2 + 1];
                    buf[n * 4] = (uint8_t) (l & 0xff);
                    buf[n * 4 + 1] = (uint8_t) ((l >> 8) & 0xff);
                    buf[n * 4 + 2] = (uint8_t) (r & 0xff);
                    buf[n * 4 + 3] = (uint8_t) ((r >> 8) & 0xff);
                }
            };
            take (start1, size1);
            take (start2, size2);
            link.inFifo.finishedRead (n);
            link.send (0x85, buf, n * 4);
        }
    }
}

void EmulatorLink::sendUsbAudio (const int16_t* lr, int frames)
{
    uint8_t buf[1024 * 4];
    frames = juce::jmin (frames, 1024);
    for (int i = 0; i < frames * 2; ++i)
    {
        buf[i * 2] = (uint8_t) (lr[i] & 0xff);
        buf[i * 2 + 1] = (uint8_t) ((lr[i] >> 8) & 0xff);
    }
    send (0x87, buf, frames * 4);
}

void EmulatorLink::sendBusesWanted (bool on)
{
    const uint8_t b = on ? 1 : 0;
    send (0x89, &b, 1);
}

void EmulatorLink::sendUsbMidi (const std::vector<UsbMidiEvent>& events)
{
    for (size_t i = 0; i < events.size(); i += 512)
    {
        uint8_t buf[512 * 8];
        const size_t n = juce::jmin ((size_t) 512, events.size() - i);
        for (size_t k = 0; k < n; ++k)
        {
            const auto& e = events[i + k];
            for (int b = 0; b < 4; ++b)
                buf[k * 8 + (size_t) b] = (uint8_t) (e.frame >> (8 * b));
            std::memcpy (buf + k * 8 + 4, e.packet, 4);
        }
        send (0x88, buf, (int) (n * 8));
    }
}

void EmulatorLink::sendSdCard (const juce::File& image)
{
    const auto path = image == juce::File() ? juce::String() : image.getFullPathName();
    send (0x86, path.toRawUTF8(), (int) path.getNumBytesAsUTF8());
}

void EmulatorLink::sendEncoder (int detents)
{
    while (detents != 0)
    {
        const int n = juce::jlimit (-127, 127, detents);
        const auto b = (uint8_t) (int8_t) n;
        send (0x84, &b, 1);
        detents -= n;
    }
}
