#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "DysektLookAndFeel.h"

//==============================================================================
//  SliceFlagRow
//
//  Pill row for the Bench slicer inspector: REV / LOOP (cycles Off, Loop, Ping) /
//  1SH / MONO-POLY / STR / TAIL for the selected slice. These are the same
//  switches the old LCD flag strip offered and they send the same
//  CmdSetSliceParam commands, including the loop / one-shot exclusion.
//==============================================================================
class SliceFlagRow final : public juce::Component,
                           private juce::Timer
{
public:
    explicit SliceFlagRow (DysektProcessor& p) : processor (p) { startTimerHz (10); }
    ~SliceFlagRow() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        const auto& t = getTheme();
        const auto s = current();
        const float sf = (float) getHeight() / 24.0f;

        struct Pill { juce::String text; bool on; };
        const Pill pills[kNum] = {
            { "REV",  s.reverse },
            { s.loopMode == 2 ? "PING" : "LOOP", s.loopMode > 0 },
            { "1SH",  s.oneShot },
            { mono() ? "MONO" : "POLY", mono() },
            { "STR",  s.stretchEnabled },
            { "TAIL", s.releaseTail },
        };

        g.setFont (DysektLookAndFeel::makeFont (10.5f * sf, true));
        for (int i = 0; i < kNum; ++i)
        {
            const auto r = pillRect (i).toFloat();
            g.setColour (pills[i].on ? t.accent : t.button);
            g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            g.setColour (pills[i].on ? t.waveformBg : t.foreground.withAlpha (0.6f));
            g.drawText (pills[i].text, r.toNearestInt(), juce::Justification::centred, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < kNum; ++i)
            if (pillRect (i).contains (e.getPosition())) { toggle (i); repaint(); return; }
    }

private:
    static constexpr int kNum = 6;
    using F = DysektProcessor;

    juce::Rectangle<int> pillRect (int i) const
    {
        const int gap = juce::jmax (2, getHeight() / 6);
        const int w = (getWidth() - gap * (kNum - 1)) / kNum;
        return { i * (w + gap), 0, w, getHeight() };
    }

    Slice current() const
    {
        const auto& ui = processor.getUiSliceSnapshot();
        const int idx = ui.selectedSlice;
        if (idx >= 0 && idx < ui.numSlices && processor.sliceManager.getNumSlices() > idx)
            return processor.sliceManager.getSlice (idx);
        return {};
    }

    bool mono() const
    {
        auto* p = processor.apvts.getRawParameterValue (ParamIds::globalMono);
        return p != nullptr && p->load() > 0.5f;
    }

    void send (int field, float value)
    {
        DysektProcessor::Command cmd;
        cmd.type        = F::CmdSetSliceParam;
        cmd.intParam1   = field;
        cmd.floatParam1 = value;
        processor.pushCommand (cmd);
    }

    void toggle (int i)
    {
        const auto s = current();
        switch (i)
        {
            case 0: send (F::FieldReverse, s.reverse ? 0.0f : 1.0f); break;
            case 1:
            {
                const int next = (s.loopMode + 1) % 3;
                send (F::FieldLoop, (float) next);
                if (next > 0 && s.oneShot) send (F::FieldOneShot, 0.0f);
                break;
            }
            case 2:
            {
                const float v = s.oneShot ? 0.0f : 1.0f;
                send (F::FieldOneShot, v);
                if (v > 0.0f && s.loopMode > 0) send (F::FieldLoop, 0.0f);
                break;
            }
            case 3: send (F::FieldGlobalMono, mono() ? 0.0f : 1.0f); break;
            case 4: send (F::FieldStretchEnabled, s.stretchEnabled ? 0.0f : 1.0f); break;
            case 5: send (F::FieldReleaseTail, s.releaseTail ? 0.0f : 1.0f); break;
            default: break;
        }
    }

    void timerCallback() override { if (isShowing()) repaint(); }

    DysektProcessor& processor;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SliceFlagRow)
};
