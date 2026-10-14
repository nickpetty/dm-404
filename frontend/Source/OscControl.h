#pragma once

#include "PanelComponent.h"

// OSC control: the panel from TouchOSC (or any OSC app) over the network,
// with the LEDs and knobs sent back. tools/touchosc_layout.py makes a
// TouchOSC layout that uses these addresses (frontend/Resources/DM-404.tosc).
//
// In (UDP, the listen port, 8000 by default), one number each (float, int
// or T/F; none counts as 1):
//   /dm404/key/<id>    a key held while > 0 (bus_fx, filter_drive, a_f, ...)
//   /dm404/pad/<n>     pad 1-16 held while > 0, at that velocity (0-1)
//   /dm404/knob/<id>   volume, ctrl_1, ctrl_2, ctrl_3: 0-1
//   /dm404/value       VALUE as an endless knob, 0-1 wrapping (24 steps a turn)
//   /dm404/value/step  VALUE turned by that many steps (+ or -)
//   /dm404/value/push  VALUE pushed while > 0
//   /dm404/sync        send everything back now
// <id> is the printed name in lower case, other characters as "_".
//
// Out, to every device heard from in the last minute, at the reply port
// (9000 by default; TouchOSC's usual pair):
//   /dm404/led/<id>     the key's LED, 0-1
//   /dm404/led/pad/<n>  level, r, g, b (0-1) of the pad's LED
//   /dm404/knob/<id>    a knob moved from elsewhere (mouse, MIDI)
//
// Off until switched on (Options > TouchOSC): it opens a network port.
class OscControl : private juce::Thread, private juce::Timer
{
public:
    OscControl (PanelComponent& p, juce::PropertiesFile& s) : juce::Thread ("OSC in"), panel (p), settings (s)
    {
        if (settings.getBoolValue ("oscEnabled", false))
            start();
    }

    ~OscControl() override
    {
        *alive = false;
        stop();
    }

    bool isEnabled() const { return running; }
    int listenPort() const { return settings.getIntValue ("oscPort", 8000); }
    int replyPort() const { return settings.getIntValue ("oscReplyPort", 9000); }

    void setEnabled (bool on)
    {
        settings.setValue ("oscEnabled", on);
        settings.saveIfNeeded();
        stop();
        if (on)
            start();
    }

    void setPorts (int in, int out)
    {
        settings.setValue ("oscPort", in);
        settings.setValue ("oscReplyPort", out);
        settings.saveIfNeeded();
        if (running)
        {
            stop();
            start();
        }
    }

    // For the settings window: what it is doing.
    juce::String status() const
    {
        if (! running)
            return error.isNotEmpty() ? error : "Off";
        const juce::ScopedLock sl (lock);
        juce::String s = "Listening on port " + juce::String (listenPort());
        const auto now = juce::Time::getMillisecondCounter();
        for (auto& [ip, seen] : clients)
            s << "\nHeard from " << ip << " " << (int) ((now - seen) / 1000) << " s ago";
        if (clients.empty())
            s << "\nNothing heard yet";
        return s;
    }

    // The printed name as an OSC address part.
    static juce::String idFor (const juce::String& name)
    {
        juce::String id;
        for (auto ch : name.toLowerCase())
            id << (juce::CharacterFunctions::isLetterOrDigit (ch) ? juce::String::charToString (ch)
                                                                  : juce::String ("_"));
        while (id.contains ("__"))
            id = id.replace ("__", "_");
        return id.trimCharactersAtStart ("_").trimCharactersAtEnd ("_");
    }

private:
    void start()
    {
        error.clear();
        in = std::make_unique<juce::DatagramSocket> (false);
        if (! in->bindToPort (listenPort()))
        {
            error = "Port " + juce::String (listenPort()) + " is in use (another app?)";
            in.reset();
            return;
        }
        out = std::make_unique<juce::DatagramSocket> (false);
        running = true;
        sent.clear();
        startThread();
        startTimerHz (25);
    }

    void stop()
    {
        stopTimer();
        running = false;
        signalThreadShouldExit();
        if (in != nullptr)
            in->shutdown();
        stopThread (1000);
        in.reset();
        out.reset();
        const juce::ScopedLock sl (lock);
        clients.clear();
    }

    //==========================================================================
    // In: the network thread reads packets and hands the panel its part.
    void run() override
    {
        std::vector<char> buf (65536);
        while (! threadShouldExit())
        {
            if (in->waitUntilReady (true, 200) != 1)
                continue;
            juce::String ip;
            int port = 0;
            const int n = in->read (buf.data(), (int) buf.size(), false, ip, port);
            if (n <= 0)
                continue;
            {
                const juce::ScopedLock sl (lock);
                if (clients.find (ip) == clients.end())
                    fresh.insert (ip);
                clients[ip] = juce::Time::getMillisecondCounter();
            }
            packet (reinterpret_cast<const uint8_t*> (buf.data()), (size_t) n, ip, 0);
        }
    }

    static uint32_t be32 (const uint8_t* p) { return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3]; }
    static size_t pad4 (size_t n) { return (n + 3) & ~(size_t) 3; }

    // A message or a bundle of them.
    void packet (const uint8_t* p, size_t n, const juce::String& ip, int depth)
    {
        if (n >= 16 && std::memcmp (p, "#bundle", 8) == 0)
        {
            if (depth > 4)
                return;
            for (size_t at = 16; at + 4 <= n;)
            {
                const size_t len = be32 (p + at);
                at += 4;
                if (len > n - at)
                    return;
                packet (p + at, len, ip, depth + 1);
                at += len;
            }
            return;
        }
        // The address, the type tags, then the first argument as a number.
        const auto* end = static_cast<const uint8_t*> (std::memchr (p, 0, n));
        if (end == nullptr || p[0] != '/')
            return;
        const juce::String address (reinterpret_cast<const char*> (p), (size_t) (end - p));
        size_t at = pad4 ((size_t) (end - p) + 1);
        float value = 1.0f;
        if (at < n && p[at] == ',')
        {
            const auto* tagsEnd = static_cast<const uint8_t*> (std::memchr (p + at, 0, n - at));
            if (tagsEnd == nullptr)
                return;
            const char tag = (tagsEnd - (p + at)) > 1 ? (char) p[at + 1] : 0;
            const size_t arg = pad4 ((size_t) (tagsEnd - p) + 1);
            if ((tag == 'f' || tag == 'i') && arg + 4 <= n)
            {
                const uint32_t bits = be32 (p + arg);
                float f;
                std::memcpy (&f, &bits, 4);
                value = tag == 'f' ? f : (float) (int32_t) bits;
            }
            else if (tag == 'd' && arg + 8 <= n)
            {
                const uint64_t bits = (uint64_t) be32 (p + arg) << 32 | be32 (p + arg + 4);
                double d;
                std::memcpy (&d, &bits, 8);
                value = (float) d;
            }
            else if (tag == 'F' || tag == 'N')
                value = 0.0f;
        }
        message (address, value, ip);
    }

    void message (const juce::String& address, float v, const juce::String& ip)
    {
        if (! address.startsWith ("/dm404/"))
            return;
        const auto path = address.substring (7);
        juce::MessageManager::callAsync ([this, alive = std::weak_ptr<bool> (alive), path, v, ip]
        {
            if (alive.expired())
                return;
            handle (path, v, ip);
        });
    }

    // On the message thread.
    void handle (const juce::String& path, float v, const juce::String& ip)
    {
        if (path == "sync")
        {
            const juce::ScopedLock sl (lock);
            fresh.insert (ip);
            return;
        }
        if (path == "value")
        {
            // An endless knob: the change from its last position, wrapped.
            if (lastValue >= 0.0f)
            {
                float d = v - lastValue;
                if (d > 0.5f)
                    d -= 1.0f;
                if (d < -0.5f)
                    d += 1.0f;
                valueAccum += d * 24.0f;
                const int steps = (int) valueAccum;
                if (steps != 0)
                {
                    valueAccum -= (float) steps;
                    panel.turnValue (steps);
                }
            }
            lastValue = v;
            return;
        }
        if (path == "value/step")
        {
            if (const int steps = juce::roundToInt (v); steps != 0)
                panel.turnValue (steps);
            return;
        }
        if (path == "value/push")
        {
            panel.fromOsc ("VALUE", v);
            return;
        }
        const auto kind = path.upToFirstOccurrenceOf ("/", false, false);
        const auto id = path.fromFirstOccurrenceOf ("/", false, false);
        for (auto& s : panel.shownState())
        {
            const bool pad = s.type == PanelControl::Type::pad;
            const bool knob = s.type == PanelControl::Type::knob;
            if ((kind == "pad" && pad && s.name == id) || (kind == "key" && ! pad && ! knob && idFor (s.name) == id)
                || (kind == "knob" && knob && idFor (s.name) == id))
            {
                if (knob)
                    knobFromOsc[s.name] = juce::Time::getMillisecondCounter();
                panel.fromOsc (s.name, v);
                return;
            }
        }
    }

    //==========================================================================
    // Out: what changed since the last look, to every device heard from.
    void timerCallback() override
    {
        std::vector<juce::String> targets, news;
        {
            const juce::ScopedLock sl (lock);
            const auto now = juce::Time::getMillisecondCounter();
            for (auto it = clients.begin(); it != clients.end();)
                if (now - it->second > 60000)
                    it = clients.erase (it);
                else
                    targets.push_back ((it++)->first);
            news.assign (fresh.begin(), fresh.end());
            fresh.clear();
        }
        if (targets.empty() || out == nullptr)
            return;
        const auto now = juce::Time::getMillisecondCounter();
        for (auto& s : panel.shownState())
        {
            juce::String address;
            std::vector<float> args;
            if (s.type == PanelControl::Type::knob)
            {
                if (s.name == "VALUE")
                    continue;
                // Not while it is being moved from here: it would fight the finger.
                if (auto it = knobFromOsc.find (s.name); it != knobFromOsc.end() && now - it->second < 500)
                {
                    sent[s.name] = { s.level };
                    continue;
                }
                address = "/dm404/knob/" + idFor (s.name);
                args = { s.level };
            }
            else if (s.type == PanelControl::Type::pad)
            {
                address = "/dm404/led/pad/" + s.name;
                args = { s.level, s.r, s.g, s.b };
            }
            else
            {
                address = "/dm404/led/" + idFor (s.name);
                args = { s.level };
            }
            auto& last = sent[s.name];
            const bool changed = last != args;
            last = args;
            if (changed)
                for (auto& t : targets)
                    send (t, address, args);
            else
                for (auto& t : news)
                    send (t, address, args);
        }
    }

    void send (const juce::String& ip, const juce::String& address, const std::vector<float>& args)
    {
        std::vector<uint8_t> m;
        auto str = [&m] (const juce::String& s)
        {
            const auto* c = s.toRawUTF8();
            m.insert (m.end(), c, c + std::strlen (c));
            m.push_back (0);
            while (m.size() % 4)
                m.push_back (0);
        };
        str (address);
        str ("," + juce::String::repeatedString ("f", (int) args.size()));
        for (float f : args)
        {
            uint32_t bits;
            std::memcpy (&bits, &f, 4);
            for (int sh = 24; sh >= 0; sh -= 8)
                m.push_back ((uint8_t) (bits >> sh));
        }
        out->write (ip, replyPort(), m.data(), (int) m.size());
    }

    PanelComponent& panel;
    juce::PropertiesFile& settings;
    std::unique_ptr<juce::DatagramSocket> in, out;
    std::atomic<bool> running { false };
    juce::String error;
    juce::CriticalSection lock;
    std::map<juce::String, uint32_t> clients;      // ip -> last heard (ms)
    std::set<juce::String> fresh;                 // to be sent everything
    std::map<juce::String, std::vector<float>> sent;
    std::map<juce::String, uint32_t> knobFromOsc;  // control -> last moved over OSC
    float lastValue = -1.0f, valueAccum = 0.0f;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
};
