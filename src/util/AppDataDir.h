#pragma once
#include <juce_core/juce_core.h>

/** The per-user app-data folder (settings, bookmarks, themes, device labels ...).
    The folder used to be called "DYSEKT-SF". The first time the new "XIVA" folder is
    asked for and does not exist yet, the old folder is copied across (the old one is
    left untouched, so nothing is lost if the user goes back to an older build). */
namespace XivaPaths
{
    inline juce::File appDataDir()
    {
        static const juce::File dir = []
        {
            const auto base   = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
            const auto fresh  = base.getChildFile ("XIVA");
            const auto legacy = base.getChildFile ("DYSEKT-SF");
            if (! fresh.exists() && legacy.isDirectory())
                legacy.copyDirectoryTo (fresh);
            fresh.createDirectory();
            return fresh;
        }();
        return dir;
    }
}
