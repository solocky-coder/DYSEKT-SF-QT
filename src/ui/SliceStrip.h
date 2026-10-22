#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "DysektLookAndFeel.h"

//==============================================================================
//  SliceStrip
//
//  Bench slicer: a row of coloured slice chips directly under the waveform, lined
//  up with the waveform's visible range (same zoom / scroll maths as
//  WaveformOverview). Shows the slice number and a dot for locked slices.
//  Clicking a chip selects that slice and plays it, like a pad.
//==============================================================================
class SliceStrip final : public juce::Component,
                         private juce::Timer
{
public:
    explicit SliceStrip (DysektProcessor& p) : processor (p) { startTimerHz (15); }
    ~SliceStrip() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        const auto& t = getTheme();
        const auto& ui = processor.getUiSliceSnapshot();
        const float sf = (float) getHeight() / 28.0f;

        g.setColour (t.waveformBg);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), t.cornerRadius * 0.6f);

        int visStart = 0, visLen = 0;
        if (! visibleRange (visStart, visLen)) return;

        g.setFont (DysektLookAndFeel::makeFont (10.5f * sf, true));
        const float W = (float) getWidth();
        for (int i = 0; i < ui.numSlices; ++i)
        {
            const auto& s = ui.slices[(size_t) i];
            const int a = s.startSample, b = ui.sliceEndSamples[(size_t) i];
            if (b <= visStart || a >= visStart + visLen) continue;

            const float x0 = juce::jmax (0.0f, (float) (a - visStart) / (float) visLen * W);
            const float x1 = juce::jmin (W, (float) (b - visStart) / (float) visLen * W);
            auto r = juce::Rectangle<float> (x0 + 1.0f, 2.0f, juce::jmax (1.0f, x1 - x0 - 2.0f), (float) getHeight() - 4.0f);

            const bool sel = (i == ui.selectedSlice);
            g.setColour (s.colour.withAlpha (sel ? 0.85f : 0.45f));
            g.fillRoundedRectangle (r, 4.0f * sf);
            if (sel)
            {
                g.setColour (t.foreground.withAlpha (0.9f));
                g.drawRoundedRectangle (r, 4.0f * sf, 1.5f);
            }

            if (r.getWidth() > 18.0f * sf)
            {
                g.setColour (juce::Colours::black.withAlpha (sel ? 0.9f : 0.7f));
                g.drawText (juce::String (i + 1).paddedLeft ('0', 2), r.toNearestInt().reduced (4, 0),
                            juce::Justification::centredLeft, false);
                if (s.lockMask != 0)
                    g.fillEllipse (r.getRight() - 9.0f * sf, r.getCentreY() - 2.5f * sf, 5.0f * sf, 5.0f * sf);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto& ui = processor.getUiSliceSnapshot();
        int visStart = 0, visLen = 0;
        if (! visibleRange (visStart, visLen) || getWidth() <= 0) return;

        const int sample = visStart + (int) ((float) e.x / (float) getWidth() * (float) visLen);
        for (int i = 0; i < ui.numSlices; ++i)
        {
            if (sample >= ui.slices[(size_t) i].startSample && sample < ui.sliceEndSamples[(size_t) i])
            {
                DysektProcessor::Command cmd;
                cmd.type      = DysektProcessor::CmdSelectSlice;
                cmd.intParam1 = i;
                processor.pushCommand (cmd);
                processor.uiNoteOnRequest.store (processor.sliceManager.getSlice (i).midiNote, std::memory_order_relaxed);
                repaint();
                return;
            }
        }
    }

private:
    bool visibleRange (int& start, int& len) const
    {
        auto snap = processor.sampleData.getSnapshot();
        if (! snap || snap->buffer.getNumSamples() == 0) return false;
        const int nf = snap->buffer.getNumSamples();
        const float z = juce::jmax (1.0f, processor.zoom.load());
        const float sc = processor.scroll.load();
        len = juce::jlimit (1, nf, (int) ((float) nf / z));
        const int maxStart = juce::jmax (0, nf - len);
        start = juce::jlimit (0, maxStart, (int) (sc * (float) maxStart));
        return true;
    }

    void timerCallback() override { if (isShowing()) repaint(); }

    DysektProcessor& processor;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SliceStrip)
};
