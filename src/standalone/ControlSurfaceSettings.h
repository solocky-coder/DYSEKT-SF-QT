#pragma once
//==============================================================================
//  ControlSurfaceSettings.h  -  "Control Surface..." dialog (Mackie Control)
//  Enable switch, MIDI in / out port pickers, and a live log of what the surface sends.
//==============================================================================
#include <juce_gui_basics/juce_gui_basics.h>
#include "MackieControlSurface.h"

class ControlSurfaceSettingsComponent : public juce::Component,
                                        private juce::Timer
{
public:
    explicit ControlSurfaceSettingsComponent (MackieControlSurface& s) : surface (s)
    {
        setSize (560, 440);

        title.setText ("Control Surface (Mackie Control)", juce::dontSendNotification);
        title.setFont (juce::Font (16.0f, juce::Font::bold));
        title.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (title);

        help.setText ("Works with any Mackie Control compatible surface. Pick its MIDI input and output "
                      "(output = motor faders and lights). Track / Instrument buttons switch between "
                      "arranger tracks and slices; fader 9 is the master.",
                      juce::dontSendNotification);
        help.setJustificationType (juce::Justification::topLeft);
        help.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible (help);

        enable.setButtonText ("Enable control surface");
        enable.setColour (juce::ToggleButton::textColourId, juce::Colours::white);
        addAndMakeVisible (enable);

        for (auto* l : { &inLabel, &outLabel })
        {
            l->setColour (juce::Label::textColourId, juce::Colours::white);
            addAndMakeVisible (l);
        }
        inLabel.setText ("MIDI input", juce::dontSendNotification);
        outLabel.setText ("MIDI output", juce::dontSendNotification);
        addAndMakeVisible (inBox);
        addAndMakeVisible (outBox);

        status.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible (status);

        log.setMultiLine (true);
        log.setReadOnly (true);
        log.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
        log.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xFF08080D));
        log.setColour (juce::TextEditor::textColourId, juce::Colours::lightgreen);
        addAndMakeVisible (log);

        const auto cur = surface.getSettings();
        enable.setToggleState (cur.enabled, juce::dontSendNotification);

        inBox.addItem ("(none)", 1);
        int id = 2;
        for (const auto& d : juce::MidiInput::getAvailableDevices())
        {
            inBox.addItem (d.name, id);
            if (d.name == cur.inName) inBox.setSelectedId (id, juce::dontSendNotification);
            ++id;
        }
        if (inBox.getSelectedId() == 0) inBox.setSelectedId (1, juce::dontSendNotification);

        outBox.addItem ("(none)", 1);
        id = 2;
        for (const auto& d : juce::MidiOutput::getAvailableDevices())
        {
            outBox.addItem (d.name, id);
            if (d.name == cur.outName) outBox.setSelectedId (id, juce::dontSendNotification);
            ++id;
        }
        if (outBox.getSelectedId() == 0) outBox.setSelectedId (1, juce::dontSendNotification);

        auto apply = [this]
        {
            MackieControlSurface::Settings s;
            s.enabled = enable.getToggleState();
            s.inName  = inBox.getSelectedId()  > 1 ? inBox.getText()  : juce::String();
            s.outName = outBox.getSelectedId() > 1 ? outBox.getText() : juce::String();
            surface.applySettings (s);
        };
        enable.onClick = apply;
        inBox.onChange = apply;
        outBox.onChange = apply;

        startTimerHz (5);
        timerCallback();
    }

    ~ControlSurfaceSettingsComponent() override { stopTimer(); }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xFF0D0D14)); }

    void resized() override
    {
        auto a = getLocalBounds().reduced (16);
        title.setBounds (a.removeFromTop (28));
        help.setBounds (a.removeFromTop (62));
        enable.setBounds (a.removeFromTop (28));
        a.removeFromTop (6);
        auto r1 = a.removeFromTop (28); inLabel.setBounds (r1.removeFromLeft (100));  inBox.setBounds (r1);
        a.removeFromTop (6);
        auto r2 = a.removeFromTop (28); outLabel.setBounds (r2.removeFromLeft (100)); outBox.setBounds (r2);
        a.removeFromTop (8);
        status.setBounds (a.removeFromTop (22));
        log.setBounds (a);
    }

private:
    void timerCallback() override
    {
        juce::String s = surface.getSettings().enabled ? "Input: " : "Disabled.  Input: ";
        s += surface.isInputOpen() ? "open" : "closed";
        s += "   Output: ";
        s += surface.isOutputOpen() ? "open" : "closed";
        s += juce::String ("   Mode: ") + (surface.getMode() == MackieControlSurface::Mode::Tracks ? "TRACKS" : "SLICES")
             + "   Bank starts at " + juce::String (surface.getBankStart() + 1);
        status.setText (s, juce::dontSendNotification);

        const auto text = surface.getLog().joinIntoString ("\n");
        if (text != lastLog)
        {
            lastLog = text;
            log.setText (text, false);
            log.moveCaretToEnd();
        }
    }

    MackieControlSurface& surface;
    juce::Label title, help, inLabel, outLabel, status;
    juce::ToggleButton enable;
    juce::ComboBox inBox, outBox;
    juce::TextEditor log;
    juce::String lastLog;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlSurfaceSettingsComponent)
};
