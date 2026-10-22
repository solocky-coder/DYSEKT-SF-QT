#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "DysektLookAndFeel.h"
#include "WaveformView.h"

//==============================================================================
//  SliceHeaderCard
//
//  Bench slicer header above the waveform: file name, a sub-line, info chips and
//  the slicing controls that used to live in the waveform's right-click menu:
//  Auto slice into 4/8/16/32 (CmdEqualChop), MIDI slice (lazy chop) and the
//  waveform drawing style. It sends exactly the commands the menu does.
//==============================================================================
class SliceHeaderCard final : public juce::Component,
                              private juce::Timer
{
public:
    static constexpr int kDesignHeight = 56;

    SliceHeaderCard (DysektProcessor& p, WaveformView& w) : processor (p), waveform (w) { startTimerHz (8); }
    ~SliceHeaderCard() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        const auto& t  = getTheme();
        const float sf = (float) getHeight() / (float) kDesignHeight;
        hits.clear();

        const auto bf = getLocalBounds().toFloat();
        g.setColour (t.waveformBg);
        g.fillRoundedRectangle (bf, t.cornerRadius);
        g.setColour (t.separator);
        g.drawRoundedRectangle (bf.reduced (0.5f), t.cornerRadius, 1.0f);

        auto snap = processor.sampleData.getSnapshot();
        const double sr = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0;
        const juce::String name = snap ? juce::File (snap->filePath).getFileNameWithoutExtension() : juce::String();
        const double seconds = snap ? (double) snap->buffer.getNumSamples() / sr : 0.0;
        const int numSlices = processor.getUiSliceSnapshot().numSlices;

        // ── Right side: controls, laid out from the right edge ──────────────
        const int pad = juce::roundToInt (12.0f * sf);
        const int segH = juce::roundToInt (28.0f * sf);
        const int segY = (getHeight() - segH) / 2;
        int right = getWidth() - pad;

        right = segmented (g, right, segY, segH, sf, { { "Hard", 100 }, { "Soft", 101 }, { "Outline", 102 }, { "Bars", 105 }, { "RMS", 106 } },
                           waveform.getWaveformMode() + 100);
        right -= juce::roundToInt (8.0f * sf);
        right = segmented (g, right, segY, segH, sf, { { "Auto 4", 4 }, { "8", 8 }, { "16", 16 }, { "32", 32 }, { "MIDI", 50 } },
                           waveform.isMidiSliceActive() ? 50 : -1);

        // ── Left side: name, sub-line, chips ────────────────────────────────
        const int leftLimit = right - juce::roundToInt (12.0f * sf);
        auto left = juce::Rectangle<int> (pad, 0, juce::jmax (0, leftLimit - pad), getHeight());

        const auto nameFont = DysektLookAndFeel::makeFont (15.5f * sf, true);
        const juce::String title = name.isNotEmpty() ? name : "No sample";
        g.setColour (t.foreground);
        g.setFont (nameFont);
        g.drawText (title, left.withTrimmedTop (juce::roundToInt (7.0f * sf)).withHeight (juce::roundToInt (22.0f * sf)),
                    juce::Justification::centredLeft, true);

        g.setColour (t.foreground.withAlpha (0.55f));
        g.setFont (DysektLookAndFeel::makeFont (11.0f * sf));
        g.drawText ("Slicer  ·  " + juce::String (numSlices) + (numSlices == 1 ? " slice" : " slices")
                        + "  ·  " + juce::String (sr / 1000.0, 1) + " kHz",
                    left.withTrimmedTop (juce::roundToInt (30.0f * sf)).withHeight (juce::roundToInt (16.0f * sf)),
                    juce::Justification::centredLeft, true);
        juce::ignoreUnused (seconds);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (const auto& h : hits)
            if (h.r.contains (e.getPosition())) { activate (h.id); repaint(); return; }
    }

private:
    struct Seg { const char* text; int id; };
    struct Hit { juce::Rectangle<int> r; int id; };

    // Draws one segmented control right-aligned to `right`; returns its left edge.
    int segmented (juce::Graphics& g, int right, int y, int h, float sf, std::initializer_list<Seg> items, int selectedId)
    {
        const auto& t = getTheme();
        const auto font = DysektLookAndFeel::makeFont (12.0f * sf, true);
        const int padX = juce::roundToInt (11.0f * sf);
        const int inset = juce::roundToInt (3.0f * sf);

        int total = inset * 2;
        std::vector<int> widths;
        for (auto& it : items) { widths.push_back (juce::GlyphArrangement::getStringWidthInt (font, it.text) + padX * 2); total += widths.back(); }

        const int left = right - total;
        g.setColour (t.button);
        g.fillRoundedRectangle (juce::Rectangle<int> (left, y, total, h).toFloat(), t.cornerRadius);

        int x = left + inset, i = 0;
        g.setFont (font);
        for (auto& it : items)
        {
            const juce::Rectangle<int> r (x, y + inset, widths[(size_t) i], h - inset * 2);
            const bool sel = (it.id == selectedId);
            if (sel)
            {
                g.setColour (t.accent);
                g.fillRoundedRectangle (r.toFloat(), juce::jmax (2.0f, t.cornerRadius - 3.0f));
            }
            g.setColour (sel ? t.waveformBg : t.foreground.withAlpha (0.7f));
            g.drawText (it.text, r, juce::Justification::centred, false);
            hits.push_back ({ r, it.id });
            x += widths[(size_t) i++];
        }
        return left;
    }

    void activate (int id)
    {
        if (id >= 100) { waveform.setWaveformMode (id - 100); return; }

        if (id == 50)   // MIDI slice (same flow as the waveform's context menu)
        {
            DysektProcessor::Command cmd;
            if (waveform.isMidiSliceActive())
            {
                cmd.type = DysektProcessor::CmdLazyChopStop;
                processor.pushCommand (cmd);
                waveform.setMidiSliceActive (false);
            }
            else
            {
                cmd.type = DysektProcessor::CmdLazyChopStart;
                processor.pushCommand (cmd);
                processor.midiSelectsSlice.store (true);
                waveform.setMidiSliceActive (true);
            }
            return;
        }

        DysektProcessor::Command cmd;
        cmd.type      = DysektProcessor::CmdEqualChop;
        cmd.intParam1 = id;
        processor.pushCommand (cmd);
    }

    void timerCallback() override { if (isShowing()) repaint(); }

    DysektProcessor& processor;
    WaveformView&    waveform;
    std::vector<Hit> hits;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SliceHeaderCard)
};
