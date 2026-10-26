#pragma once
//==============================================================================
//  MidiRouter.h  -  physical MIDI output ports for the XIVA standalone
//
//  Two classes:
//    MidiRouter        - opens MIDI output ports on demand, sends the messages that the
//                        sequencer queues for them, and forwards live MIDI input (Thru).
//    MidiRoutingDialog - read-only overview of where each Network MIDI track sends, plus
//                        the Thru setting. A track's destination is chosen on the track
//                        itself (Track inspector, "OUT").
//
//  Data flow:
//    SequencerEngine (audio thread)  -> MidiRouter::enginePortSink() -> lock-free queue
//    sender thread                   -> MidiOutput::sendMessageNow()   (never on the audio thread)
//
//  Ports are opened the first time a track sends to them and are looked up by NAME, so a
//  route survives unplugging and re-plugging a device.
//==============================================================================

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_events/juce_events.h>
#include <array>
#include <memory>
#include <vector>

//==============================================================================
class MidiRouter : public juce::MidiInputCallback,
                   private juce::Thread
{
public:
    //==========================================================================
    MidiRouter() : juce::Thread ("XIVA MIDI out")
    {
        refresh();
        startThread();
    }

    ~MidiRouter() override
    {
        stopThread (1000);
        const juce::ScopedLock sl (routeLock);
        openOutputs.clear();
    }

    //==========================================================================
    //  Device list (message thread)
    //==========================================================================

    /** Rescans the available MIDI output ports. Call on device change events. */
    void refresh()
    {
        // Enumerate before taking the lock: this can be slow and must not block Thru.
        std::vector<std::pair<juce::String, juce::String>> found;   // name, identifier
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
            found.emplace_back (d.name, d.identifier);

        const juce::ScopedLock sl (routeLock);
        deviceNames.clear();
        deviceIdentifiers.clear();
        for (const auto& f : found)
        {
            deviceNames.add (f.first);
            deviceIdentifiers.add (f.second);
        }

        // Drop handles of ports that disappeared; they reopen by name when the port returns.
        for (int i = (int) openOutputs.size() - 1; i >= 0; --i)
            if (! deviceIdentifiers.contains (openOutputs[(size_t) i].id))
                openOutputs.erase (openOutputs.begin() + i);

        lastScanMs = juce::Time::getMillisecondCounter();
    }

    juce::StringArray getOutputDeviceNames() const
    {
        const juce::ScopedLock sl (routeLock);
        return deviceNames;
    }

    //==========================================================================
    //  Engine sink: Network MIDI tracks that send to a physical port
    //==========================================================================

    /** SequencerEngine::MidiPortSink. Audio-thread safe: only copies the message into a queue. */
    static void enginePortSink (void* context, const juce::String& portName, const juce::MidiMessage& message) noexcept
    {
        if (context != nullptr)
            static_cast<MidiRouter*> (context)->queue (portName, message);
    }

    //==========================================================================
    //  MIDI Thru (MidiInputCallback)
    //==========================================================================

    /** Port name to forward live MIDI input to, or "" for off. */
    void setThruOutput (const juce::String& portName)
    {
        const juce::ScopedLock sl (routeLock);
        thruPort = portName;
    }

    juce::String getThruOutput() const
    {
        const juce::ScopedLock sl (routeLock);
        return thruPort;
    }

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& msg) override
    {
        const juce::ScopedTryLock stl (routeLock);
        if (! stl.isLocked() || thruPort.isEmpty()) return;
        // Realtime messages (clock etc.) are not forwarded: they would double any clock the
        // destination already receives.
        if (msg.isMidiClock() || msg.isActiveSense()) return;
        if (auto* out = findOpenOutputLocked (thruPort))
            out->sendMessageNow (msg);
    }

private:
    //==========================================================================
    struct OpenOutput
    {
        juce::String id;
        std::unique_ptr<juce::MidiOutput> out;
    };

    struct Queued
    {
        juce::String port;
        juce::uint8  data[3] = { 0, 0, 0 };
        int          size = 0;
    };

    static constexpr int kQueueSize = 2048;

    void queue (const juce::String& port, const juce::MidiMessage& m) noexcept
    {
        const int n = m.getRawDataSize();
        if (n < 1 || n > 3) return;   // system-exclusive is not sent to track ports

        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 < 1) return;           // queue full: drop rather than block the audio thread
        auto& q = items[(size_t) s1];
        q.port = port;
        q.size = n;
        for (int i = 0; i < n; ++i) q.data[i] = m.getRawData()[i];
        fifo.finishedWrite (1);
        wake.signal();
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            wake.wait (2);
            while (fifo.getNumReady() > 0)
            {
                int s1, n1, s2, n2;
                fifo.prepareToRead (1, s1, n1, s2, n2);
                if (n1 < 1) break;
                const Queued q = items[(size_t) s1];
                fifo.finishedRead (1);
                send (q);
            }
        }
    }

    void send (const Queued& q)
    {
        const juce::MidiMessage msg (q.data, q.size);
        const juce::ScopedLock sl (routeLock);
        if (auto* out = findOpenOutputLocked (q.port))
            out->sendMessageNow (msg);
    }

    /** Finds (opening on demand) the output with this port name. Caller holds routeLock. */
    juce::MidiOutput* findOpenOutputLocked (const juce::String& name)
    {
        int idx = deviceNames.indexOf (name);
        if (idx < 0)
        {
            // Not in the list: maybe plugged in since the last scan. Rescan at most every 2 s.
            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastScanMs < 2000) return nullptr;
            lastScanMs = now;
            deviceNames.clear();
            deviceIdentifiers.clear();
            for (const auto& d : juce::MidiOutput::getAvailableDevices())
            {
                deviceNames.add (d.name);
                deviceIdentifiers.add (d.identifier);
            }
            idx = deviceNames.indexOf (name);
            if (idx < 0) return nullptr;
        }

        const juce::String& id = deviceIdentifiers[idx];
        for (auto& o : openOutputs)
            if (o.id == id) return o.out.get();

        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastOpenFailMs < 2000 && lastOpenFailMs != 0) return nullptr;
        if (auto out = juce::MidiOutput::openDevice (id))
        {
            auto* raw = out.get();
            openOutputs.push_back ({ id, std::move (out) });
            return raw;
        }
        lastOpenFailMs = now;
        return nullptr;
    }

    //==========================================================================
    mutable juce::CriticalSection routeLock;

    juce::StringArray deviceNames;        // parallel arrays, rebuilt by refresh()
    juce::StringArray deviceIdentifiers;
    std::vector<OpenOutput> openOutputs;
    juce::String thruPort;
    juce::uint32 lastScanMs = 0, lastOpenFailMs = 0;

    juce::AbstractFifo fifo { kQueueSize };
    std::array<Queued, kQueueSize> items;
    juce::WaitableEvent wake;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiRouter)
};

//==============================================================================
//  MidiRoutingDialog
//
//  Overview, not a matrix: the destination of a Network MIDI track is set on the track
//  (Track inspector -> OUT).  This dialog lists them and holds the Thru setting.
//==============================================================================
class MidiRoutingDialog : public juce::Component,
                          private juce::Timer
{
public:
    struct TrackOut
    {
        juce::String trackName;
        int          channel = 1;        // 1-16
        juce::String port;               // physical port name, or "" = network
        juce::String peer;               // network address when port is empty
    };

    MidiRoutingDialog (MidiRouter& router, std::vector<TrackOut> trackOutputs)
        : midiRouter (router), outputs (std::move (trackOutputs))
    {
        setSize (520, 300);
        startTimerHz (2);
        rebuild();
    }

    ~MidiRoutingDialog() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFF0D0D14));
        g.setColour (juce::Colour (0xFF1A2030));
        g.fillRect (0, 0, getWidth(), 28);
        g.fillRect (0, thruHeaderY, getWidth(), 28);

        g.setColour (juce::Colour (0xFF00CCAA));
        g.setFont (juce::Font (juce::FontOptions (13.f, juce::Font::bold)));
        g.drawText ("TRACK OUTPUTS", 8, 0, 400, 28, juce::Justification::centredLeft);
        g.drawText ("MIDI THRU",     8, thruHeaderY, 400, 28, juce::Justification::centredLeft);
    }

    void resized() override
    {
        // Layout is positional only (no rebuild), so resizing can never recurse.
        for (auto* c : rows) c->setSize (juce::jmax (100, getWidth() - 24), c->getHeight());
    }

private:
    void rebuild()
    {
        rows.clear();
        const auto ports = midiRouter.getOutputDeviceNames();
        int y = 32;

        auto addLabel = [this] (const juce::String& text, int x, int yy, int w, int h, juce::Colour col, float size = 12.f)
        {
            auto* l = new juce::Label ({}, text);
            l->setFont (juce::Font (juce::FontOptions (size)));
            l->setColour (juce::Label::textColourId, col);
            l->setBounds (x, yy, w, h);
            addAndMakeVisible (l);
            rows.add (l);
            return l;
        };

        const auto dim = juce::Colour (0xFF8A94A6), txt = juce::Colour (0xFFCCD0D8);
        const auto red = juce::Colour (0xFFE05A5A);

        if (outputs.empty())
        {
            addLabel ("No Network MIDI tracks yet. Add one from a network audio track (+ MIDI CHILD).", 12, y, getWidth() - 24, 22, dim);
            y += 26;
        }
        for (const auto& o : outputs)
        {
            const bool toPort = o.port.isNotEmpty();
            const bool missing = toPort && ! ports.contains (o.port);
            addLabel (o.trackName + "  (ch " + juce::String (o.channel) + ")", 12, y, 230, 22, txt);
            addLabel (toPort ? juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92 ")) + o.port + (missing ? "  (missing)" : "")
                             : juce::String (juce::CharPointer_UTF8 ("\xe2\x86\x92 Network ")) + o.peer,
                      248, y, getWidth() - 260, 22, missing ? red : dim);
            y += 24;
        }
        addLabel ("Choose a track's output in its inspector (OUT).", 12, y + 2, getWidth() - 24, 18, dim, 11.f);
        y += 28;

        thruHeaderY = y;
        y += 34;

        addLabel ("Forward live MIDI input to", 12, y, 170, 24, txt);
        auto* thruBox = new juce::ComboBox();
        thruBox->addItem ("(Off)", 1);
        for (int i = 0; i < ports.size(); ++i)
            thruBox->addItem (ports[i], i + 2);
        const int cur = ports.indexOf (midiRouter.getThruOutput());
        thruBox->setSelectedId (cur >= 0 ? cur + 2 : 1, juce::dontSendNotification);
        thruBox->setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xFF121820));
        thruBox->setColour (juce::ComboBox::textColourId, txt);
        thruBox->setBounds (186, y, getWidth() - 198, 24);
        thruWarning = addLabel ({}, 12, y + 30, getWidth() - 24, 32, red, 11.f);
        thruBox->onChange = [this, thruBox]
        {
            const auto name = thruBox->getSelectedId() <= 1 ? juce::String() : thruBox->getText();
            bool loops = false;
            for (const auto& in : juce::MidiInput::getAvailableDevices())
                if (name.isNotEmpty() && in.name == name) loops = true;
            if (loops)
            {
                thruBox->setSelectedId (1, juce::dontSendNotification);
                midiRouter.setThruOutput ({});
                thruWarning->setText ("This port is also a MIDI input. Forwarding to it would loop, so Thru stays off.",
                                      juce::dontSendNotification);
                return;
            }
            thruWarning->setText ({}, juce::dontSendNotification);
            midiRouter.setThruOutput (name);
        };
        addAndMakeVisible (thruBox);
        rows.add (thruBox);
        y += 70;

        lastPorts = ports;
        setSize (getWidth(), juce::jmax (200, y));
        repaint();
    }

    void timerCallback() override
    {
        // The list is refreshed by the host on device changes; follow it here.
        if (midiRouter.getOutputDeviceNames() != lastPorts)
            rebuild();
    }

    MidiRouter& midiRouter;
    std::vector<TrackOut> outputs;
    juce::StringArray lastPorts;
    juce::OwnedArray<juce::Component> rows;
    juce::Label* thruWarning = nullptr;
    int thruHeaderY = 200;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiRoutingDialog)
};
