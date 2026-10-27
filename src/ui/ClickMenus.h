#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"

//==============================================================================
//  ClickMenus - the two context menus shared by every transport bar.
//    showRecordMenu : record mode (Overdub / Add) and count-in length (Off / 1 / 2 bars)
//    showClickMenu  : metronome on/off, click volume, click output
//==============================================================================
namespace ClickMenus
{
    inline void showRecordMenu (SequencerEngine& engine)
    {
        const auto mode = engine.getRecordMode();
        const int countIn = engine.getCountInBars();

        juce::PopupMenu m;
        m.addItem (1, juce::String (juce::CharPointer_UTF8 ("Overdub \xe2\x80\x94 merge into existing clip")), true, mode == SequencerEngine::RecordMode::Overdub);
        m.addItem (2, juce::String (juce::CharPointer_UTF8 ("Add \xe2\x80\x94 always start a new clip")),      true, mode == SequencerEngine::RecordMode::Add);
        m.addSeparator();
        m.addItem (3, "Count-in: off",    true, countIn == 0);
        m.addItem (4, "Count-in: 1 bar",  true, countIn == 1);
        m.addItem (5, "Count-in: 2 bars", true, countIn == 2);

        m.showMenuAsync (juce::PopupMenu::Options(), [&engine] (int r)
        {
            if      (r == 1) engine.setRecordMode (SequencerEngine::RecordMode::Overdub);
            else if (r == 2) engine.setRecordMode (SequencerEngine::RecordMode::Add);
            else if (r == 3) engine.setCountInBars (0);
            else if (r == 4) engine.setCountInBars (1);
            else if (r == 5) engine.setCountInBars (2);
        });
    }

    inline void showClickMenu (SequencerEngine& engine)
    {
        const float vol = engine.getMetronomeVolume();
        const int out = engine.getClickOutput();

        juce::PopupMenu m;
        m.addItem (1, "Metronome on", true, engine.isMetronomeEnabled());
        m.addSeparator();
        const float vols[] = { 0.25f, 0.5f, 0.75f, 1.0f };
        for (int i = 0; i < 4; ++i)
            m.addItem (10 + i, "Volume " + juce::String (juce::roundToInt (vols[i] * 100.f)) + "%", true,
                       std::abs (vol - vols[i]) < 0.13f && (i == 3 || std::abs (vol - vols[i]) <= std::abs (vol - vols[i + 1])));
        m.addSeparator();
        juce::PopupMenu outs;
        outs.addItem (20, "Main mix", true, out == 0);
        for (int b = 1; b <= 8; ++b)
            outs.addItem (20 + b, "Out " + juce::String (b + 1), true, out == b);
        m.addSubMenu ("Click output", outs);

        m.showMenuAsync (juce::PopupMenu::Options(), [&engine, vols] (int r)
        {
            if      (r == 1)              engine.setMetronomeEnabled (! engine.isMetronomeEnabled());
            else if (r >= 10 && r <= 13)  engine.setMetronomeVolume (vols[r - 10]);
            else if (r >= 20 && r <= 28)  engine.setClickOutput (r - 20);
        });
    }
}
