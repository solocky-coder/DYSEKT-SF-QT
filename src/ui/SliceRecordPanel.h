#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "XivaLookAndFeel.h"

//==============================================================================
//  SliceRecordPanel
//
//  Covers the Slicer's waveform area while recording a sample. Source is the
//  audio input or the master output; while a take runs, every MIDI key press
//  drops a slice marker at that exact position. STOP & USE loads the take into
//  the Slicer with those markers as slices.
//==============================================================================
class SliceRecordPanel final : public juce::Component,
                               private juce::Timer
{
public:
    std::function<void()> onClose;   // editor hides the panel

    explicit SliceRecordPanel (XivaProcessor& p) : processor (p)
    {
        setOpaque (false);
#if ! XIVA_STANDALONE
        sourceChoice = SliceRecorder::Source::Output;   // plugin builds have no audio input bus
#endif
    }
    ~SliceRecordPanel() override { stopTimer(); }

    void visibilityChanged() override
    {
        if (isVisible()) startTimerHz (30); else stopTimer();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = getTheme();
        auto& rec = processor.sliceRecorder;
        const bool recording = rec.isRecording();
        hits.clear();

        const auto bf = getLocalBounds().toFloat();
        g.setColour (t.waveformBg);
        g.fillRoundedRectangle (bf, t.cornerRadius);
        g.setColour (t.separator);
        g.drawRoundedRectangle (bf.reduced (0.5f), t.cornerRadius, 1.0f);

        const int pad = 16;
        auto area = getLocalBounds().reduced (pad);

        // ── Header: title + source switch ───────────────────────────────────
        auto head = area.removeFromTop (34);
        g.setColour (t.foreground);
        g.setFont (XivaLookAndFeel::makeFont (15.5f, true));
        g.drawText ("Record sample", head.removeFromLeft (180), juce::Justification::centredLeft);

        auto src = head.removeFromRight (250);
        const bool inputSel = (sourceChoice == SliceRecorder::Source::Input);
        chip (g, src.removeFromLeft (122), "AUDIO INPUT", inputSel, ! recording, 1);
        src.removeFromLeft (6);
        chip (g, src.removeFromLeft (122), "MASTER OUT", ! inputSel, ! recording, 2);

        // ── Footer: status + buttons ─────────────────────────────────────────
        auto foot = area.removeFromBottom (48);
        area.removeFromBottom (8);

        auto btns = foot.removeFromRight (250);
        button (g, btns.removeFromLeft (150), recording ? "STOP & USE" : "REC", true, recording ? 3 : 4);
        btns.removeFromLeft (8);
        button (g, btns, recording ? "CANCEL" : "CLOSE", false, 5);

        const double sr = rec.getSampleRate() > 0.0 ? rec.getSampleRate() : juce::jmax (1.0, processor.getSampleRate());
        const double secs = (double) rec.getFrames() / sr;
        g.setFont (XivaLookAndFeel::makeFont (13.0f, true));
        g.setColour (recording ? t.accent : t.foreground.withAlpha (0.8f));
        g.drawText (formatTime (secs), foot.removeFromLeft (92), juce::Justification::centredLeft);

        g.setFont (XivaLookAndFeel::makeFont (11.5f));
        g.setColour (t.foreground.withAlpha (0.6f));
        juce::String status;
        if (recording)
            status = juce::String (rec.getNumMarkers()) + " slice markers  -  press any MIDI key to mark a slice"
                     + (rec.isFull() ? "  -  buffer full, press STOP" : "");
        else if (sourceChoice == SliceRecorder::Source::Input && rec.getInputChannelCount() == 0)
            status = "No audio input is enabled. Enable one under Audio / MIDI > Audio Settings.";
        else
            status = "Press REC, then play MIDI keys to mark slices while it records. Notes are not sounded while recording.";
        g.drawText (status, foot, juce::Justification::centredLeft, true);

        // ── Live waveform ────────────────────────────────────────────────────
        auto wave = area;
        g.setColour (t.darkBar);
        g.fillRoundedRectangle (wave.toFloat(), 6.0f);
        const auto inner = wave.reduced (8, 8);

        if (inner.getWidth() > 4 && inner.getHeight() > 4)
        {
            rec.snapshot (peaksCopy, markersCopy);
            const int totalPeaks = (int) peaksCopy.size();
            // Fixed 30 s window that grows then compresses, so the take always fits.
            const int framesShown = juce::jmax (rec.getFrames(), (int) (sr * 10.0));
            const float pxPerFrame = (float) inner.getWidth() / (float) framesShown;
            const float midY = (float) inner.getCentreY();
            const float halfH = (float) inner.getHeight() * 0.5f;

            g.setColour (t.separator);
            g.drawHorizontalLine ((int) midY, (float) inner.getX(), (float) inner.getRight());

            g.setColour (t.accent.withAlpha (0.85f));
            for (int x = 0; x < inner.getWidth(); ++x)
            {
                const int f0 = (int) ((float) x / pxPerFrame);
                const int f1 = (int) ((float) (x + 1) / pxPerFrame);
                const int i0 = f0 / SliceRecorder::kPeakBlock;
                const int i1 = juce::jmax (i0, (f1 - 1) / SliceRecorder::kPeakBlock);
                if (i0 >= totalPeaks) break;
                float pk = 0.0f;
                for (int i = i0; i <= i1 && i < totalPeaks; ++i)
                    pk = juce::jmax (pk, peaksCopy[(size_t) i]);
                const float h = juce::jmax (1.0f, juce::jmin (1.0f, pk) * halfH);
                g.drawVerticalLine (inner.getX() + x, midY - h, midY + h);
            }

            g.setColour (t.foreground.withAlpha (0.9f));
            g.setFont (XivaLookAndFeel::makeFont (10.0f, true));
            int n = 1;
            for (int m : markersCopy)
            {
                const int x = inner.getX() + juce::roundToInt ((float) m * pxPerFrame);
                if (x < inner.getRight())
                {
                    g.drawVerticalLine (x, (float) inner.getY(), (float) inner.getBottom());
                    g.drawText (juce::String (n), x + 3, inner.getY(), 28, 14, juce::Justification::centredLeft);
                }
                ++n;
            }

            // Input level meter along the bottom edge
            const float lv = juce::jlimit (0.0f, 1.0f, rec.getLevel());
            const float lvDb = lv > 0.0f ? juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (lv) + 60.0f) / 60.0f) : 0.0f;
            g.setColour (t.separator);
            g.fillRect (inner.getX(), inner.getBottom() - 4, inner.getWidth(), 4);
            g.setColour (lv > 0.97f ? juce::Colour (0xFFE05050) : t.accent);
            g.fillRect (inner.getX(), inner.getBottom() - 4, juce::roundToInt ((float) inner.getWidth() * lvDb), 4);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (const auto& h : hits)
            if (h.r.contains (e.getPosition()) && h.enabled) { activate (h.id); repaint(); return; }
    }

    /** Stops an in-progress take without keeping it (used when the editor closes). */
    void discardIfRecording()
    {
        if (processor.sliceRecorder.isRecording())
            processor.sliceRecorder.stop();
    }

private:
    struct Hit { juce::Rectangle<int> r; int id; bool enabled; };

    void timerCallback() override { repaint(); }

    static juce::String formatTime (double s)
    {
        const int m = (int) (s / 60.0);
        return juce::String (m) + ":" + juce::String (s - m * 60.0, 1).paddedLeft ('0', 4);
    }

    void chip (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, bool on, bool enabled, int id)
    {
        const auto& t = getTheme();
        g.setColour (on ? t.accent.withAlpha (enabled ? 1.0f : 0.5f) : t.button);
        g.fillRoundedRectangle (r.toFloat(), 7.0f);
        if (! on)
        {
            g.setColour (t.separator);
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 7.0f, 1.0f);
        }
        g.setColour (on ? t.waveformBg : t.foreground.withAlpha (enabled ? 0.8f : 0.4f));
        g.setFont (XivaLookAndFeel::makeFont (11.5f, true));
        g.drawText (text, r, juce::Justification::centred);
        hits.push_back ({ r, id, enabled });
    }

    void button (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text, bool primary, int id)
    {
        const auto& t = getTheme();
        const bool recording = processor.sliceRecorder.isRecording();
        g.setColour (primary ? (recording ? juce::Colour (0xFFE05050) : t.accent) : t.button);
        g.fillRoundedRectangle (r.toFloat(), 7.0f);
        if (! primary)
        {
            g.setColour (t.separator);
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 7.0f, 1.0f);
        }
        g.setColour (primary ? t.waveformBg : t.foreground.withAlpha (0.85f));
        g.setFont (XivaLookAndFeel::makeFont (13.0f, true));
        g.drawText (text, r, juce::Justification::centred);
        hits.push_back ({ r, id, true });
    }

    void activate (int id)
    {
        auto& rec = processor.sliceRecorder;
        switch (id)
        {
            case 1: sourceChoice = SliceRecorder::Source::Input;  break;
            case 2: sourceChoice = SliceRecorder::Source::Output; break;
            case 4: rec.start (sourceChoice, processor.getSampleRate()); break;
            case 3:   // STOP & USE
            {
                auto take = rec.stop();
                if (take.getNumFrames() > 0)
                    processor.commitRecordedTake (std::move (take));
                if (onClose) onClose();
                break;
            }
            case 5:   // CANCEL / CLOSE
                if (rec.isRecording()) rec.stop();
                if (onClose) onClose();
                break;
            default: break;
        }
    }

    XivaProcessor& processor;
    SliceRecorder::Source sourceChoice { SliceRecorder::Source::Input };
    std::vector<Hit>   hits;
    std::vector<float> peaksCopy;
    std::vector<int>   markersCopy;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SliceRecordPanel)
};
