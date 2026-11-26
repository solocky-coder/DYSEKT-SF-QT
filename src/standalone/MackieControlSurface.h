#pragma once
//==============================================================================
//  MackieControlSurface.h  -  generic Mackie Control (MCU) support, standalone only
//
//  Works with any surface that speaks the Mackie Control protocol (Mackie MCU /
//  Extender, Behringer X-Touch, Icon, Faderport, Yamaha 01X in its Logic mode, ...).
//  Nothing in here is specific to one device.
//
//  Protocol summary (what this class implements)
//    Surface -> host                                Host -> surface (feedback)
//      fader      pitch bend, MIDI ch 1-8 (9=main)    pitch bend (motor faders)
//      fader touch note 0x68+n (0x70 = main)
//      rec/solo/mute/select  notes 0x00/08/10/18+n  note on 127/0 = LED on/off
//      v-pot turn CC 0x10+n (bit6 = ccw, 0-5 = ticks)  CC 0x30+n = ring LEDs
//      v-pot push note 0x20+n
//      transport  notes 0x5B..0x5F, cycle 0x56
//      bank/channel buttons 0x2E..0x31, jog wheel CC 0x3C
//                                                   SysEx: LCD text (id 0x14)
//
//  Two modes (Track / Instrument buttons on the surface):
//    TRACKS  - strips are the arranger tracks (volume, pan, mute, solo, select)
//    SLICES  - strips are the Slicer's slices (volume, pan, select)
//  Fader 9 is always the master volume.
//
//  Threading: the MIDI input callback only queues messages; everything else runs on
//  the message thread (a 60 Hz timer drains the queue and refreshes the feedback).
//  The surface's input port is opened here, NOT through the AudioDeviceManager, so its
//  faders and buttons never reach the instrument as notes or pitch bend.
//==============================================================================

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include "../PluginProcessor.h"
#include "../params/ParamIds.h"
#include "../network/RtpMidiSession.h"
#include "../util/AppDataDir.h"

class MackieControlSurface : private juce::MidiInputCallback,
                             private juce::Timer
{
public:
    /** Windows numbers MIDI ports ("3- Axiom AIR 25") and the number changes when a device is
        replugged or the PC restarts, so ports are matched without that prefix. */
    static juce::String portKey (const juce::String& name)
    {
        auto s = name.trim();
        int i = 0;
        while (i < s.length() && juce::CharacterFunctions::isDigit (s[i])) ++i;
        if (i > 0 && s.substring (i).startsWith ("- "))
            s = s.substring (i + 2);
        return s.trim();
    }

    struct Settings
    {
        bool         enabled = false;
        juce::String inName;    // MIDI input port name  ("" = none)
        juce::String outName;   // MIDI output port name ("" = none: no motor/LED feedback)
        bool         network = false;   // also host a network MIDI session for a phone/tablet surface
        int          networkPort = 5006;
    };

    enum class Mode { Tracks, Slices };

    explicit MackieControlSurface (XivaProcessor& p) : processor (p)
    {
        settings = loadSettings();
        resetFeedbackCaches();
        startTimerHz (60);
        reopenPorts();
    }

    ~MackieControlSurface() override
    {
        stopTimer();
        netSession.onMidiReceived = nullptr;
        netSession.stop();
        shuttingDown = true;
        closePorts (true);
    }

    //==========================================================================
    //  Settings
    //==========================================================================
    static juce::File settingsFile() { return XivaPaths::appDataDir().getChildFile ("control-surface.json"); }

    static Settings loadSettings()
    {
        Settings s;
        const auto parsed = juce::JSON::parse (settingsFile());
        if (auto* o = parsed.getDynamicObject())
        {
            s.enabled = (bool) o->getProperty ("enabled");
            s.inName  = o->getProperty ("in").toString();
            s.outName = o->getProperty ("out").toString();
            s.network = (bool) o->getProperty ("network");
            if (o->hasProperty ("networkPort"))
                s.networkPort = juce::jlimit (1, 65533, (int) o->getProperty ("networkPort"));
        }
        return s;
    }

    static void saveSettings (const Settings& s)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("enabled", s.enabled);
        o->setProperty ("in", s.inName);
        o->setProperty ("out", s.outName);
        o->setProperty ("network", s.network);
        o->setProperty ("networkPort", s.networkPort);
        settingsFile().replaceWithText (juce::JSON::toString (juce::var (o)));
    }

    const Settings& getSettings() const noexcept { return settings; }

    void applySettings (const Settings& s)
    {
        settings = s;
        saveSettings (s);
        reopenPorts();
    }

    /** True when this input port is reserved for the surface and must not be given to the
        instrument / thru / RTP forwarder. */
    bool claimsInput (const juce::MidiDeviceInfo& d) const
    {
        return settings.enabled && settings.inName.isNotEmpty() && portKey (d.name) == portKey (settings.inName);
    }

    /** MainWindow supplies this so the surface can take its port away from the host's own
        MIDI input callbacks before opening it (Windows MIDI inputs are exclusive). */
    std::function<void (const juce::String& identifier)> releaseInputFromHost;
    /** Called when the surface lets go of a port again, so the host can hand it back to the instrument. */
    std::function<void (const juce::String& identifier)> returnInputToHost;

    //==========================================================================
    //  Status / log (message thread)
    //==========================================================================
    bool isInputOpen()  const noexcept { return midiIn  != nullptr; }
    bool isOutputOpen() const noexcept { return midiOut != nullptr; }
    bool isNetworkConnected() const { return settings.network && netSession.getState() == RtpMidiSession::State::Connected; }
    int  getNetworkPackets() const { return settings.network ? netSession.getPacketsSeen() : 0; }
    juce::String getNetworkStatus() const
    {
        if (! settings.network) return "Off";
        const int n = netSession.getPacketsSeen();
        return netSession.getStatusText() + "  [packets received: " + juce::String (n)
               + (n > 0 ? ", last from " + netSession.getLastSender() : juce::String (" - nothing has reached this PC")) + "]";
    }
    Mode getMode()      const noexcept { return mode; }
    int  getBankStart() const noexcept { return bankBase; }

    /** The most recent incoming messages, oldest first. */
    juce::StringArray getLog() const { return logLines; }
    void clearLog() { logLines.clear(); }

private:
    //==========================================================================
    //  Ports
    //==========================================================================
    void closePorts (bool includeMainOut)
    {
        juce::ignoreUnused (includeMainOut);
        if (midiIn != nullptr)
        {
            midiIn->stop();
            midiIn.reset();
        }
        if (claimedId.isNotEmpty())
        {
            const auto id = claimedId;
            claimedId.clear();
            if (returnInputToHost && ! shuttingDown) returnInputToHost (id);
        }
        {
            const juce::ScopedLock sl (inboxLock);
            inbox.clear();
        }
        midiOut.reset();
    }

    bool canSend() const { return midiOut != nullptr || (settings.network && netSession.getState() == RtpMidiSession::State::Connected); }

    void sendOut (const juce::MidiMessage& m)
    {
        if (midiOut != nullptr) midiOut->sendMessageNow (m);
        if (settings.network)   netSession.sendMessage (m);   // channel messages only; SysEx text stays on the cable
    }

    void reopenPorts()
    {
        closePorts (true);
        if (! settings.enabled)
            return;

        if (settings.inName.isNotEmpty())
            for (const auto& d : juce::MidiInput::getAvailableDevices())
                if (portKey (d.name) == portKey (settings.inName))
                {
                    if (releaseInputFromHost) releaseInputFromHost (d.identifier);
                    midiIn = juce::MidiInput::openDevice (d.identifier, this);
                    if (midiIn != nullptr) { midiIn->start(); claimedId = d.identifier; }
                    else if (returnInputToHost) returnInputToHost (d.identifier);   // could not open: keep it with the instrument
                    break;
                }

        if (settings.outName.isNotEmpty())
            for (const auto& d : juce::MidiOutput::getAvailableDevices())
                if (portKey (d.name) == portKey (settings.outName))
                {
                    midiOut = juce::MidiOutput::openDevice (d.identifier);
                    break;
                }

        netSession.stop();
        if (settings.network)
        {
            netSession.onMidiReceived = [this] (const juce::MidiMessage& m) { handleIncomingMidiMessage (nullptr, m); };
            netSession.startAsHost (settings.networkPort);
        }

        resetFeedbackCaches();
        if (settings.inName.isNotEmpty() && midiIn == nullptr)
        {
            bool found = false;
            for (const auto& d : juce::MidiInput::getAvailableDevices())
                if (portKey (d.name) == portKey (settings.inName)) found = true;
            addLog (found ? "Input '" + settings.inName + "' exists but could not be opened - another program may be using it (close it and re-select the port)."
                          : "Input '" + settings.inName + "' was not found - is the device plugged in?");
        }
        if (settings.outName.isNotEmpty() && midiOut == nullptr)
        {
            bool found = false;
            for (const auto& d : juce::MidiOutput::getAvailableDevices())
                if (portKey (d.name) == portKey (settings.outName)) found = true;
            addLog (found ? "Output '" + settings.outName + "' exists but could not be opened - another program may be using it."
                          : "Output '" + settings.outName + "' was not found - is the device plugged in?");
        }
        addLog (juce::String ("Control surface: cable input ") + (midiIn ? "open" : "not open")
                + ", cable output " + (midiOut ? "open" : "not open"));
    }

    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
    {
        const juce::ScopedLock sl (inboxLock);
        if (inbox.size() < 2048)
            inbox.push_back (m);
    }

    //==========================================================================
    //  Timer: drain input, refresh feedback
    //==========================================================================
    void timerCallback() override
    {
        std::vector<juce::MidiMessage> msgs;
        {
            const juce::ScopedLock sl (inboxLock);
            msgs.swap (inbox);
        }
        for (const auto& m : msgs)
            handle (m);

        if (settings.enabled && canSend() && ++feedbackDivider >= 3)
        {
            feedbackDivider = 0;
            updateFeedback();
        }
    }

    //==========================================================================
    //  Fader mapping:  0..1  <->  dB   (0.75 = 0 dB, top = +6 dB)
    //==========================================================================
    static float faderToDb (float pos, float minDb)
    {
        pos = juce::jlimit (0.0f, 1.0f, pos);
        if (pos <= 0.0f) return minDb;
        if (pos <= 0.75f) return -60.0f + (pos / 0.75f) * 60.0f;
        return (pos - 0.75f) / 0.25f * 6.0f;
    }

    static float dbToFader (float db)
    {
        if (db <= -60.0f) return 0.0f;
        if (db <= 0.0f)   return (db + 60.0f) / 60.0f * 0.75f;
        return juce::jmin (1.0f, 0.75f + db / 6.0f * 0.25f);
    }

    //==========================================================================
    //  Strips
    //==========================================================================
    struct StripState
    {
        bool         valid = false;
        juce::String name;
        float        volDb = 0.0f;
        float        pan = 0.0f;
        bool         mute = false, solo = false, selected = false;
    };

    int numStrips() const
    {
        if (mode == Mode::Tracks)
            return processor.sequencer.getNumTracks();
        const auto& s = processor.getUiSliceSnapshot();
        const int n = s.numSlices;
        processor.releaseUiSliceSnapshot();
        return n;
    }

    StripState readStrip (int index) const
    {
        StripState st;
        if (index < 0) return st;

        if (mode == Mode::Tracks)
        {
            auto& seq = processor.sequencer;
            if (index >= seq.getNumTracks()) return st;
            const auto info = seq.getTrackInfo (index);
            st.valid    = true;
            st.name     = arrangerTrackName (info);
            st.volDb    = info.volumeDb;
            st.pan      = info.pan;
            st.mute     = ! info.enabled;
            st.solo     = info.solo;
            st.selected = false;
            return st;
        }

        const auto& s = processor.getUiSliceSnapshot();
        if (index < s.numSlices)
        {
            const auto& sl = s.slices[(size_t) index];
            st.valid    = true;
            st.name     = sl.name.isNotEmpty() ? sl.name : juce::String (index + 1);
            st.volDb    = sl.volume;
            st.pan      = sl.pan;
            st.selected = (s.selectedSlice == index);
        }
        processor.releaseUiSliceSnapshot();
        return st;
    }

    void setStripVolume (int index, float pos01)
    {
        if (mode == Mode::Tracks)
        {
            if (index >= 0 && index < processor.sequencer.getNumTracks())
                processor.sequencer.setTrackVolumeDb (index, juce::jlimit (-60.0f, 6.0f, faderToDb (pos01, -60.0f)));
            return;
        }
        sliceParam (index, XivaProcessor::FieldVolume, juce::jlimit (-100.0f, 24.0f, faderToDb (pos01, -100.0f)));
    }

    void nudgeStripPan (int index, int ticks)
    {
        const auto st = readStrip (index);
        if (! st.valid) return;
        const float np = juce::jlimit (-1.0f, 1.0f, st.pan + (float) ticks * 0.02f);
        if (mode == Mode::Tracks) processor.sequencer.setTrackPan (index, np);
        else                      sliceParam (index, XivaProcessor::FieldPan, np);
    }

    void centreStripPan (int index)
    {
        const auto st = readStrip (index);
        if (! st.valid) return;
        if (mode == Mode::Tracks) processor.sequencer.setTrackPan (index, 0.0f);
        else                      sliceParam (index, XivaProcessor::FieldPan, 0.0f);
    }

    /** Slicer edits always act on the selected slice, so select it first (commands are
        processed in order). */
    void sliceParam (int index, int field, float value)
    {
        const auto& s = processor.getUiSliceSnapshot();
        const bool ok = index >= 0 && index < s.numSlices;
        const bool already = ok && s.selectedSlice == index;
        processor.releaseUiSliceSnapshot();
        if (! ok) return;

        if (! already)
        {
            XivaProcessor::Command sel;
            sel.type = XivaProcessor::CmdSelectSlice;
            sel.intParam1 = index;
            processor.pushCommand (sel);
        }
        XivaProcessor::Command cmd;
        cmd.type = XivaProcessor::CmdSetSliceParam;
        cmd.intParam1 = field;
        cmd.floatParam1 = value;
        processor.pushCommand (cmd);
    }

    float getMasterDb() const
    {
        return processor.apvts.getRawParameterValue (ParamIds::masterVolume)->load();
    }

    void setMasterDb (float db)
    {
        if (auto* p = processor.apvts.getParameter (ParamIds::masterVolume))
            p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (-100.0f, 24.0f, db)));
    }

    //==========================================================================
    //  Input handling
    //==========================================================================
    void handle (const juce::MidiMessage& m)
    {
        logMessage (m);

        if (m.isSysEx())
        {
            handleSysEx (m);
            return;
        }

        if (m.isPitchWheel())
        {
            const int ch = m.getChannel() - 1;           // 0..8
            if (ch < 0 || ch > 8) return;
            const int value = m.getPitchWheelValue();    // 0..16383
            lastPb[(size_t) ch] = value;
            const float pos = (float) value / 16383.0f;
            if (ch == 8) setMasterDb (faderToDb (pos, -100.0f));
            else         setStripVolume (bankBase + ch, pos);
            return;
        }

        if (m.isNoteOnOrOff())
        {
            handleNote (m.getNoteNumber(), m.isNoteOn() && m.getVelocity() > 0);
            return;
        }

        if (m.isController())
            handleController (m.getControllerNumber(), m.getControllerValue());
    }

    void handleNote (int note, bool down)
    {
        // Fader touch (0x68..0x6F strips, 0x70 main)
        if (note >= 0x68 && note <= 0x70)
        {
            const int n = note - 0x68;
            touched[(size_t) n] = down;
            if (! down) lastPb[(size_t) n] = -1000;      // resend the real position on release
            return;
        }
        if (! down) return;                               // everything below acts on press

        const int strip = bankBase;
        if (note >= 0x08 && note < 0x10)                  // SOLO
        {
            const int i = strip + (note - 0x08);
            if (mode == Mode::Tracks && i < processor.sequencer.getNumTracks())
                processor.sequencer.setTrackSolo (i, ! processor.sequencer.getTrackInfo (i).solo);
        }
        else if (note >= 0x10 && note < 0x18)             // MUTE
        {
            const int i = strip + (note - 0x10);
            if (mode == Mode::Tracks && i < processor.sequencer.getNumTracks())
                processor.sequencer.setTrackEnabled (i, ! processor.sequencer.getTrackInfo (i).enabled);
        }
        else if (note >= 0x18 && note < 0x20)             // SELECT
        {
            const int i = strip + (note - 0x18);
            if (mode == Mode::Tracks)
            {
                if (i < processor.sequencer.getNumTracks()) processor.sequencer.setSelectedTrack (i);
            }
            else
            {
                XivaProcessor::Command sel;
                sel.type = XivaProcessor::CmdSelectSlice;
                sel.intParam1 = i;
                processor.pushCommand (sel);
            }
        }
        else if (note >= 0x20 && note < 0x28)             // V-POT push = centre pan
        {
            centreStripPan (strip + (note - 0x20));
        }
        else
        {
            auto& seq = processor.sequencer;
            switch (note)
            {
                case 0x28: mode = Mode::Tracks;  bankBase = 0; resetFeedbackCaches(); break;   // TRACK
                case 0x2D: mode = Mode::Slices;  bankBase = 0; resetFeedbackCaches(); break;   // INSTRUMENT
                case 0x2E: moveBank (-8); break;
                case 0x2F: moveBank (+8); break;
                case 0x30: moveBank (-1); break;
                case 0x31: moveBank (+1); break;
                case 0x56: seq.setLooping (! seq.isLooping()); break;                           // CYCLE
                case 0x5B: seq.seekToTick (juce::jmax<int64_t> (0, seq.getPlayheadTick() - 4 * 960)); break;  // REW: back 1 bar
                case 0x5C: seq.seekToTick (seq.getPlayheadTick() + 4 * 960); break;                            // FFWD: fwd 1 bar
                case 0x5D: seq.stop(); break;                                                   // STOP
                case 0x5E: seq.play(); break;                                                   // PLAY
                case 0x5F: seq.setRecording (! seq.isRecording()); break;                       // RECORD
                default: break;
            }
        }
    }

    static int decodeRelative (int v) { return (v & 0x40) ? -(v & 0x3F) : (v & 0x3F); }

    void handleController (int cc, int value)
    {
        if (cc >= 0x10 && cc < 0x18)                      // V-POT turn
            nudgeStripPan (bankBase + (cc - 0x10), decodeRelative (value));
        else if (cc == 0x3C)                              // JOG: sixteenth notes
        {
            auto& seq = processor.sequencer;
            seq.seekToTick (juce::jmax<int64_t> (0, seq.getPlayheadTick() + (int64_t) decodeRelative (value) * (960 / 4)));
        }
    }

    void moveBank (int delta)
    {
        const int n = juce::jmax (0, numStrips() - 1);
        bankBase = juce::jlimit (0, n, bankBase + delta);
        resetFeedbackCaches();
    }

    //==========================================================================
    //  Host connection handshake (the surface asks; we answer)
    //==========================================================================
    void handleSysEx (const juce::MidiMessage& m)
    {
        const auto* d = m.getSysExData();
        const int   n = m.getSysExDataSize();
        // MIDI Machine Control (7F <device> 06 <command>): many controller apps send transport this way
        if (n >= 4 && d[0] == 0x7F && d[2] == 0x06)
        {
            auto& seq = processor.sequencer;
            switch (d[3])
            {
                case 0x01: case 0x09: seq.stop(); break;
                case 0x02: case 0x03: seq.play(); break;
                case 0x04: seq.seekToTick (seq.getPlayheadTick() + 4 * 960); break;
                case 0x05: seq.seekToTick (juce::jmax<int64_t> (0, seq.getPlayheadTick() - 4 * 960)); break;
                case 0x06: seq.setRecording (true);  break;
                case 0x07: seq.setRecording (false); break;
                default: break;
            }
            return;
        }
        // 00 00 66 <id> 01 <serial x7> <challenge x4>   ->   reply 02 with the response
        if (n >= 16 && d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x66 && d[4] == 0x01)
        {
            const int c1 = d[12], c2 = d[13], c3 = d[14], c4 = d[15];
            uint8_t reply[16] = { 0x00, 0x00, 0x66, d[3], 0x02,
                                  d[5], d[6], d[7], d[8], d[9], d[10], d[11],
                                  (uint8_t) (0x7F & (c1 + (c2 ^ 0x0A) - c4)),
                                  (uint8_t) (0x7F & ((c3 >> 4) ^ (c1 + c4))),
                                  (uint8_t) (0x7F & ((c4 - (c3 << 2)) ^ (c1 | c2))),
                                  (uint8_t) (0x7F & (c2 - c3 + (0xF0 ^ (c4 << 4)))) };
            sendOut (juce::MidiMessage::createSysExMessage (reply, 16));
        }
    }

    //==========================================================================
    //  Feedback (motor faders, LEDs, V-Pot rings, LCD)
    //==========================================================================
    void resetFeedbackCaches()
    {
        lastPb.fill (-1000);
        lastLed.fill (-1);
        lastRing.fill (-1);
        lastTopRow.clear();
        lastBottomRow.clear();
    }

    void sendNote (int note, bool on)
    {
        const int v = on ? 1 : 0;
        if (lastLed[(size_t) note] == v) return;
        lastLed[(size_t) note] = v;
        sendOut (juce::MidiMessage::noteOn (1, note, (juce::uint8) (on ? 127 : 0)));
    }

    void sendPitch (int ch, float pos01)
    {
        const int value = juce::jlimit (0, 16383, juce::roundToInt (pos01 * 16383.0f));
        if (std::abs (value - lastPb[(size_t) ch]) < 48) return;
        lastPb[(size_t) ch] = value;
        sendOut (juce::MidiMessage::pitchWheel (ch + 1, value));
    }

    void sendText (int offset, const juce::String& text)
    {
        std::vector<uint8_t> data { 0x00, 0x00, 0x66, 0x14, 0x12, (uint8_t) offset };
        for (int i = 0; i < text.length(); ++i)
        {
            const auto c = text[i];
            data.push_back ((uint8_t) ((c >= 32 && c < 127) ? c : '?'));
        }
        sendOut (juce::MidiMessage::createSysExMessage (data.data(), (int) data.size()));
    }

    static juce::String fit (const juce::String& s, int w)
    {
        return s.substring (0, w).paddedRight (' ', w);
    }

    void updateFeedback()
    {
        auto& seq = processor.sequencer;

        juce::String top, bottom;
        for (int n = 0; n < 8; ++n)
        {
            const auto st = readStrip (bankBase + n);
            const int idx = bankBase + n;
            juce::ignoreUnused (idx);

            if (! touched[(size_t) n] && st.valid)
                sendPitch (n, dbToFader (st.volDb));
            else if (! st.valid && ! touched[(size_t) n])
                sendPitch (n, 0.0f);

            sendNote (0x08 + n, st.valid && st.solo);
            sendNote (0x10 + n, st.valid && st.mute);
            sendNote (0x18 + n, st.valid && st.selected);

            // V-pot ring: single dot, position 1..11 (centre LED lit at pan 0)
            int ring = 0;
            if (st.valid)
            {
                const int pos = 1 + juce::roundToInt ((juce::jlimit (-1.0f, 1.0f, st.pan) + 1.0f) * 5.0f);
                ring = pos | (std::abs (st.pan) < 0.02f ? 0x40 : 0);
            }
            if (lastRing[(size_t) n] != ring)
            {
                lastRing[(size_t) n] = ring;
                sendOut (juce::MidiMessage::controllerEvent (1, 0x30 + n, ring));
            }

            top    += fit (st.valid ? st.name : juce::String(), 6) + " ";
            bottom += fit (st.valid ? (st.volDb <= -59.9f ? "-inf" : juce::String (st.volDb, 1)) : juce::String(), 6) + " ";
        }
        top    = top.substring (0, 56);
        bottom = bottom.substring (0, 56);
        if (top != lastTopRow)       { lastTopRow = top;       sendText (0x00, top); }
        if (bottom != lastBottomRow) { lastBottomRow = bottom; sendText (0x38, bottom); }

        // Master fader
        if (! touched[8])
            sendPitch (8, dbToFader (getMasterDb()));

        // Mode and transport lights
        sendNote (0x28, mode == Mode::Tracks);
        sendNote (0x2D, mode == Mode::Slices);
        sendNote (0x56, seq.isLooping());
        sendNote (0x5D, ! seq.isPlaying());
        sendNote (0x5E, seq.isPlaying());
        sendNote (0x5F, seq.isRecording());
    }

    //==========================================================================
    //  Log
    //==========================================================================
    void addLog (const juce::String& line)
    {
        logLines.add (line);
        while (logLines.size() > 120) logLines.remove (0);
    }

    void logMessage (const juce::MidiMessage& m)
    {
        // Fader and jog streams would flood the log: keep one line per channel/controller.
        if (m.isPitchWheel())
        {
            const juce::String key = "Fader ch " + juce::String (m.getChannel());
            const auto line = key + "  value " + juce::String (m.getPitchWheelValue());
            if (logLines.size() > 0 && logLines[logLines.size() - 1].startsWith (key)) logLines.set (logLines.size() - 1, line);
            else addLog (line);
            return;
        }
        if (m.isSysEx())            { addLog ("SysEx (" + juce::String (m.getSysExDataSize()) + " bytes)"); return; }
        if (m.isNoteOnOrOff())      { addLog (juce::String (m.isNoteOn() && m.getVelocity() > 0 ? "Note on  " : "Note off ")
                                              + "0x" + juce::String::toHexString (m.getNoteNumber()) + "  ch " + juce::String (m.getChannel())); return; }
        if (m.isController())       { addLog ("CC 0x" + juce::String::toHexString (m.getControllerNumber())
                                              + " = " + juce::String (m.getControllerValue()) + "  ch " + juce::String (m.getChannel())); return; }
        addLog (m.getDescription());
    }

    //==========================================================================
    XivaProcessor& processor;
    Settings settings;
    Mode mode = Mode::Tracks;
    int  bankBase = 0;
    int  feedbackDivider = 0;
    juce::String claimedId;
    bool shuttingDown = false;

    std::unique_ptr<juce::MidiInput>  midiIn;
    std::unique_ptr<juce::MidiOutput> midiOut;
    RtpMidiSession netSession;

    juce::CriticalSection              inboxLock;
    std::vector<juce::MidiMessage>     inbox;

    std::array<bool, 9>  touched {};
    std::array<int, 9>   lastPb {};
    std::array<int, 128> lastLed {};
    std::array<int, 8>   lastRing {};
    juce::String         lastTopRow, lastBottomRow;
    juce::StringArray    logLines;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MackieControlSurface)
};
