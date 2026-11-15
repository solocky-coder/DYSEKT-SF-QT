#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include <vector>
#include <algorithm>

// The standalone app owns one AudioDeviceManager. Some drivers (ASIO, exclusive WASAPI) allow only one
// client, so helpers such as the file-browser preview must play through the app's own device instead of
// opening a second one. Message thread only.
namespace XivaSharedAudio
{
    inline juce::AudioDeviceManager*& manager() { static juce::AudioDeviceManager* m = nullptr; return m; }
    inline std::vector<juce::AudioSourcePlayer*>& players() { static std::vector<juce::AudioSourcePlayer*> v; return v; }

    inline bool available() { return manager() != nullptr && manager()->getCurrentAudioDevice() != nullptr; }

    inline bool isAttached (juce::AudioSourcePlayer* p)
    {
        auto& v = players();
        return std::find (v.begin(), v.end(), p) != v.end();
    }

    inline void attach (juce::AudioSourcePlayer* p)
    {
        if (manager() == nullptr || isAttached (p)) return;
        manager()->addAudioCallback (p);
        players().push_back (p);
    }

    inline void detach (juce::AudioSourcePlayer* p)
    {
        auto& v = players();
        auto it = std::find (v.begin(), v.end(), p);
        if (it == v.end()) return;
        v.erase (it);
        if (manager() != nullptr) manager()->removeAudioCallback (p);
    }

    // Called by the app before its device manager goes away.
    inline void shutdown()
    {
        if (manager() != nullptr)
            for (auto* p : players()) manager()->removeAudioCallback (p);
        players().clear();
        manager() = nullptr;
    }
}
