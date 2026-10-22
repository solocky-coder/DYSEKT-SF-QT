#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "DysektLookAndFeel.h"

//==============================================================================
//  SliceTable
//
//  Bench layout dock under the slicer waveform: one row per slice with its
//  colour, number, name, start, length and lock state. Clicking a row selects
//  that slice and auditions it using the same commands as a pad click
//  (CmdSelectSlice + uiNoteOnRequest). Read-only otherwise; it never edits a
//  slice. Shown only in the Bench theme while the Slicer tab has a sample.
//==============================================================================
class SliceTable final : public juce::Component,
                         private juce::Timer
{
public:
    /** Design height in layout units (scaled by the editor like every other region). */
    static constexpr int kDesignHeight = 200;

    explicit SliceTable (DysektProcessor& p) : processor (p) { startTimerHz (10); }
    ~SliceTable() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        const auto& t  = getTheme();
        const float sf = scale();
        const auto& ui = processor.getUiSliceSnapshot();
        const double sr = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0;

        const auto bf = getLocalBounds().toFloat();
        g.setColour (t.waveformBg);
        g.fillRoundedRectangle (bf, t.cornerRadius);
        g.setColour (t.separator);
        g.drawRoundedRectangle (bf.reduced (0.5f), t.cornerRadius, 1.0f);

        const int pad = juce::roundToInt (12.0f * sf);

        // Header
        auto head = getLocalBounds().reduced (pad, 0).removeFromTop (headerH());
        g.setColour (t.foreground);
        g.setFont (DysektLookAndFeel::makeFont (14.0f * sf, true));
        g.drawText ("Slices", head, juce::Justification::centredLeft, false);
        g.setColour (t.foreground.withAlpha (0.5f));
        g.setFont (DysektLookAndFeel::makeFont (11.5f * sf));
        g.drawText ("click a row to audition", head, juce::Justification::centredRight, false);

        // Column titles
        const auto cols = columns();
        auto colRow = juce::Rectangle<int> (pad, headerH(), getWidth() - pad * 2, rowH());
        g.setFont (DysektLookAndFeel::makeFont (10.5f * sf));
        g.setColour (t.foreground.withAlpha (0.45f));
        g.drawText ("#",      colRow.withX (cols.num).withWidth (cols.name - cols.num),       juce::Justification::centredLeft, false);
        g.drawText ("NAME",   colRow.withX (cols.name).withWidth (cols.start - cols.name),    juce::Justification::centredLeft, false);
        g.drawText ("START",  colRow.withX (cols.start).withWidth (cols.length - cols.start), juce::Justification::centredLeft, false);
        g.drawText ("LENGTH", colRow.withX (cols.length).withWidth (cols.lock - cols.length), juce::Justification::centredLeft, false);
        g.drawText ("LOCK",   colRow.withX (cols.lock).withWidth (getWidth() - pad - cols.lock), juce::Justification::centredLeft, false);

        if (ui.numSlices <= 0)
        {
            g.setColour (t.foreground.withAlpha (0.4f));
            g.setFont (DysektLookAndFeel::makeFont (12.5f * sf));
            g.drawText ("No slices yet. Use Auto slice or click the waveform to add one.",
                        getLocalBounds().withTrimmedTop (headerH() + rowH()), juce::Justification::centred, false);
            return;
        }

        scrollRow = juce::jlimit (0, juce::jmax (0, ui.numSlices - visibleRows()), scrollRow);

        g.saveState();
        g.reduceClipRegion (getLocalBounds().withTrimmedTop (headerH() + rowH()).reduced (1));
        const int firstY = headerH() + rowH();
        for (int i = scrollRow; i < ui.numSlices; ++i)
        {
            const int y = firstY + (i - scrollRow) * rowH();
            if (y > getHeight()) break;

            const auto& s = ui.slices[(size_t) i];
            const bool sel = (i == ui.selectedSlice);
            auto row = juce::Rectangle<int> (pad - 4, y, getWidth() - (pad - 4) * 2, rowH());
            if (sel)
            {
                g.setColour (t.accent.withAlpha (0.16f));
                g.fillRoundedRectangle (row.toFloat(), t.cornerRadius * 0.5f);
            }

            // colour chip
            const float chip = 10.0f * sf;
            g.setColour (s.colour);
            g.fillRoundedRectangle ((float) pad, (float) y + ((float) rowH() - chip) * 0.5f, chip, chip, 2.5f * sf);

            const int endS   = ui.sliceEndSamples[(size_t) i];
            const double st  = (double) s.startSample / sr;
            const double len = (double) juce::jmax (0, endS - s.startSample) / sr;

            g.setFont (DysektLookAndFeel::makeMonoFont (11.5f * sf));
            g.setColour (t.foreground.withAlpha (0.6f));
            g.drawText (juce::String (i + 1).paddedLeft ('0', 2),
                        row.withX (cols.num).withWidth (cols.name - cols.num), juce::Justification::centredLeft, false);
            g.setColour (t.foreground);
            g.setFont (DysektLookAndFeel::makeFont (13.0f * sf));
            g.drawText (s.name.isNotEmpty() ? s.name : ("Slice " + juce::String (i + 1)),
                        row.withX (cols.name).withWidth (cols.start - cols.name - 6), juce::Justification::centredLeft, true);
            g.setFont (DysektLookAndFeel::makeMonoFont (11.5f * sf));
            g.setColour (t.foreground.withAlpha (0.7f));
            g.drawText (juce::String (st, 3), row.withX (cols.start).withWidth (cols.length - cols.start), juce::Justification::centredLeft, false);
            g.drawText (juce::String (len, 3), row.withX (cols.length).withWidth (cols.lock - cols.length), juce::Justification::centredLeft, false);
            if (s.lockMask != 0)
            {
                g.setColour (t.lockActive);
                g.fillEllipse ((float) cols.lock, (float) y + ((float) rowH() - 7.0f * sf) * 0.5f, 7.0f * sf, 7.0f * sf);
            }
        }
        g.restoreState();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int idx = rowAt (e.getPosition());
        if (idx < 0) return;

        DysektProcessor::Command cmd;
        cmd.type      = DysektProcessor::CmdSelectSlice;
        cmd.intParam1 = idx;
        processor.pushCommand (cmd);

        // Audition, same as a pad click.
        processor.uiNoteOnRequest.store (processor.sliceManager.getSlice (idx).midiNote, std::memory_order_relaxed);
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        const int before = scrollRow;
        scrollRow -= (w.deltaY > 0.0f ? 1 : (w.deltaY < 0.0f ? -1 : 0));
        scrollRow = juce::jmax (0, scrollRow);
        if (scrollRow != before) repaint();
    }

private:
    struct Cols { int num, name, start, length, lock; };

    float scale() const noexcept { return (float) getHeight() / (float) kDesignHeight; }
    int   headerH() const noexcept { return juce::roundToInt (28.0f * scale()); }
    int   rowH() const noexcept { return juce::jmax (12, juce::roundToInt (24.0f * scale())); }
    int   visibleRows() const noexcept { return juce::jmax (1, (getHeight() - headerH() - rowH()) / rowH()); }

    Cols columns() const noexcept
    {
        const int pad = juce::roundToInt (12.0f * scale());
        const int w   = getWidth() - pad * 2;
        Cols c;
        c.num    = pad + juce::roundToInt (22.0f * scale());
        c.name   = c.num + juce::roundToInt (30.0f * scale());
        c.lock   = pad + w - juce::roundToInt (46.0f * scale());
        c.length = c.lock - juce::roundToInt (90.0f * scale());
        c.start  = c.length - juce::roundToInt (90.0f * scale());
        return c;
    }

    int rowAt (juce::Point<int> p) const
    {
        const auto& ui = processor.getUiSliceSnapshot();
        const int y0 = headerH() + rowH();
        if (p.y < y0) return -1;
        const int idx = scrollRow + (p.y - y0) / rowH();
        return (idx >= 0 && idx < ui.numSlices) ? idx : -1;
    }

    void timerCallback() override { if (isShowing()) repaint(); }

    DysektProcessor& processor;
    mutable int scrollRow = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SliceTable)
};
