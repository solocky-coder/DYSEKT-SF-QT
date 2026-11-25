#pragma once
//==============================================================================
//  MidiRouter.h  -  physical MIDI output ports for the XIVA standalone
//
//  MidiRouter opens MIDI output ports on demand, sends the messages that the sequencer
//  queues for them, and forwards live MIDI input (Thru, using the saved setting).
//  A track's destination is chosen on the track itself (Track inspector, "OUT").
//
//  Data flow:
//    SequencerEngine (audio thread)  -> MidiRouter::enginePortSink() -> lock-free queue
//    sender thread                   -> MidiOutput::sendMessageNow()   (never on the audio thread)
//
//  Ports are opened the first time a track sends to them and are looked up by NAME, so a
//  route survives unplugging and re-plugging a device.
//==============================================================================

#include <juce_audio_devices/juce_audio_devices.h>
#include "../util/AppDataDir.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_events/juce_events.h>
#include "../sequencer/SequencerEngine.h"
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
        loadThruSetting();
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
    static void enginePortSink (void* context, const juce::String& portName, const juce::MidiMessage& message, double delayMs) noexcept
    {
        if (context != nullptr)
            static_cast<MidiRouter*> (context)->queue (portName, message, delayMs);
    }

    //==========================================================================
    //  MIDI Thru (MidiInputCallback)
    //==========================================================================

    /** Port name to forward live MIDI input to, or "" for off. */
    void setThruOutput (const juce::String& portName)
    {
        {
            const juce::ScopedLock sl (routeLock);
            thruPort = portName;
        }
        saveThruSetting (portName);
    }

    juce::String getThruOutput() const
    {
        const juce::ScopedLock sl (routeLock);
        return thruPort;
    }

    /** Live input for the selected Network MIDI track goes out from here, on the MIDI input
        thread, instead of waiting for the next audio block. */
    void setLiveEngine (SequencerEngine* engine) noexcept { liveEngine.store (engine, std::memory_order_release); }

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& msg) override
    {
        route (msg, true);
    }

    /** Same as a normal MIDI input, but `forwardToNetworkTrack` = false keeps the message
        out of the selected Network MIDI track (used so a device never gets its own notes back). */
    void route (const juce::MidiMessage& msg, bool forwardToNetworkTrack)
    {
        if (forwardToNetworkTrack)
            if (auto* e = liveEngine.load (std::memory_order_acquire))
                e->forwardLiveMidi (msg);

        const juce::ScopedTryLock stl (routeLock);
        if (! stl.isLocked() || thruPort.isEmpty()) return;
        // Realtime messages (clock etc.) are not forwarded: they would double any clock the
        // destination already receives.
        if (msg.isMidiClock() || msg.isActiveSense()) return;
        if (auto* out = findOpenOutputLocked (thruPort))
            out->sendMessageNow (msg);
    }

private:
    // The Thru port is remembered between launches (app-level setting, not part of a project).
    static juce::File thruSettingFile()
    {
        return XivaPaths::appDataDir().getChildFile ("midi-thru.xml");
    }
    void loadThruSetting()
    {
        if (auto x = juce::XmlDocument::parse (thruSettingFile()))
            if (x->hasTagName ("MIDITHRU"))
                thruPort = x->getStringAttribute ("port");
    }
    static void saveThruSetting (const juce::String& port)
    {
        juce::XmlElement x ("MIDITHRU");
        x.setAttribute ("port", port);
        const auto f = thruSettingFile();
        f.getParentDirectory().createDirectory();
        x.writeTo (f);
    }

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
        juce::int64  dueTicks = 0;   // high-resolution tick at which to send
    };

    static constexpr int kQueueSize = 2048;

    void queue (const juce::String& port, const juce::MidiMessage& m, double delayMs) noexcept
    {
        const int n = m.getRawDataSize();
        if (n < 1 || n > 3) return;   // system-exclusive is not sent to track ports

        const juce::SpinLock::ScopedLockType producer (producerLock);
        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 < 1) return;           // queue full: drop rather than block the audio thread
        auto& q = items[(size_t) s1];
        q.port = port;
        q.size = n;
        q.dueTicks = juce::Time::getHighResolutionTicks()
                   + (juce::int64) (juce::jmax (0.0, delayMs) * 0.001 * (double) juce::Time::getHighResolutionTicksPerSecond());
        for (int i = 0; i < n; ++i) q.data[i] = m.getRawData()[i];
        fifo.finishedWrite (1);
        wake.signal();
    }

    void run() override
    {
        const double tps = (double) juce::Time::getHighResolutionTicksPerSecond();
        std::vector<Queued> pending;
        pending.reserve (kQueueSize);

        while (! threadShouldExit())
        {
            // Sleep until the next event is due (at most 1 ms when idle so new items are noticed).
            int waitMs = 1;
            if (! pending.empty())
            {
                juce::int64 next = pending.front().dueTicks;
                for (const auto& q : pending) next = juce::jmin (next, q.dueTicks);
                const double msLeft = (double) (next - juce::Time::getHighResolutionTicks()) * 1000.0 / tps;
                waitMs = msLeft <= 0.0 ? 0 : juce::jlimit (0, 1, (int) msLeft);
            }
            if (waitMs > 0) wake.wait (waitMs);

            while (fifo.getNumReady() > 0)
            {
                int s1, n1, s2, n2;
                fifo.prepareToRead (1, s1, n1, s2, n2);
                if (n1 < 1) break;
                if (pending.size() < (size_t) kQueueSize) pending.push_back (items[(size_t) s1]);
                fifo.finishedRead (1);
            }

            const auto now = juce::Time::getHighResolutionTicks();
            size_t keep = 0;
            for (size_t i = 0; i < pending.size(); ++i)
            {
                if (pending[i].dueTicks <= now) send (pending[i]);
                else                            pending[keep++] = pending[i];
            }
            pending.resize (keep);
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

    std::atomic<SequencerEngine*> liveEngine { nullptr };
    juce::SpinLock producerLock;   // the audio thread (clips) and the MIDI thread (live) both queue
    juce::AbstractFifo fifo { kQueueSize };
    std::array<Queued, kQueueSize> items;
    juce::WaitableEvent wake;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiRouter)
};
