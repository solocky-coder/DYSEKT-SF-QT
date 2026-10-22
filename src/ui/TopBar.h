#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../PluginProcessor.h"
#include "../params/ParamIds.h"
#include "../sequencer/SequencerEngine.h"
#include "DysektLookAndFeel.h"

//==============================================================================
//  TopBar
//
//  Slim top bar for the Bench layout. It paints the bar, shows the current
//  mode and tempo, and hosts the two global sliders (pitch, volume), which are
//  bound straight to the same parameters the old control-frame knobs drive
//  (ParamIds::defaultPitch / ParamIds::masterVolume).
//
//  The transport (MiniTransportBar) and the UNDO / REDO / PANIC / settings row
//  (HeaderBar) are existing components that the editor places into the slots
//  computed by computeSlots(), so their behaviour is untouched.
//  Only shown while the Bench theme is active.
//==============================================================================
class TopBar final : public juce::Component,
                     private juce::Timer
{
public:
    /** Design height in layout units. */
    static constexpr int kDesignHeight = 44;

    struct Slots
    {
        juce::Rectangle<int> title, transport, bpm, pitch, volume, buttons;
        float scale = 1.0f;
    };

    TopBar (DysektProcessor& p, SequencerEngine& seq)
        : processor (p), engine (seq)
    {
        for (auto* s : { &pitchSlider, &volumeSlider })
        {
            s->setSliderStyle (juce::Slider::LinearHorizontal);
            s->setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            s->setWantsKeyboardFocus (false);
            s->onValueChange = [this] { repaint(); };
            addAndMakeVisible (*s);
        }

        pitchAttach  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                           processor.apvts, ParamIds::defaultPitch, pitchSlider);
        volumeAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                           processor.apvts, ParamIds::masterVolume, volumeSlider);

        startTimerHz (10);
        timerCallback();
    }

    ~TopBar() override { stopTimer(); }

    void setModeTitle (const juce::String& t)
    {
        if (t != modeTitle) { modeTitle = t; repaint(); }
    }

    /** Where each piece sits, in the coordinates of `bounds` (the bar's bounds in its parent). */
    static Slots computeSlots (juce::Rectangle<int> bounds, float sf)
    {
        // Never wider than the bar allows: shrink everything if the window is narrow.
        const float s = juce::jmin (sf, (float) bounds.getWidth() / 860.0f);
        auto u = [s] (float v) { return juce::roundToInt (v * s); };

        Slots out;
        out.scale = s;
        auto row = bounds.reduced (u (12.0f), 0);
        const int h = bounds.getHeight();
        auto centred = [&] (juce::Rectangle<int> r, int itemH)
        {
            return r.withSizeKeepingCentre (r.getWidth(), juce::jmin (itemH, h - 4));
        };

        out.buttons   = centred (row.removeFromRight (u (232.0f)), u (30.0f));
        row.removeFromRight (u (14.0f));
        out.volume    = centred (row.removeFromRight (u (112.0f)), u (34.0f));
        row.removeFromRight (u (10.0f));
        out.pitch     = centred (row.removeFromRight (u (112.0f)), u (34.0f));
        row.removeFromRight (u (14.0f));
        out.title     = row.removeFromLeft (u (150.0f));
        row.removeFromLeft (u (8.0f));
        out.transport = centred (row.removeFromLeft (u (190.0f)), u (36.0f));
        row.removeFromLeft (u (10.0f));
        out.bpm       = centred (row.removeFromLeft (u (88.0f)), u (34.0f));
        return out;
    }

    void resized() override
    {
        const auto sl = computeSlots (getLocalBounds(), scaleForHeight());
        slots = sl;
        pitchSlider .setBounds (sl.pitch .withTrimmedTop (juce::roundToInt (sl.pitch .getHeight() * 0.45f)));
        volumeSlider.setBounds (sl.volume.withTrimmedTop (juce::roundToInt (sl.volume.getHeight() * 0.45f)));
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = getTheme();
        const float s = slots.scale;

        g.setColour (t.waveformBg);
        g.fillRect (getLocalBounds());
        g.setColour (t.separator);
        g.fillRect (0, getHeight() - 1, getWidth(), 1);

        // Title + mode
        g.setColour (t.foreground);
        g.setFont (juce::Font (juce::FontOptions (15.0f * s, juce::Font::bold)));
        juce::String titleText = "DYSEKT-SF";
        if (auto snap = processor.sampleData.getSnapshot())
            if (! snap->filePath.containsIgnoreCase ("DYSEKT_default.wav") && snap->filePath.isNotEmpty())
                titleText = juce::File (snap->filePath).getFileNameWithoutExtension();
        g.drawText (titleText, slots.title.withHeight (slots.title.getHeight() / 2 + juce::roundToInt (4 * s)),
                    juce::Justification::bottomLeft, false);
        g.setColour (t.foreground.withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (10.5f * s)));
        g.drawText (modeTitle, slots.title.withTrimmedTop (slots.title.getHeight() / 2 + juce::roundToInt (4 * s)),
                    juce::Justification::topLeft, false);

        // Tempo readout
        g.setColour (t.foreground.withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (9.5f * s)));
        g.drawText ("TEMPO", slots.bpm.withHeight (slots.bpm.getHeight() / 2), juce::Justification::bottomLeft, false);
        g.setColour (t.foreground);
        g.setFont (juce::Font (juce::FontOptions (14.0f * s)));
        g.drawText (bpmText, slots.bpm.withTrimmedTop (slots.bpm.getHeight() / 2), juce::Justification::topLeft, false);

        // Slider captions
        auto caption = [&] (const juce::Rectangle<int>& r, const juce::String& label, const juce::String& value)
        {
            auto top = r.withHeight (juce::roundToInt (r.getHeight() * 0.45f));
            g.setColour (t.foreground.withAlpha (0.55f));
            g.setFont (juce::Font (juce::FontOptions (9.5f * s)));
            g.drawText (label, top, juce::Justification::bottomLeft, false);
            g.setColour (t.foreground);
            g.setFont (juce::Font (juce::FontOptions (10.5f * s)));
            g.drawText (value, top, juce::Justification::bottomRight, false);
        };
        const double pv = pitchSlider.getValue(), vv = volumeSlider.getValue();
        caption (slots.pitch,  "PITCH", (pv >= 0.0 ? "+" : "") + juce::String ((int) std::round (pv)) + " st");
        caption (slots.volume, "VOL",   (vv >= 0.0 ? "+" : "") + juce::String (vv, 1) + " dB");
    }

private:
    float scaleForHeight() const noexcept { return (float) getHeight() / (float) kDesignHeight; }

    void timerCallback() override
    {
        const auto txt = juce::String (engine.getBpm(), 1);
        if (txt != bpmText) { bpmText = txt; repaint(); }
    }

    DysektProcessor& processor;
    SequencerEngine& engine;

    juce::Slider pitchSlider, volumeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchAttach, volumeAttach;

    juce::String modeTitle { "Slicer" };
    juce::String bpmText   { "120.0" };
    Slots slots;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TopBar)
};
