#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

class XivaProcessor;

class LogoBar : public juce::Component
{
public:
    explicit LogoBar (XivaProcessor& p);
    void paint (juce::Graphics& g) override;
private:
    XivaProcessor& processor;
};
