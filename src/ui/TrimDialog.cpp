#include "TrimDialog.h"
#include "XivaLookAndFeel.h"
#include "UIHelpers.h"
#include "WaveformView.h"
#include "../PluginProcessor.h"
#include "../PluginEditor.h"

TrimDialog::TrimDialog (XivaProcessor& proc, WaveformView& wv)
    : processor (proc), waveformView (wv)
{
    UIHelpers::stylePrimaryPopupButton (applyBtn, getTheme());
    applyBtn.onClick = [this] { onApply(); };
    addAndMakeVisible (applyBtn);

    UIHelpers::styleSecondaryPopupButton (cancelBtn, getTheme());
    cancelBtn.onClick = [this] { onCancel(); };
    addAndMakeVisible (cancelBtn);

    UIHelpers::styleSecondaryPopupButton (snapBtn, getTheme());
    snapBtn.onClick = [this] { snapToZeroCrossings(); };
    snapBtn.setTooltip ("Move IN and OUT to the nearest zero crossing so the cut does not click");
    addAndMakeVisible (snapBtn);

    UIHelpers::styleSecondaryPopupButton (resetBtn, getTheme());
    resetBtn.onClick = [this] { resetToFull(); };
    addAndMakeVisible (resetBtn);

    // 30 Hz repaint keeps cells in sync with MIDI CC driving trimRegionStart/End
    startTimerHz (30);
}

TrimDialog::~TrimDialog() { stopTimer(); }

void TrimDialog::timerCallback() { repaint(); }

// ─────────────────────────────────────────────────────────────────────────────
//  Bench trim card: IN / OUT (draggable) and LENGTH readouts, helper buttons, Cancel / Apply
// ─────────────────────────────────────────────────────────────────────────────
static juce::String formatTrimTime (double seconds)
{
    seconds = juce::jmax (0.0, seconds);
    const int m = (int) (seconds / 60.0);
    return juce::String (m) + ":" + juce::String (seconds - m * 60.0, 3).paddedLeft ('0', 6);
}

static juce::String formatThousands (int v)
{
    auto s = juce::String (v);
    juce::String out;
    for (int i = 0; i < s.length(); ++i)
    {
        if (i > 0 && (s.length() - i) % 3 == 0) out += " ";
        out += juce::String::charToString (s[i]);
    }
    return out;
}

void TrimDialog::drawTrimKnob (juce::Graphics& g,
                                juce::Rectangle<int> cell,
                                const char* label, int sampleVal, int totalFrames,
                                bool invertFill)
{
    const auto& T = getTheme();
    const float sf = (float) getHeight() / 92.0f;
    const double sr = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0;
    const bool draggable = (label[0] == 'I' || label[0] == 'O');

    g.setColour (T.button);
    g.fillRoundedRectangle (cell.toFloat(), T.cornerRadius);
    g.setColour (T.separator);
    g.drawRoundedRectangle (cell.toFloat().reduced (0.5f), T.cornerRadius, 1.0f);

    auto r = cell.reduced (juce::roundToInt (12.0f * sf), juce::roundToInt (8.0f * sf));
    g.setFont (XivaLookAndFeel::makeFont (10.0f * sf, true));
    g.setColour (T.foreground.withAlpha (0.6f));
    g.drawText (label, r.removeFromTop (juce::roundToInt (13.0f * sf)), juce::Justification::centredLeft, false);

    g.setFont (XivaLookAndFeel::makeFont (18.0f * sf, true));
    g.setColour (draggable ? T.accent : T.foreground);
    g.drawText (formatTrimTime ((double) sampleVal / sr), r.removeFromTop (juce::roundToInt (24.0f * sf)),
                juce::Justification::centredLeft, false);

    g.setFont (XivaLookAndFeel::makeFont (11.0f * sf));
    g.setColour (T.foreground.withAlpha (0.55f));
    g.drawText (formatThousands (sampleVal) + " smp", r, juce::Justification::centredLeft, false);

    juce::ignoreUnused (totalFrames, invertFill);
}

void TrimDialog::paint (juce::Graphics& g)
{
    const auto& T = getTheme();
    const auto bf = getLocalBounds().toFloat();
    g.setColour (T.waveformBg);
    g.fillRoundedRectangle (bf, T.cornerRadius);
    g.setColour (T.separator);
    g.drawRoundedRectangle (bf.reduced (0.5f), T.cornerRadius, 1.0f);

    const int total = processor.sampleData.getNumFrames();
    const int inPt  = processor.trimRegionStart.load (std::memory_order_relaxed);
    const int outPt = processor.trimRegionEnd  .load (std::memory_order_relaxed);

    drawTrimKnob (g, inCell,  "IN",     inPt,         total, true);
    drawTrimKnob (g, outCell, "OUT",    outPt,        total, false);
    drawTrimKnob (g, lenCell, "LENGTH", outPt - inPt, total, false);
}

void TrimDialog::resized()
{
    const float sf = (float) getHeight() / 92.0f;
    auto si = [sf] (int v) { return juce::roundToInt ((float) v * sf); };

    auto b = getLocalBounds().reduced (si (14), si (10));
    const int gap = si (12);

    applyBtn.setBounds (b.removeFromRight (si (118)).withSizeKeepingCentre (si (118), si (44)));
    b.removeFromRight (gap);
    cancelBtn.setBounds (b.removeFromRight (si (96)).withSizeKeepingCentre (si (96), si (44)));
    b.removeFromRight (gap * 2);

    const int cellW = juce::jmax (si (60), juce::jmin (si (210), (b.getWidth() - gap * 4 - si (190)) / 3));
    inCell  = b.removeFromLeft (cellW);  b.removeFromLeft (gap);
    outCell = b.removeFromLeft (cellW);  b.removeFromLeft (gap);
    lenCell = b.removeFromLeft (cellW);  b.removeFromLeft (gap);

    auto col = b.removeFromLeft (juce::jmin (b.getWidth(), si (190)));
    snapBtn .setBounds (col.removeFromTop (si (34)));
    col.removeFromTop (si (6));
    resetBtn.setBounds (col.removeFromTop (si (34)));
    labelArea = {};
}

// Moves IN and OUT to the nearest sign change of the first channel (within ~50 ms) so the cut does not click.
void TrimDialog::snapToZeroCrossings()
{
    auto snap = processor.sampleData.getSnapshot();
    if (snap == nullptr || snap->buffer.getNumChannels() == 0) return;
    const auto* d = snap->buffer.getReadPointer (0);
    const int total = snap->buffer.getNumSamples();
    const int window = juce::jmax (64, (int) (0.05 * (processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0)));

    auto nearest = [&] (int pos)
    {
        pos = juce::jlimit (1, total - 1, pos);
        for (int k = 0; k <= window; ++k)
            for (int sgn : { -1, 1 })
            {
                const int i = pos + sgn * k;
                if (i < 1 || i >= total) continue;
                if ((d[i - 1] <= 0.0f && d[i] > 0.0f) || (d[i - 1] >= 0.0f && d[i] < 0.0f)) return i;
            }
        return pos;
    };

    const int curIn  = processor.trimRegionStart.load (std::memory_order_relaxed);
    const int curOut = processor.trimRegionEnd  .load (std::memory_order_relaxed);
    const int inPt  = curIn  <= 0     ? 0     : nearest (curIn);
    const int outPt = curOut >= total ? total : nearest (curOut);
    if (outPt - inPt < 64) return;
    processor.trimRegionStart.store (inPt,  std::memory_order_relaxed);
    processor.trimRegionEnd  .store (outPt, std::memory_order_relaxed);
    waveformView.setTrimPoints (inPt, outPt);
    repaint();
}

void TrimDialog::resetToFull()
{
    const int total = processor.sampleData.getNumFrames();
    if (total <= 0) return;
    processor.trimRegionStart.store (0,     std::memory_order_relaxed);
    processor.trimRegionEnd  .store (total, std::memory_order_relaxed);
    waveformView.setTrimPoints (0, total);
    repaint();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Mouse — vertical drag moves IN / OUT, shift = fine
//  Same CC: FieldSliceStart drives IN, FieldSliceEnd drives OUT (in processor)
// ─────────────────────────────────────────────────────────────────────────────
void TrimDialog::showMidiLearnMenu (int fieldId, juce::Point<int> screenPos)
{
    const bool mapped = processor.midiLearn.isMapped (fieldId);
    juce::PopupMenu menu;
    menu.addItem (1, "Learn MIDI CC");
    if (mapped)
        menu.addItem (2, "Clear (" + processor.midiLearn.getLabelText (fieldId) + ")");
    menu.addSeparator();
    menu.addItem (1000, "Open MIDI Learn Dialog...");
    auto* topLvl = getTopLevelComponent();
    float ms = XivaLookAndFeel::getMenuScale();
    menu.showMenuAsync (
        juce::PopupMenu::Options()
            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1))
            .withParentComponent (topLvl)
            .withStandardItemHeight ((int)(24 * ms)),
        [this, fieldId] (int result) {
            if (result == 1) { processor.midiLearn.armLearn (fieldId); repaint(); }
            else if (result == 2) { processor.midiLearn.clearMapping (fieldId); repaint(); }
            else if (result == 1000)
            {
                if (auto* editor = findParentComponentOfClass<XivaEditor>())
                    editor->keyPressed (juce::KeyPress ('M', juce::ModifierKeys::commandModifier, 0));
            }
        }
    );
}
void TrimDialog::mouseDown (const juce::MouseEvent& e)
{
    using F = XivaProcessor;
    if (inCell.contains (e.getPosition()))
    {
        if (e.mods.isRightButtonDown()) { showMidiLearnMenu (F::FieldSliceStart, e.getScreenPosition()); return; }
        activeDrag = 0;
    }
    else if (outCell.contains (e.getPosition()))
    {
        if (e.mods.isRightButtonDown()) { showMidiLearnMenu (F::FieldTrimOut, e.getScreenPosition()); return; }
        activeDrag = 1;
    }
    else return;
    dragStartY   = e.getPosition().y;
    dragStartVal = (activeDrag == 0)
        ? processor.trimRegionStart.load (std::memory_order_relaxed)
        : processor.trimRegionEnd  .load (std::memory_order_relaxed);
}

void TrimDialog::mouseDrag (const juce::MouseEvent& e)
{
    if (activeDrag < 0) return;

    const int total = processor.sampleData.getNumFrames();
    if (total <= 0) return;

    float sensitivity = (float) total / 300.f;
    if (e.mods.isShiftDown()) sensitivity *= 0.05f;

    const int delta = (int) ((dragStartY - e.getPosition().y) * sensitivity);

    if (activeDrag == 0)
    {
        const int curEnd = processor.trimRegionEnd.load (std::memory_order_relaxed);
        const int newIn  = juce::jlimit (0, curEnd - 64, dragStartVal + delta);
        processor.trimRegionStart.store (newIn, std::memory_order_relaxed);
        waveformView.setTrimPoints (newIn, curEnd);
    }
    else
    {
        const int curIn  = processor.trimRegionStart.load (std::memory_order_relaxed);
        const int newOut = juce::jlimit (curIn + 64, total, dragStartVal + delta);
        processor.trimRegionEnd.store (newOut, std::memory_order_relaxed);
        waveformView.setTrimPoints (curIn, newOut);
    }

    repaint();
}

void TrimDialog::mouseUp (const juce::MouseEvent&) { activeDrag = -1; }

// ─────────────────────────────────────────────────────────────────────────────
void TrimDialog::onApply()
{
    const int inPt  = processor.trimRegionStart.load (std::memory_order_relaxed);
    const int outPt = processor.trimRegionEnd  .load (std::memory_order_relaxed);
    if (waveformView.onTrimApplied)
        waveformView.onTrimApplied (inPt, outPt);
}

void TrimDialog::onCancel()
{
    if (waveformView.onTrimCancelled)
        waveformView.onTrimCancelled();
}
