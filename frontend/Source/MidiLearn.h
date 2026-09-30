#pragma once

#include <JuceHeader.h>

// MIDI learn: notes and controllers from the computer's MIDI inputs mapped
// to panel controls, by the control's printed name. Right-click a control,
// "MIDI learn", then press a key or move a knob on the MIDI device.
//
// Mapped messages work the panel and do not reach the unit; the rest play
// it as before (its MIDI IN). A message maps to one control (learning it
// again moves it); a control can have several. Kept in the settings
// ("midiMap").
//
// take() runs on the MIDI thread; what it maps is handed to the message
// thread through onControl.
class MidiLearn
{
public:
    // How a controller turns VALUE (an endless encoder): an absolute knob
    // (the change from its last value), or a relative encoder in one of
    // the two common codings (1 / 127 = +1 / -1; 65 / 63 = +1 / -1).
    enum class Mode { absolute, twosComplement, offset };

    struct Map
    {
        bool note = false;          // a note (else a controller)
        int channel = 1;            // 1-16
        int number = 0;             // note or controller number
        juce::String control;
        Mode mode = Mode::absolute;

        juce::String describe() const
        {
            return (note ? juce::MidiMessage::getMidiNoteName (number, true, true, 3) + " (note " + juce::String (number) + ")"
                         : "CC " + juce::String (number))
                   + ", channel " + juce::String (channel);
        }
    };

    // A mapped message, for the panel: note on (value = velocity), note
    // off (0) or controller value; mode as learnt.
    std::function<void (const juce::String& control, bool note, int value, Mode)> onControl;
    // Learning finished: the control and what it now answers to.
    std::function<void (const juce::String& control, const juce::String& what)> onLearnt;

    explicit MidiLearn (juce::PropertiesFile& s) : settings (s) { load(); }

    // Start learning for a control (empty: stop).
    void learn (const juce::String& control)
    {
        const juce::ScopedLock sl (lock);
        learning = control;
    }
    juce::String learningControl() const
    {
        const juce::ScopedLock sl (lock);
        return learning;
    }

    // What a control answers to ("" if nothing).
    juce::String describe (const juce::String& control) const
    {
        const juce::ScopedLock sl (lock);
        juce::StringArray what;
        for (auto& m : maps)
            if (m.control == control)
                what.add (m.describe());
        return what.joinIntoString ("; ");
    }

    void forget (const juce::String& control)
    {
        {
            const juce::ScopedLock sl (lock);
            maps.erase (std::remove_if (maps.begin(), maps.end(), [&] (const Map& m) { return m.control == control; }),
                        maps.end());
        }
        save();
    }

    void forgetAll()
    {
        {
            const juce::ScopedLock sl (lock);
            maps.clear();
        }
        save();
    }

    bool empty() const
    {
        const juce::ScopedLock sl (lock);
        return maps.empty();
    }

    // A message from a MIDI input (MIDI thread): true if it was learnt or
    // is mapped, and so is not for the unit.
    bool take (const juce::MidiMessage& m)
    {
        const bool noteOn = m.isNoteOn(), noteOff = m.isNoteOff(), cc = m.isController();
        if (! (noteOn || noteOff || cc))
            return false;
        const bool note = ! cc;
        const int number = cc ? m.getControllerNumber() : m.getNoteNumber();
        const int value = cc ? m.getControllerValue() : noteOn ? m.getVelocity() : 0;
        juce::String control, learnt, what;
        Mode mode = Mode::absolute;
        {
            const juce::ScopedLock sl (lock);
            if (learning.isNotEmpty())
            {
                // A key's release, or a stray note off: wait for a press.
                if (noteOff)
                    return true;
                Map n { note, m.getChannel(), number, learning, Mode::absolute };
                // An encoder's first step reads as +-1 in its coding; an
                // absolute knob is anywhere.
                if (cc && (value == 1 || value == 127))
                    n.mode = Mode::twosComplement;
                else if (cc && (value == 63 || value == 65))
                    n.mode = Mode::offset;
                maps.erase (std::remove_if (maps.begin(), maps.end(), [&] (const Map& o) { return same (o, n); }),
                            maps.end());
                maps.push_back (n);
                learnt = learning;
                what = n.describe();
                learning.clear();
            }
            else
            {
                const Map key { note, m.getChannel(), number, {}, Mode::absolute };
                for (auto& o : maps)
                    if (same (o, key))
                    {
                        control = o.control;
                        mode = o.mode;
                        break;
                    }
                if (control.isEmpty())
                    return false;
            }
        }
        if (learnt.isNotEmpty())
        {
            juce::MessageManager::callAsync ([this, alive = std::weak_ptr<bool> (alive), learnt, what]
            {
                if (alive.expired())
                    return;
                save();
                if (onLearnt)
                    onLearnt (learnt, what);
            });
            return true;
        }
        juce::MessageManager::callAsync ([this, alive = std::weak_ptr<bool> (alive), control, note, value, mode]
        {
            if (! alive.expired() && onControl)
                onControl (control, note, value, mode);
        });
        return true;
    }

private:
    static bool same (const Map& a, const Map& b)
    {
        return a.note == b.note && a.channel == b.channel && a.number == b.number;
    }

    void load()
    {
        const auto v = juce::JSON::parse (settings.getValue ("midiMap"));
        if (auto* a = v.getArray())
            for (auto& e : *a)
                maps.push_back ({ (bool) e["note"], (int) e["channel"], (int) e["number"],
                                  e["control"].toString(), (Mode) (int) e["mode"] });
    }

    void save()
    {
        juce::Array<juce::var> a;
        {
            const juce::ScopedLock sl (lock);
            for (auto& m : maps)
            {
                auto* o = new juce::DynamicObject();
                o->setProperty ("note", m.note);
                o->setProperty ("channel", m.channel);
                o->setProperty ("number", m.number);
                o->setProperty ("control", m.control);
                o->setProperty ("mode", (int) m.mode);
                a.add (juce::var (o));
            }
        }
        settings.setValue ("midiMap", juce::JSON::toString (juce::var (a), true));
        settings.saveIfNeeded();
    }

    juce::PropertiesFile& settings;
    juce::CriticalSection lock;
    std::vector<Map> maps;
    juce::String learning;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);  // for callbacks after it is gone
};
