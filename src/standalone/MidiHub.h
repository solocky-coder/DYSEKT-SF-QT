#pragma once
//==============================================================================
//  MidiHub.h  -  ONE window for every kind of MIDI connection
//
//    INPUTS           every cable/USB MIDI input: On (plays the instrument) or Off.
//    CONTROL SURFACE  Mackie Control: cable/USB input + output together, or a phone/tablet
//                     over the network (Session B, both directions), plus a live monitor.
//    NETWORK          Session A (port 5004): keyboard / pads from a phone or tablet.
//    OUTPUTS          track output overview.
//
//  Both network sessions show their status, how many packets reached this PC and the
//  exact address to type into the phone / tablet app. Ports are fixed, so they never clash.
//==============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <functional>
#include "MackieControlSurface.h"
#include "MidiRouter.h"
#include "NetworkMidiSettings.h"
#include "../network/NetworkMidiManager.h"

//==============================================================================
//  Remembers which cable inputs were switched Off, across launches.
//==============================================================================
namespace MidiInputRoles
{
    inline juce::File file() { return XivaPaths::appDataDir().getChildFile ("midi-inputs-off.json"); }

    inline juce::StringArray loadOff()
    {
        juce::StringArray out;
        const auto parsed = juce::JSON::parse (file());
        if (auto* a = parsed.getArray())
            for (const auto& v : *a) out.add (v.toString());
        return out;
    }

    inline bool isOff (const juce::String& portName)
    {
        const auto key = MackieControlSurface::portKey (portName);
        for (const auto& n : loadOff())
            if (MackieControlSurface::portKey (n) == key) return true;
        return false;
    }

    inline void setOff (const juce::String& portName, bool off)
    {
        auto names = loadOff();
        const auto key = MackieControlSurface::portKey (portName);
        for (int i = names.size(); --i >= 0;)
            if (MackieControlSurface::portKey (names[i]) == key) names.remove (i);
        if (off) names.add (portName);
        juce::Array<juce::var> a;
        for (const auto& n : names) a.add (n);
        file().replaceWithText (juce::JSON::toString (juce::var (a)));
    }
}

//==============================================================================
struct MidiHubHost
{
    juce::AudioDeviceManager&  deviceManager;
    MackieControlSurface&      surface;
    MidiRouter&                router;
    NetworkMidiManager&        network;

    std::function<bool (const juce::String& id)> isInstrument;     // input delivered to the instrument?
    std::function<void (const juce::String& id)> instrumentOn;
    std::function<void (const juce::String& id)> instrumentOff;
    std::function<void()> openNetworkAdvanced;                      // USB device, channels, clock
    std::function<void()> openTrackOutputs;                         // track output overview
};

class MidiHubComponent final : public juce::Component,
                               private juce::Timer
{
public:
    static constexpr int kSessionAPort = 5004;

    explicit MidiHubComponent (MidiHubHost h) : host (std::move (h))
    {
        setOpaque (true);
        build();
        startTimerHz (2);
    }

    ~MidiHubComponent() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (bg);
        for (const auto& s : sections)
        {
            g.setColour (header);
            g.fillRect (0, s.y, getWidth(), 26);
            g.setColour (accent);
            g.setFont (juce::Font (juce::FontOptions (13.f, juce::Font::bold)));
            g.drawText (s.title, 10, s.y, getWidth() - 20, 26, juce::Justification::centredLeft);
        }
    }

private:
    //==========================================================================
    const juce::Colour bg     { 0xFF0D0D14 };
    const juce::Colour header { 0xFF1A2030 };
    const juce::Colour accent { 0xFF00CCAA };
    const juce::Colour text   { 0xFFCCD0D8 };
    const juce::Colour dim    { 0xFF8A94A6 };
    const juce::Colour red    { 0xFFE05A5A };

    struct Section { int y; juce::String title; };
    std::vector<Section> sections;
    juce::OwnedArray<juce::Component> items;

    MidiHubHost host;
    juce::StringArray lastInputIds, lastOutputNames;

    // live widgets
    juce::ToggleButton sessionA, sessionB;
    juce::Label statusA, statusB, addrA, addrB, surfaceInfo;
    juce::ComboBox surfaceInBox, surfaceOutBox;
    juce::TextButton advancedBtn { "More options..." }, trackOutBtn { "Track outputs overview..." };
    juce::TextEditor monitor;
    juce::String lastMonitor;

    //==========================================================================
    template <typename T> T* own (T* c) { items.add (c); addAndMakeVisible (c); return c; }

    juce::Label* label (const juce::String& t, juce::Rectangle<int> r, juce::Colour col, float size = 12.f)
    {
        auto* l = own (new juce::Label ({}, t));
        l->setFont (juce::Font (juce::FontOptions (size)));
        l->setColour (juce::Label::textColourId, col);
        l->setBounds (r);
        return l;
    }

    void styleLabel (juce::Label& l, juce::Rectangle<int> r, juce::Colour col, float size = 12.f)
    {
        l.setFont (juce::Font (juce::FontOptions (size)));
        l.setColour (juce::Label::textColourId, col);
        l.setBounds (r);
        addAndMakeVisible (l);
    }

    void styleCombo (juce::ComboBox& c)
    {
        c.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xFF121820));
        c.setColour (juce::ComboBox::textColourId, text);
    }

    static juce::String thisPcAddresses()
    {
        juce::StringArray out;
        for (const auto& a : juce::IPAddress::getAllAddresses (false))
        {
            const auto s = a.toString();
            if (! a.isIPv6 && ! s.startsWith ("127.") && ! s.startsWith ("169.254."))
                out.addIfNotAlreadyThere (s);
        }
        return out.isEmpty() ? juce::String ("<this PC's IP>") : out.joinIntoString (" or ");
    }

    //==========================================================================
    //  Build (also re-run when ports are plugged / unplugged)
    //==========================================================================
    void build()
    {
        removeAllChildren();
        items.clear();
        sections.clear();

        const int W = 640;
        const int pad = 12;
        int y = 0;

        //── INPUTS ────────────────────────────────────────────────────────────
        sections.push_back ({ y, "INPUTS  -  cable / USB MIDI (what plays the instrument)" });
        y += 34;
        const auto inputs = juce::MidiInput::getAvailableDevices();
        lastInputIds.clear();
        for (const auto& d : inputs) lastInputIds.add (d.identifier);

        if (inputs.isEmpty())
        {
            label ("No MIDI input devices found.", { pad, y, W - 2 * pad, 22 }, dim);
            y += 28;
        }
        for (const auto& d : inputs)
        {
            label (d.name, { pad, y, 330, 24 }, text, 13.f);
            auto* state = own (new juce::ComboBox());
            styleCombo (*state);
            if (host.surface.claimsInput (d))
            {
                state->addItem ("Used by control surface", 1);
                state->setSelectedId (1, juce::dontSendNotification);
                state->setEnabled (false);
            }
            else
            {
                state->addItem ("On", 1);
                state->addItem ("Off", 3);
                state->setSelectedId (host.isInstrument && host.isInstrument (d.identifier) ? 1 : 3,
                                      juce::dontSendNotification);
                const auto dev = d;
                state->onChange = [this, state, dev] { applyInputState (dev, state->getSelectedId() == 1); };
            }
            state->setBounds (350, y, W - 350 - pad, 24);
            y += 30;
        }
        y += 6;

        //── CONTROL SURFACE ───────────────────────────────────────────────────
        sections.push_back ({ y, "CONTROL SURFACE  -  Mackie Control (faders, pan, transport)" });
        y += 34;
        const auto outPorts = host.router.getOutputDeviceNames();
        lastOutputNames = outPorts;
        const auto cur = host.surface.getSettings();

        label ("Cable / USB input", { pad, y, 150, 24 }, text);
        surfaceInBox.clear (juce::dontSendNotification);
        surfaceInBox.addItem ("(None)", 1);
        for (int i = 0; i < inputs.size(); ++i) surfaceInBox.addItem (inputs[i].name, i + 2);
        int sel = 1;
        for (int i = 0; i < inputs.size(); ++i) if (MackieControlSurface::portKey (inputs[i].name) == MackieControlSurface::portKey (cur.inName)) sel = i + 2;
        surfaceInBox.setSelectedId (sel, juce::dontSendNotification);
        styleCombo (surfaceInBox);
        surfaceInBox.setBounds (170, y, W - 170 - pad, 24);
        surfaceInBox.onChange = [this] { applySurfaceIn(); };
        addAndMakeVisible (surfaceInBox);
        y += 30;

        label ("Cable / USB output", { pad, y, 150, 24 }, text);
        surfaceOutBox.clear (juce::dontSendNotification);
        surfaceOutBox.addItem ("(None)", 1);
        for (int i = 0; i < outPorts.size(); ++i) surfaceOutBox.addItem (outPorts[i], i + 2);
        int si = -1;
        for (int i = 0; i < outPorts.size(); ++i)
            if (MackieControlSurface::portKey (outPorts[i]) == MackieControlSurface::portKey (cur.outName)) si = i;
        surfaceOutBox.setSelectedId (si >= 0 ? si + 2 : 1, juce::dontSendNotification);
        styleCombo (surfaceOutBox);
        surfaceOutBox.setBounds (170, y, W - 170 - pad, 24);
        surfaceOutBox.onChange = [this] { applySurfaceOut(); };
        addAndMakeVisible (surfaceOutBox);
        y += 28;
        label ("Output = motor faders, lights and display feedback.", { pad, y, W - 2 * pad, 18 }, dim, 11.f);
        y += 24;

        const auto ips = thisPcAddresses();
        auto setupSession = [&] (juce::ToggleButton& t, juce::Label& status, juce::Label& addr,
                                 const juce::String& title, int port)
        {
            t.setButtonText (title);
            t.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
            t.setBounds (pad, y, W - 2 * pad, 24);
            addAndMakeVisible (t);
            y += 26;
            styleLabel (addr, { pad + 24, y, W - 2 * pad - 24, 20 }, accent, 12.f);
            addr.setText ("In the phone app enter:   " + ips + "   port " + juce::String (port), juce::dontSendNotification);
            y += 20;
            styleLabel (status, { pad + 24, y, W - 2 * pad - 24, 20 }, dim, 11.5f);
            y += 28;
        };

        setupSession (sessionB, statusB, addrB, "Or: phone / tablet over Wi-Fi (Session B, both directions)",
                      cur.networkPort);
        sessionB.onClick = [this] { applySessionB(); };
        sessionB.setToggleState (cur.network, juce::dontSendNotification);

        styleLabel (surfaceInfo, { pad, y - 6, W - 2 * pad, 20 }, dim, 11.5f);
        y += 18;
        monitor.setMultiLine (true);
        monitor.setReadOnly (true);
        monitor.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 12.f, juce::Font::plain));
        monitor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xFF08080D));
        monitor.setColour (juce::TextEditor::textColourId, juce::Colours::lightgreen);
        monitor.setBounds (pad, y, W - 2 * pad, 96);
        addAndMakeVisible (monitor);
        y += 108;

        //── NETWORK ───────────────────────────────────────────────────────────
        sections.push_back ({ y, "NETWORK  -  keyboard / pads over Wi-Fi (RTP-MIDI)" });
        y += 34;
        setupSession (sessionA, statusA, addrA, "Session A  -  plays the instrument", kSessionAPort);
        sessionA.onClick = [this] { applySessionA(); };
        sessionA.setSize (W - 2 * pad - 160, 24);
        advancedBtn.setBounds (W - pad - 150, y - 74, 150, 24);
        advancedBtn.onClick = [this] { if (host.openNetworkAdvanced) host.openNetworkAdvanced(); };
        addAndMakeVisible (advancedBtn);
        sessionA.setToggleState (NetworkMidiSettings::load().enabled && NetworkMidiSettings::load().wifiHost,
                                 juce::dontSendNotification);
        y += 2;

        //── OUTPUTS ───────────────────────────────────────────────────────────
        sections.push_back ({ y, "OUTPUTS" });
        y += 34;
        trackOutBtn.setBounds (pad, y, 210, 26);
        trackOutBtn.onClick = [this] { if (host.openTrackOutputs) host.openTrackOutputs(); };
        addAndMakeVisible (trackOutBtn);
        y += 38;

        setSize (W, y);
        refreshStatus();
    }

    //==========================================================================
    //  Inputs: On / Off, and the surface's cable input
    //==========================================================================
    void applyInputState (const juce::MidiDeviceInfo& d, bool on)
    {
        MidiInputRoles::setOff (d.name, ! on);
        if (on) { if (host.instrumentOn)  host.instrumentOn (d.identifier); }
        else    { if (host.instrumentOff) host.instrumentOff (d.identifier); }
    }

    void applySurfaceIn()
    {
        auto s = host.surface.getSettings();
        s.inName = surfaceInBox.getSelectedId() > 1 ? surfaceInBox.getText() : juce::String();
        if (s.inName.isNotEmpty())
            MidiInputRoles::setOff (s.inName, false);
        s.enabled = s.network || s.inName.isNotEmpty();
        host.surface.applySettings (s);        // takes the port from the instrument / hands the old one back
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MidiHubComponent> (this)]
                                         { if (safe != nullptr) safe->build(); });
    }

    //==========================================================================
    //  Network sessions
    //==========================================================================
    void applySessionA()
    {
        auto v = NetworkMidiSettings::load();
        const bool on = sessionA.getToggleState();
        v.wifiHost = true;
        v.port = kSessionAPort;                            // fixed: never the same as session B
        v.enabled = on;
        NetworkMidiSettings::save (v);
        NetworkMidiSettings::applyFilters (host.network, 1, v);
        if (auto* s = host.network.getSession (1))
        {
            if (on) s->startAsHost (kSessionAPort);
            else    s->stop();
        }
        refreshStatus();
    }

    void applySessionB()
    {
        auto s = host.surface.getSettings();
        s.network = sessionB.getToggleState();
        s.enabled = s.network || s.inName.isNotEmpty();
        host.surface.applySettings (s);
        refreshStatus();
    }

    void applySurfaceOut()
    {
        auto s = host.surface.getSettings();
        s.outName = surfaceOutBox.getSelectedId() > 1 ? surfaceOutBox.getText() : juce::String();
        s.enabled = s.network || s.inName.isNotEmpty() || s.outName.isNotEmpty();
        host.surface.applySettings (s);
    }

    //==========================================================================
    //  Live status
    //==========================================================================
    static juce::String packets (const RtpMidiSession* s)
    {
        if (s == nullptr) return {};
        const int n = s->getPacketsSeen();
        return n > 0 ? "   [" + juce::String (n) + " packets received, last from " + s->getLastSender() + "]"
                     : "   [no packets have reached this PC yet]";
    }

    void refreshStatus()
    {
        const auto* a = host.network.getSession (1);
        statusA.setText (sessionA.getToggleState() && a != nullptr ? a->getStatusText() + packets (a)
                                                                    : juce::String ("Off"),
                         juce::dontSendNotification);
        statusB.setText (host.surface.getNetworkStatus(), juce::dontSendNotification);

        juce::String info = juce::String (host.surface.getSettings().enabled ? "Surface on" : "Surface off")
                          + "   Mode: " + (host.surface.getMode() == MackieControlSurface::Mode::Tracks ? "TRACKS" : "SLICES")
                          + "   Bank starts at " + juce::String (host.surface.getBankStart() + 1)
                          + "   Cable input: " + (host.surface.isInputOpen() ? "open" : "none")
                          + "   Feedback output: " + (host.surface.isOutputOpen() ? "open" : "none");
        surfaceInfo.setText (info, juce::dontSendNotification);

        const auto log = host.surface.getLog().joinIntoString ("\n");
        if (log != lastMonitor)
        {
            lastMonitor = log;
            monitor.setText (log, false);
            monitor.moveCaretToEnd();
        }
    }

    void timerCallback() override
    {
        juce::StringArray ids;
        for (const auto& d : juce::MidiInput::getAvailableDevices()) ids.add (d.identifier);
        if (ids != lastInputIds || host.router.getOutputDeviceNames() != lastOutputNames)
        {
            build();
            return;
        }
        refreshStatus();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiHubComponent)
};
