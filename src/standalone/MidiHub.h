#pragma once
//==============================================================================
//  MidiHub.h  -  ONE window for every kind of MIDI connection
//
//    INPUTS   every cable/USB MIDI input with a Role: Play instrument, Control surface
//             (Mackie) or Off.
//    NETWORK  two network (RTP-MIDI) sessions on fixed, non-clashing ports. Each shows its
//             status, how many packets reached this PC and the exact address to type into
//             the phone / tablet app.
//                Session A  port 5004  keyboard, pads ... (plays the instrument)
//                Session B  port 5006  Mackie control surface
//    OUTPUTS  MIDI thru, the control surface's feedback port, track output overview.
//    MONITOR  what the control surface sends.
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

    inline bool isOff (const juce::String& portName) { return loadOff().contains (portName); }

    inline void setOff (const juce::String& portName, bool off)
    {
        auto names = loadOff();
        if (off) names.addIfNotAlreadyThere (portName); else names.removeString (portName);
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
    juce::Label statusA, statusB, addrA, addrB, thruWarning, surfaceInfo;
    juce::ComboBox thruBox, surfaceOutBox;
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
        sections.push_back ({ y, "INPUTS  -  cable / USB MIDI" });
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
            auto* role = own (new juce::ComboBox());
            styleCombo (*role);
            role->addItem ("Play instrument", 1);
            role->addItem ("Control surface (Mackie)", 2);
            role->addItem ("Off", 3);
            role->setSelectedId (currentRole (d), juce::dontSendNotification);
            role->setBounds (350, y, W - 350 - pad, 24);
            const auto dev = d;
            role->onChange = [this, role, dev] { applyRole (dev, role->getSelectedId()); };
            y += 30;
        }
        y += 6;

        //── NETWORK ───────────────────────────────────────────────────────────
        sections.push_back ({ y, "NETWORK  -  phone / tablet over Wi-Fi (RTP-MIDI)" });
        y += 34;
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

        setupSession (sessionA, statusA, addrA, "Session A  -  keyboard, pads (plays the instrument)", kSessionAPort);
        sessionA.onClick = [this] { applySessionA(); };
        sessionA.setSize (W - 2 * pad - 160, 24);
        advancedBtn.setBounds (W - pad - 150, y - 74, 150, 24);
        advancedBtn.onClick = [this] { if (host.openNetworkAdvanced) host.openNetworkAdvanced(); };
        addAndMakeVisible (advancedBtn);

        setupSession (sessionB, statusB, addrB, "Session B  -  Mackie control surface (faders, pan, transport)",
                      host.surface.getSettings().networkPort);
        sessionB.onClick = [this] { applySessionB(); };

        sessionA.setToggleState (NetworkMidiSettings::load().enabled && NetworkMidiSettings::load().wifiHost,
                                 juce::dontSendNotification);
        sessionB.setToggleState (host.surface.getSettings().network, juce::dontSendNotification);
        y += 2;

        //── OUTPUTS ───────────────────────────────────────────────────────────
        sections.push_back ({ y, "OUTPUTS" });
        y += 34;
        const auto ports = host.router.getOutputDeviceNames();
        lastOutputNames = ports;

        label ("Forward live MIDI input to (thru)", { pad, y, 250, 24 }, text);
        thruBox.clear (juce::dontSendNotification);
        thruBox.addItem ("(Off)", 1);
        for (int i = 0; i < ports.size(); ++i) thruBox.addItem (ports[i], i + 2);
        const int ti = ports.indexOf (host.router.getThruOutput());
        thruBox.setSelectedId (ti >= 0 ? ti + 2 : 1, juce::dontSendNotification);
        styleCombo (thruBox);
        thruBox.setBounds (270, y, W - 270 - pad, 24);
        thruBox.onChange = [this] { applyThru(); };
        addAndMakeVisible (thruBox);
        y += 28;
        styleLabel (thruWarning, { pad, y, W - 2 * pad, 18 }, red, 11.f);
        thruWarning.setText ({}, juce::dontSendNotification);
        y += 22;

        label ("Control surface feedback (motors, lights)", { pad, y, 250, 24 }, text);
        surfaceOutBox.clear (juce::dontSendNotification);
        surfaceOutBox.addItem ("(None)", 1);
        for (int i = 0; i < ports.size(); ++i) surfaceOutBox.addItem (ports[i], i + 2);
        const int si = ports.indexOf (host.surface.getSettings().outName);
        surfaceOutBox.setSelectedId (si >= 0 ? si + 2 : 1, juce::dontSendNotification);
        styleCombo (surfaceOutBox);
        surfaceOutBox.setBounds (270, y, W - 270 - pad, 24);
        surfaceOutBox.onChange = [this] { applySurfaceOut(); };
        addAndMakeVisible (surfaceOutBox);
        y += 32;

        trackOutBtn.setBounds (pad, y, 210, 26);
        trackOutBtn.onClick = [this] { if (host.openTrackOutputs) host.openTrackOutputs(); };
        addAndMakeVisible (trackOutBtn);
        y += 38;

        //── MONITOR ───────────────────────────────────────────────────────────
        sections.push_back ({ y, "MONITOR  -  control surface messages" });
        y += 32;
        styleLabel (surfaceInfo, { pad, y, W - 2 * pad, 20 }, dim, 11.5f);
        y += 22;
        monitor.setMultiLine (true);
        monitor.setReadOnly (true);
        monitor.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 12.f, juce::Font::plain));
        monitor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xFF08080D));
        monitor.setColour (juce::TextEditor::textColourId, juce::Colours::lightgreen);
        monitor.setBounds (pad, y, W - 2 * pad, 150);
        addAndMakeVisible (monitor);
        y += 160;

        setSize (W, y);
        refreshStatus();
    }

    //==========================================================================
    //  Inputs: roles
    //==========================================================================
    int currentRole (const juce::MidiDeviceInfo& d) const
    {
        if (host.surface.claimsInput (d))               return 2;
        if (host.isInstrument && host.isInstrument (d.identifier)) return 1;
        return 3;
    }

    void applyRole (const juce::MidiDeviceInfo& d, int role)
    {
        auto s = host.surface.getSettings();
        const bool wasSurface = host.surface.claimsInput (d);

        if (role == 2)                                     // control surface
        {
            MidiInputRoles::setOff (d.name, false);
            s.inName = d.name;
            s.enabled = true;
            host.surface.applySettings (s);                // takes the port from the instrument
        }
        else
        {
            if (wasSurface)
            {
                s.inName.clear();
                s.enabled = s.network;
                host.surface.applySettings (s);            // hands the port back to the instrument
            }
            if (role == 1)
            {
                MidiInputRoles::setOff (d.name, false);
                if (host.instrumentOn) host.instrumentOn (d.identifier);
            }
            else
            {
                MidiInputRoles::setOff (d.name, true);
                if (host.instrumentOff) host.instrumentOff (d.identifier);
            }
        }
        // Only one input can be the surface: refresh the other rows.
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
        host.surface.applySettings (s);
    }

    void applyThru()
    {
        const auto name = thruBox.getSelectedId() <= 1 ? juce::String() : thruBox.getText();
        bool loops = false;
        for (const auto& in : juce::MidiInput::getAvailableDevices())
            if (name.isNotEmpty() && in.name == name) loops = true;
        if (loops)
        {
            thruBox.setSelectedId (1, juce::dontSendNotification);
            host.router.setThruOutput ({});
            thruWarning.setText ("This port is also a MIDI input. Forwarding to it would loop, so Thru stays off.",
                                 juce::dontSendNotification);
            return;
        }
        thruWarning.setText ({}, juce::dontSendNotification);
        host.router.setThruOutput (name);
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
