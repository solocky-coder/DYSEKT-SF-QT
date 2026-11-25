#pragma once
//==============================================================================
//  MidiHub.h  -  ONE simple window for every MIDI connection
//
//    DEVICES  every cable/USB MIDI input: Keyboard, Control surface (Mackie) or Off.
//             A control surface gets its matching output port automatically.
//    WI-FI    one phone / tablet connection (RTP-MIDI, port 5004), used either as a
//             keyboard & pads or as a Mackie control surface - one at a time.
//    A log for the control surface is available behind "Show log".
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
};

class MidiHubComponent final : public juce::Component,
                               private juce::Timer
{
public:
    static constexpr int kWifiPort = 5004;

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
        if (dotY >= 0)
        {
            g.setColour (dotColour);
            g.fillEllipse (12.0f, (float) dotY, 10.0f, 10.0f);
        }
    }

private:
    const juce::Colour bg     { 0xFF0D0D14 };
    const juce::Colour header { 0xFF1A2030 };
    const juce::Colour accent { 0xFF00CCAA };
    const juce::Colour text   { 0xFFCCD0D8 };
    const juce::Colour dim    { 0xFF8A94A6 };

    struct Section { int y; juce::String title; };
    std::vector<Section> sections;
    juce::OwnedArray<juce::Component> items;
    MidiHubHost host;
    juce::StringArray lastInputIds, lastOutputNames;
    bool showLog = false;
    int dotY = -1;
    juce::Colour dotColour { 0xFF8A94A6 };

    juce::ToggleButton wifiOn;
    juce::ComboBox wifiRole;
    juce::Label wifiAddr, wifiStatus;
    juce::TextButton advancedBtn { "Advanced network options..." }, logBtn { "Show log" };
    juce::TextEditor monitor;
    juce::String lastMonitor;

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

    /** "Axiom AIR 25 MIDI In" and "Axiom AIR 25 MIDI Out" are the same device. */
    static juce::String deviceKey (const juce::String& name)
    {
        auto s = MackieControlSurface::portKey (name).toLowerCase();
        for (const char* tok : { " midi in", " midi out", " in", " out" })
        {
            const juce::String t (tok);
            if (s.endsWith (t)) { s = s.dropLastCharacters (t.length()); break; }
        }
        return s.trim();
    }

    static juce::String guessOutput (const juce::String& inName, const juce::StringArray& outs)
    {
        const auto k = deviceKey (inName);
        for (const auto& o : outs)
            if (deviceKey (o) == k) return o;
        return {};
    }

    //==========================================================================
    void build()
    {
        removeAllChildren();
        items.clear();
        sections.clear();
        dotY = -1;

        const int W = 600, pad = 12;
        int y = 0;

        //── DEVICES ───────────────────────────────────────────────────────────
        sections.push_back ({ y, "DEVICES  -  cable / USB MIDI" });
        y += 34;
        const auto inputs = juce::MidiInput::getAvailableDevices();
        const auto outPorts = host.router.getOutputDeviceNames();
        lastOutputNames = outPorts;
        lastInputIds.clear();
        for (const auto& d : inputs) lastInputIds.add (d.identifier);

        if (inputs.isEmpty())
        {
            label ("No MIDI input devices found.", { pad, y, W - 2 * pad, 22 }, dim);
            y += 28;
        }

        const auto cur = host.surface.getSettings();
        for (const auto& d : inputs)
        {
            label (d.name, { pad, y, 340, 24 }, text, 13.f);
            auto* choice = own (new juce::ComboBox());
            styleCombo (*choice);
            choice->addItem ("Keyboard", 1);
            choice->addItem ("Control surface (Mackie)", 2);
            choice->addItem ("Off", 3);
            const bool isSurface = host.surface.claimsInput (d);
            choice->setSelectedId (isSurface ? 2 : (host.isInstrument && host.isInstrument (d.identifier) ? 1 : 3),
                                   juce::dontSendNotification);
            choice->setBounds (360, y, W - 360 - pad, 24);
            const auto dev = d;
            choice->onChange = [this, choice, dev] { applyDevice (dev, choice->getSelectedId()); };
            y += 30;

            if (isSurface)
            {
                label ("Feedback output (motor faders, lights)", { pad + 20, y, 300, 22 }, dim, 11.5f);
                auto* out = own (new juce::ComboBox());
                styleCombo (*out);
                out->addItem ("(none)", 1);
                int sel = 1;
                for (int i = 0; i < outPorts.size(); ++i)
                {
                    out->addItem (outPorts[i], i + 2);
                    if (MackieControlSurface::portKey (outPorts[i]) == MackieControlSurface::portKey (cur.outName))
                        sel = i + 2;
                }
                out->setSelectedId (sel, juce::dontSendNotification);
                out->setBounds (360, y, W - 360 - pad, 22);
                out->onChange = [this, out] { applySurfaceOut (out->getSelectedId() > 1 ? out->getText() : juce::String()); };
                y += 28;
            }
        }
        y += 8;

        //── WI-FI ─────────────────────────────────────────────────────────────
        sections.push_back ({ y, "WI-FI  -  phone / tablet" });
        y += 34;

        wifiOn.setButtonText ("Connect a phone / tablet over Wi-Fi");
        wifiOn.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
        wifiOn.setBounds (pad, y, 300, 24);
        wifiOn.onClick = [this] { applyWifi(); };
        addAndMakeVisible (wifiOn);

        wifiRole.clear (juce::dontSendNotification);
        wifiRole.addItem ("Use as: Keyboard and pads", 1);
        wifiRole.addItem ("Use as: Mackie control surface", 2);
        styleCombo (wifiRole);
        wifiRole.setBounds (320, y, W - 320 - pad, 24);
        wifiRole.onChange = [this] { applyWifi(); };
        addAndMakeVisible (wifiRole);
        y += 30;

        const bool mackie = host.surface.getSettings().network;
        const bool keyboard = NetworkMidiSettings::load().enabled && NetworkMidiSettings::load().wifiHost;
        wifiOn.setToggleState (mackie || keyboard, juce::dontSendNotification);
        wifiRole.setSelectedId (mackie ? 2 : 1, juce::dontSendNotification);

        styleLabel (wifiAddr, { pad + 20, y, W - 2 * pad - 20, 20 }, accent, 12.f);
        wifiAddr.setText ("In the phone app enter:   " + thisPcAddresses() + "   port " + juce::String (kWifiPort),
                          juce::dontSendNotification);
        y += 22;
        dotY = y + 4;
        styleLabel (wifiStatus, { pad + 20, y, W - 2 * pad - 20, 36 }, dim, 11.5f);
        wifiStatus.setMinimumHorizontalScale (1.0f);
        y += 44;

        advancedBtn.setBounds (pad, y, 210, 26);
        advancedBtn.onClick = [this] { if (host.openNetworkAdvanced) host.openNetworkAdvanced(); };
        addAndMakeVisible (advancedBtn);
        logBtn.setButtonText (showLog ? "Hide log" : "Show log");
        logBtn.setBounds (W - pad - 110, y, 110, 26);
        logBtn.onClick = [this] { showLog = ! showLog; juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MidiHubComponent> (this)] { if (safe) safe->build(); }); };
        addAndMakeVisible (logBtn);
        y += 36;

        if (showLog)
        {
            sections.push_back ({ y, "LOG  -  control surface messages" });
            y += 32;
            monitor.setMultiLine (true);
            monitor.setReadOnly (true);
            monitor.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 12.f, juce::Font::plain));
            monitor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xFF08080D));
            monitor.setColour (juce::TextEditor::textColourId, juce::Colours::lightgreen);
            monitor.setBounds (pad, y, W - 2 * pad, 140);
            addAndMakeVisible (monitor);
            y += 150;
        }

        setSize (W, y);
        lastMonitor = "\u0001";   // force a refresh
        refreshStatus();
    }

    //==========================================================================
    //  Cable / USB devices
    //==========================================================================
    void applyDevice (const juce::MidiDeviceInfo& d, int choice)
    {
        auto s = host.surface.getSettings();
        const bool wasSurface = host.surface.claimsInput (d);

        if (choice == 2)                                           // control surface
        {
            MidiInputRoles::setOff (d.name, false);
            s.inName = d.name;
            s.outName = guessOutput (d.name, host.router.getOutputDeviceNames());
            s.enabled = true;
            host.surface.applySettings (s);
        }
        else
        {
            if (wasSurface)
            {
                s.inName.clear();
                s.outName.clear();
                s.enabled = s.network;
                host.surface.applySettings (s);                    // hands the port back to the instrument
            }
            MidiInputRoles::setOff (d.name, choice == 3);
            if (choice == 1) { if (host.instrumentOn)  host.instrumentOn (d.identifier); }
            else             { if (host.instrumentOff) host.instrumentOff (d.identifier); }
        }
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MidiHubComponent> (this)]
                                         { if (safe != nullptr) safe->build(); });
    }

    void applySurfaceOut (const juce::String& outName)
    {
        auto s = host.surface.getSettings();
        s.outName = outName;
        host.surface.applySettings (s);
    }

    //==========================================================================
    //  Wi-Fi: one connection, one role at a time
    //==========================================================================
    void applyWifi()
    {
        const bool on = wifiOn.getToggleState();
        const bool mackie = wifiRole.getSelectedId() == 2;

        // Keyboard session (manager device 1) on port 5004
        auto v = NetworkMidiSettings::load();
        v.wifiHost = true;
        v.port = kWifiPort;
        v.enabled = on && ! mackie;
        NetworkMidiSettings::save (v);
        NetworkMidiSettings::applyFilters (host.network, 1, v);
        if (auto* sess = host.network.getSession (1))
        {
            if (v.enabled) sess->startAsHost (kWifiPort);
            else           sess->stop();                           // frees 5004 before the surface takes it
        }

        // Mackie surface session on the same port
        auto s = host.surface.getSettings();
        s.network = on && mackie;
        s.networkPort = kWifiPort;
        s.enabled = s.network || s.inName.isNotEmpty();
        host.surface.applySettings (s);

        refreshStatus();
    }

    //==========================================================================
    //  Live status
    //==========================================================================
    void refreshStatus()
    {
        const bool on = wifiOn.getToggleState();
        const bool mackie = wifiRole.getSelectedId() == 2;
        juce::String msg;
        bool connected = false;

        if (! on)
        {
            msg = "Off.";
        }
        else if (mackie)
        {
            connected = host.surface.isNetworkConnected();
            msg = connected ? "Connected." : unconnectedHelp (host.surface.getNetworkPackets());
        }
        else
        {
            const auto* sess = host.network.getSession (1);
            connected = sess != nullptr && sess->getState() == RtpMidiSession::State::Connected;
            msg = connected ? "Connected." : unconnectedHelp (sess != nullptr ? sess->getPacketsSeen() : 0);
        }

        wifiStatus.setText (msg, juce::dontSendNotification);
        dotColour = ! on ? dim : (connected ? juce::Colour (0xFF55D87A) : juce::Colour (0xFFE0B030));
        repaint();

        if (showLog)
        {
            const auto log = host.surface.getLog().joinIntoString ("\n");
            if (log != lastMonitor)
            {
                lastMonitor = log;
                monitor.setText (log, false);
                monitor.moveCaretToEnd();
            }
        }
    }

    static juce::String unconnectedHelp (int packets)
    {
        return packets > 0
            ? "Waiting for the phone to finish connecting. Try switching the connection in the phone app off and on."
            : "Waiting for the phone. Nothing has reached this PC yet: check the address and port above in the phone app, "
              "that both are on the same Wi-Fi, and that Windows Firewall allows XIVA.";
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
