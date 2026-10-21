#pragma once
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "DysektLookAndFeel.h"

//==============================================================================
//  NavRail
//
//  Vertical icon rail for the Bench layout. It owns no state of its own: every
//  click is forwarded through onItem to the editor's existing handlers
//  (setUiMode, toggleBrowserPanel, toggleMixerPanel, ...), and the editor
//  pushes the current tab / panel state back in with setTab() / setPanelActive().
//  Only shown while the Bench theme is active; other themes never see it.
//==============================================================================
class NavRail final : public juce::Component,
                      public juce::TooltipClient
{
public:
    enum Item { Slicer = 0, Multi, Sf2, Browser, Mixer, Eq, Seq, Settings, NumItems };

    /** Design width in layout units (scaled by the editor like every other region). */
    static constexpr int kDesignWidth = 60;

    std::function<void (Item)> onItem;

    void setTab (int tab)
    {
        if (tab != currentTab) { currentTab = tab; repaint(); }
    }

    void setPanelActive (Item item, bool active)
    {
        if (item < 0 || item >= NumItems) return;
        if (panelActive[item] != active) { panelActive[item] = active; repaint(); }
    }

    void setSeqAvailable (bool available)
    {
        if (seqAvailable != available) { seqAvailable = available; resized(); repaint(); }
    }

    void resized() override
    {
        const float sf = scale();
        const float itemW = (float) getWidth() - 8.0f * sf;
        const float itemH = 50.0f * sf;
        const float gap   = 4.0f * sf;
        const float x     = 4.0f * sf;
        float y = 62.0f * sf;

        for (auto& r : rects) r = {};

        auto place = [&] (Item it) { rects[it] = { x, y, itemW, itemH }; y += itemH + gap; };

        place (Slicer); place (Multi); place (Sf2);
        dividerY = y + gap * 0.5f;
        y += gap * 2.0f;
        place (Browser); place (Mixer); place (Eq);
        if (seqAvailable) place (Seq);

        rects[Settings] = { x, (float) getHeight() - itemH - 10.0f * sf, itemW, itemH };
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t  = getTheme();
        const float sf = scale();

        g.setColour (t.waveformBg);
        g.fillRect (getLocalBounds());
        g.setColour (t.separator);
        g.fillRect (getWidth() - 1, 0, 1, getHeight());

        // Mark
        {
            auto mark = juce::Rectangle<float> (36.0f * sf, 36.0f * sf)
                            .withCentre ({ (float) getWidth() * 0.5f, 30.0f * sf });
            g.setColour (t.foreground);
            g.fillRoundedRectangle (mark, 10.0f * sf);
            g.setColour (t.waveformBg);
            g.setFont (juce::Font (juce::FontOptions (15.0f * sf, juce::Font::bold)));
            g.drawText ("DS", mark.toNearestInt(), juce::Justification::centred, false);
        }

        // Group divider
        g.setColour (t.separator);
        g.fillRect (juce::Rectangle<float> (12.0f * sf, dividerY, (float) getWidth() - 24.0f * sf, 1.0f));

        static const char* labels[NumItems] = { "Slicer", "Multi", "SF2", "Browser", "Mixer", "EQ", "Seq", "Settings" };

        for (int i = 0; i < NumItems; ++i)
        {
            const auto r = rects[i];
            if (r.isEmpty()) continue;

            const bool active  = isActive ((Item) i);
            const bool hovered = (i == hoverItem);

            if (active)
            {
                g.setColour (t.accent);
                g.fillRoundedRectangle (r, 12.0f * sf);
            }
            else if (hovered)
            {
                g.setColour (t.buttonHover);
                g.fillRoundedRectangle (r, 12.0f * sf);
            }

            const auto ink = active ? t.waveformBg : (hovered ? t.foreground : t.foreground.withAlpha (0.62f));

            auto iconBox = juce::Rectangle<float> (22.0f * sf, 22.0f * sf)
                               .withCentre ({ r.getCentreX(), r.getY() + 19.0f * sf });
            drawIcon (g, (Item) i, iconBox, ink, sf);

            g.setColour (ink);
            g.setFont (juce::Font (juce::FontOptions (9.5f * sf)));
            g.drawText (labels[i],
                        juce::Rectangle<float> (r.getX(), r.getBottom() - 15.0f * sf, r.getWidth(), 12.0f * sf).toNearestInt(),
                        juce::Justification::centred, false);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = itemAt (e.position);
        if (h != hoverItem) { hoverItem = h; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverItem != -1) { hoverItem = -1; repaint(); }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = itemAt (e.position);
        if (i >= 0 && onItem) onItem ((Item) i);
    }

    juce::String getTooltip() override
    {
        static const char* tips[NumItems] = {
            "Slicer", "Multisampler", "SF2 player", "File browser",
            "Mixer", "Global EQ", "Sequencer", "Settings" };
        return (hoverItem >= 0 && hoverItem < NumItems) ? juce::String (tips[hoverItem]) : juce::String();
    }

private:
    float scale() const noexcept { return (float) getWidth() / (float) kDesignWidth; }

    bool isActive (Item it) const noexcept
    {
        switch (it)
        {
            case Slicer: return currentTab == 0;
            case Multi:  return currentTab == 1;
            case Sf2:    return currentTab == 2;
            default:     return panelActive[it];
        }
    }

    int itemAt (juce::Point<float> p) const
    {
        for (int i = 0; i < NumItems; ++i)
            if (! rects[i].isEmpty() && rects[i].contains (p)) return i;
        return -1;
    }

    static void drawIcon (juce::Graphics& g, Item it, juce::Rectangle<float> r, juce::Colour ink, float sf)
    {
        g.setColour (ink);
        const juce::PathStrokeType stroke (1.7f * sf, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        const float W = r.getWidth(), H = r.getHeight();

        switch (it)
        {
            case Slicer:
            {
                static const float hs[7] = { 0.35f, 0.7f, 1.0f, 0.55f, 0.85f, 0.4f, 0.6f };
                for (int i = 0; i < 7; ++i)
                {
                    const float x = r.getX() + (i + 0.5f) * W / 7.0f;
                    const float h = hs[i] * H;
                    g.drawLine (x, r.getCentreY() - h * 0.5f, x, r.getCentreY() + h * 0.5f, 1.7f * sf);
                }
                break;
            }
            case Multi:
            {
                auto box = r.reduced (0.0f, H * 0.18f);
                g.drawRoundedRectangle (box, 2.5f * sf, 1.6f * sf);
                for (int i = 1; i < 4; ++i)
                {
                    const float x = box.getX() + box.getWidth() * i / 4.0f;
                    g.drawLine (x, box.getY(), x, box.getBottom(), 1.3f * sf);
                }
                break;
            }
            case Sf2:
            {
                g.drawEllipse (r.reduced (1.5f * sf), 1.6f * sf);
                g.fillEllipse (r.withSizeKeepingCentre (W * 0.28f, H * 0.28f));
                break;
            }
            case Browser:
            {
                juce::Path p;
                p.startNewSubPath (r.getX(), r.getY() + 0.2f * H);
                p.lineTo (r.getX() + 0.38f * W, r.getY() + 0.2f * H);
                p.lineTo (r.getX() + 0.48f * W, r.getY() + 0.32f * H);
                p.lineTo (r.getRight(), r.getY() + 0.32f * H);
                p.lineTo (r.getRight(), r.getBottom() - 0.15f * H);
                p.lineTo (r.getX(), r.getBottom() - 0.15f * H);
                p.closeSubPath();
                g.strokePath (p, stroke);
                break;
            }
            case Mixer:
            {
                static const float ks[3] = { 0.3f, 0.65f, 0.42f };
                for (int i = 0; i < 3; ++i)
                {
                    const float x = r.getX() + (i + 0.5f) * W / 3.0f;
                    g.drawLine (x, r.getY(), x, r.getBottom(), 1.5f * sf);
                    g.fillEllipse (juce::Rectangle<float> (W * 0.3f, W * 0.3f)
                                       .withCentre ({ x, r.getY() + ks[i] * H }));
                }
                break;
            }
            case Eq:
            {
                juce::Path p;
                p.startNewSubPath (r.getX(), r.getY() + 0.7f * H);
                p.cubicTo (r.getX() + 0.3f * W, r.getY() + 0.7f * H, r.getX() + 0.3f * W, r.getY() + 0.2f * H,
                           r.getCentreX(), r.getY() + 0.2f * H);
                p.cubicTo (r.getX() + 0.7f * W, r.getY() + 0.2f * H, r.getX() + 0.7f * W, r.getY() + 0.7f * H,
                           r.getRight(), r.getY() + 0.7f * H);
                g.strokePath (p, stroke);
                break;
            }
            case Seq:
            {
                g.fillRoundedRectangle (r.getX(),               r.getY() + 0.12f * H, 0.5f * W, 0.2f * H, 1.5f * sf);
                g.fillRoundedRectangle (r.getX() + 0.28f * W,   r.getY() + 0.40f * H, 0.55f * W, 0.2f * H, 1.5f * sf);
                g.fillRoundedRectangle (r.getX() + 0.10f * W,   r.getY() + 0.68f * H, 0.4f * W, 0.2f * H, 1.5f * sf);
                break;
            }
            case Settings:
            {
                const auto c = r.getCentre();
                g.drawEllipse (r.withSizeKeepingCentre (W * 0.44f, H * 0.44f), 1.6f * sf);
                for (int i = 0; i < 8; ++i)
                {
                    const float a = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
                    g.drawLine (c.x + std::cos (a) * W * 0.36f, c.y + std::sin (a) * H * 0.36f,
                                c.x + std::cos (a) * W * 0.5f,  c.y + std::sin (a) * H * 0.5f, 1.6f * sf);
                }
                break;
            }
            default: break;
        }
    }

    int  currentTab   = 0;
    int  hoverItem    = -1;
    bool seqAvailable = true;
    bool panelActive[NumItems] {};
    float dividerY    = 0.0f;
    juce::Rectangle<float> rects[NumItems];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NavRail)
};
