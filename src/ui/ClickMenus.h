#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"

//==============================================================================
//  ClickMenus - the two context menus shared by every transport bar.
//    showRecordMenu : record mode (Overdub / Add) and count-in length (Off / 1 / 2 bars)
//    showClickMenu  : metronome on/off, click volume, click output
//==============================================================================
namespace RecordOffsetStore
{
    inline juce::File file()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("DYSEKT-SF").getChildFile ("record-offset.xml");
    }
    inline void save (const SequencerEngine& e)
    {
        juce::XmlElement x ("RECORDOFFSET");
        x.setAttribute ("ms", (double) e.getRecordOffsetMs());
        x.setAttribute ("auto", e.isRecordOffsetAuto());
        const auto f = file();
        f.getParentDirectory().createDirectory();
        x.writeTo (f);
    }
    inline void load (SequencerEngine& e)
    {
        if (auto x = juce::XmlDocument::parse (file()))
            if (x->hasTagName ("RECORDOFFSET"))
            {
                e.setRecordOffsetMs ((float) x->getDoubleAttribute ("ms", 0.0));
                e.setRecordOffsetAuto (x->getBoolAttribute ("auto", true));
            }
    }
}

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
        m.addSeparator();
        juce::PopupMenu lat;
        lat.addItem (30, "Auto (audio device latency: " + juce::String (engine.getDeviceLatencyMs(), 1) + " ms)", true, engine.isRecordOffsetAuto());
        lat.addSeparator();
        const float offs[] = { -20.f, -10.f, 0.f, 10.f, 20.f, 40.f };
        for (int i = 0; i < 6; ++i)
            lat.addItem (40 + i, "Extra offset " + juce::String (offs[i] > 0 ? "+" : "") + juce::String ((int) offs[i]) + " ms", true,
                         std::abs (engine.getRecordOffsetMs() - offs[i]) < 0.5f);
        m.addSubMenu ("Latency compensation", lat);

        m.showMenuAsync (juce::PopupMenu::Options(), [&engine] (int r)
        {
            if      (r == 1) engine.setRecordMode (SequencerEngine::RecordMode::Overdub);
            else if (r == 2) engine.setRecordMode (SequencerEngine::RecordMode::Add);
            else if (r == 3) engine.setCountInBars (0);
            else if (r == 4) engine.setCountInBars (1);
            else if (r == 5) engine.setCountInBars (2);
            else if (r == 30) engine.setRecordOffsetAuto (! engine.isRecordOffsetAuto());
            else if (r >= 40 && r <= 45)
            {
                const float offs2[] = { -20.f, -10.f, 0.f, 10.f, 20.f, 40.f };
                engine.setRecordOffsetMs (offs2[r - 40]);
            }
            else return;
            RecordOffsetStore::save (engine);
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
