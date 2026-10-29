#pragma once
#include "SequencerTrack.h"
#include "AbletonLink.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <vector>

enum class LiveTargetPlayer { none, slicer, sf2, sfz, network };
struct SelectedLiveTarget { LiveTargetPlayer player = LiveTargetPlayer::none; int midiChannel = 0; };

struct SequencerTrackInfo
{
    TrackType type = TrackType::MainSlice;
    bool enabled = true; bool solo = false; bool audioRecordArm = true; bool monitor = true; float volumeDb = 0.0f; float pan = 0.0f;
    juce::String name; juce::Colour colour = juce::Colour (0xFF3A6080);
    int sliceIdx = -1; int midiChannel = 0; Sf2PresetInfo preset; int numClips = 0;
    bool isSfzInstrument = false;
    bool isSfzLayer = false; int layerLoKey = 0; int layerHiKey = 127; bool layerShown = false;
    int64_t networkRouteId = 0; int32_t networkSourceId = 0; int networkSourceChannel = 0;
    int linkedMidiDeviceId=0; int networkMidiDeviceId=0; bool networkMidiIsChild=false; juce::String networkMidiPeer;
    juce::String midiOutPort;   // Network MIDI tracks: physical output port name ("" = network)
};

/** Name shown in the arranger. Network audio tracks are stored as
    "<device> | <source> <channel>" (kept that way so device re-labelling can find them),
    but only the device name is shown to the user. */
inline juce::String arrangerTrackName (const SequencerTrackInfo& info)
{
    if (info.type == TrackType::Audio)
    {
        const int sep = info.name.indexOf (" | ");
        if (sep > 0) return info.name.substring (0, sep);
    }
    return info.name;
}

struct SequencerClipInfo
{
    int64_t startTick = 0; int64_t lengthTicks = MidiClip::kPPQ * 4 * 4;
    int64_t endTick() const noexcept { return startTick + lengthTicks; }
};

class SequencerEngine
{
public:
    SequencerEngine(); ~SequencerEngine();
    bool isPlaying() const noexcept; bool isLooping() const noexcept; bool isRecording() const noexcept;
    int getCountInBars() const noexcept; void setCountInBars (int bars) noexcept;
    /** Record latency compensation. Recorded notes / audio takes are moved earlier by
        (auto ? deviceLatencyMs : 0) + manualMs so they line up with what was heard. */
    void  setRecordOffsetMs (float ms) noexcept;          float getRecordOffsetMs() const noexcept;
    void  setRecordOffsetAuto (bool on) noexcept;         bool  isRecordOffsetAuto() const noexcept;
    void  setDeviceLatencyMs (float ms) noexcept;         float getDeviceLatencyMs() const noexcept;
    float getEffectiveRecordOffsetMs() const noexcept;
    /** Count-in length before a recording take: 0 = off, 1 or 2 bars. */
    /** Click while playing / recording (separate from the count-in). Volume 0..1.
        clickOutput: 0 = main mix, 1..8 = extra output pair "Out 2".."Out 9". */
    void setMetronomeEnabled (bool on) noexcept; bool isMetronomeEnabled() const noexcept;
    void setMetronomeVolume (float v) noexcept;  float getMetronomeVolume() const noexcept;
    void setClickOutput (int bus) noexcept;      int   getClickOutput() const noexcept;
    int64_t getPlayheadTick() const noexcept; double getPlayheadBeats() const noexcept; float getBpm() const noexcept; bool getSyncToHost() const noexcept;
    int64_t getLengthTicks() const noexcept; int64_t getLoopStartTick() const noexcept; int64_t getLoopEndTick() const noexcept;
    void play(); void stop(); void rewind(); void setLooping (bool v); void setRecording (bool v);
    enum class RecordMode { Overdub, Add }; void setRecordMode (RecordMode m) noexcept; RecordMode getRecordMode() const noexcept;
    void setSyncToHost(bool v); void setBpm(float b); void setHostBpm(float b); void seekToTick(int64_t tick); void setLoopRange(int64_t startTick, int64_t endTick); void resetLoopRangeToDefault(); void setLengthTicks(int64_t ticks);

    /** Show/hide an SFZ layer child track's Arranger row. No-op for any other track. Message thread. */
    void setSfzLayerShown(int trackIndex, bool shown);
    int getNumTracks() const; SequencerTrackInfo getTrackInfo(int i) const; void setTrackEnabled(int i, bool enabled); void setTrackSolo(int i, bool solo);
    // Audio record arm: per track, multi-arm. setAll* touches Audio tracks only.
    void setAudioRecordArm(int i, bool armed); void setAllAudioRecordArm(bool armed);
    void setTrackVolumeDb(int i, float volumeDb); void setTrackPan(int i, float pan); void setTrackColour(int i, juce::Colour colour);
    int findSfTrackForChannel(int midiChannel0Based) const;
    void addMainTrack(); void addChromaticTrack(int sliceIdx, int chromaticChannel, const juce::String& name, juce::Colour colour); void removeChromaticTrack(int sliceIdx);
    void addSfTrack(const Sf2PresetInfo& preset, juce::Colour colour); void removeSfTrack(int trackIndex); void rebuildSfTracks(const std::vector<Sf2PresetInfo>& presets, const juce::Colour* palette, int paletteSize);
    void addOrUpdateSfTrackOnChannel(const Sf2PresetInfo& preset, int midiChannel0Based, juce::Colour colour);
    void addSfzTrack(const juce::String& name, int midiChannel0Based, juce::Colour colour); void removeSfzTrack();
    /** Makes the SFZ instrument track's child tracks match the given layers (one child per
        layer, in note order, nested under the SFZ track), within the 64-track project budget.
        Non-layer tracks and clip-bearing layers take priority; stale clip-bearing layers are
        retained rather than losing MIDI. droppedCount reports incoming layers omitted by the
        project budget. Does nothing if there is no SFZ instrument track. Message thread.
        Returns the number of current layer tracks. */
    int syncSfzLayerTracks(const std::vector<SfzLayer>& layers, int* droppedCount = nullptr);

    int addNetworkAudioTrack(int64_t routeId, int32_t sourceId, int sourceChannel, const juce::String& sourceName, const juce::String& userName = {});
    bool setNetworkAudioTrackRoute(int trackIndex, int64_t routeId, int32_t sourceId, int sourceChannel);
    bool isNetworkAudioTrack(int trackIndex) const noexcept;
    bool getNetworkAudioRoute(int trackIndex, int64_t& routeId, int32_t& sourceId, int& sourceChannel) const noexcept;
    // Renames every Audio track routed to sourceKey whose name still starts with
    // "<oldLabel> | " (the auto-generated name) so it starts with newLabel instead.
    // Returns the number of tracks renamed. Message thread.
    int renameNetworkAudioTracks(int64_t sourceKey, const juce::String& oldLabel, const juce::String& newLabel);

    // Per-device (per network source) controls. A device is identified by its sourceKey
    // and acts on every Audio track routed from it. Message thread.
    struct NetworkSourceState
    {
        int numTracks = 0;          // tracks routed from this source (0 = nothing to control yet)
        float gainDb = 0.0f;        // gain of the first track (they are normally kept equal)
        bool muted = false;         // every track muted
        bool solo = false;          // any track soloed
        bool recordArmed = false;   // every track armed
        bool monitor = true;        // every track audible on the output
    };
    NetworkSourceState getNetworkSourceState(int64_t sourceKey) const;
    void setNetworkSourceGainDb(int64_t sourceKey, float gainDb);
    void setNetworkSourceMuted(int64_t sourceKey, bool muted);
    void setNetworkSourceSolo(int64_t sourceKey, bool solo);
    void setNetworkSourceRecordArm(int64_t sourceKey, bool armed);
    void setNetworkSourceMonitor(int64_t sourceKey, bool monitor);

    static constexpr int kMaxNetworkMidiChildren=16;
    using NetworkMidiSink=void(*)(void*,int,const juce::MidiMessage&);
    void setNetworkMidiSink(NetworkMidiSink,void*) noexcept;
    /** Lets the host tell the engine which IP a network audio source lives at, so MIDI
        children of that audio track start at the audio device's own address. */
    using AudioPeerResolver=juce::String(*)(void*,int64_t sourceKey,int32_t sourceId);
    void setNetworkAudioPeerResolver(AudioPeerResolver,void*) noexcept;
    void sendNetworkMidi(int,const juce::MidiMessage&) const noexcept;
    /** Physical MIDI output ports. The host installs a sink that queues the message for the
        named port (called on the audio thread, so it must not block). */
    using MidiPortSink=void(*)(void*,const juce::String& portName,const juce::MidiMessage&);
    void setMidiPortSink(MidiPortSink,void*) noexcept;
    /** Network MIDI track -> physical port name ("" = send over the network). */
    bool setNetworkMidiOutPort(int trackIndex,const juce::String& portName);
    juce::String getNetworkMidiOutPort(int trackIndex) const;
    /** Live MIDI for the selected Network MIDI track: goes to its port, or to its network device. */
    void sendLiveNetworkMidi(const juce::MidiMessage&) const noexcept;
    /** Standalone: the host forwards live MIDI input to the selected Network MIDI track straight
        from the MIDI input thread (no wait for the next audio block). While set, processBlock no
        longer forwards it, so nothing is sent twice. */
    void setLiveDirectForward (bool on) noexcept;
    bool isLiveDirectForward() const noexcept;
    /** Called from the MIDI input thread: re-stamps channel 1 onto the selected track's channel
        and sends to its destination. Does nothing unless a Network MIDI track is selected. */
    void forwardLiveMidi (const juce::MidiMessage&) const noexcept;
    int addNetworkMidiTrack(const juce::String&,const juce::String&);
    int addNetworkMidiChild(int,int=-1);
    bool removeNetworkMidiTrack(int);
    bool setNetworkMidiChannel(int,int);
    bool setNetworkMidiPeer(int,const juce::String&);
    juce::String getNetworkMidiPeer(int) const;
    bool isNetworkMidiTrack(int) const noexcept;
    int getNetworkMidiChildCount(int) const;
    int findNetworkMidiDevice(const juce::String&) const;
    /** Network audio track = parent. Adds ONE Network MIDI child track on that audio
        device (lowest free MIDI channel; the channel can be changed 1-16 in the child's
        inspector). Returns the new child's track index, or -1. */
    int addNetworkMidiChildForAudio(int audioTrackIndex, const juce::String& defaultPeer);
    /** Number of MIDI children on the device linked to this audio track (0 if unlinked). */
    int getLinkedNetworkMidiChildCount(int audioTrackIndex) const;
    /** Per-track nesting for the track list. For a MIDI child of a network audio track:
        parentIndex = that audio track's index, deviceId = the shared device id. For an
        audio parent: deviceId = its linked device id, childCount = number of children.
        Everything else is parentIndex -1, deviceId 0, childCount 0. */
    /** hidden = an SFZ layer child track the user hasn't opted into showing (and that holds no clips):
        it gets no Arranger row. childCount counts only the children that are not hidden. */
    struct TrackNest { int parentIndex = -1; int deviceId = 0; int childCount = 0; bool hidden = false; };
    std::vector<TrackNest> getTrackNesting() const;
    void setNetworkMidiLinkState(int,int) const noexcept;
    int getNetworkMidiLinkState(int) const noexcept;
    int getSelectedNetworkMidiDevice() const noexcept;

    // True while at least one track (of any type) is soloed.
    bool isAnyTrackSoloed() const noexcept;

    //==========================================================================
    // Audio clips
    int getNumAudioClips(int trackIndex) const;
    AudioClip getAudioClip(int trackIndex, int clipIndex) const;
    int addAudioClip(int trackIndex, const AudioClip& clip);
    bool removeAudioClip(int trackIndex, int clipIndex);
    bool setAudioClipStartTick(int trackIndex, int clipIndex, int64_t newStartTick);

    // Attach a completed network recording to its destination Audio Track.
    bool addRecordedAudioClip(int trackIndex,
                              const juce::File& file,
                              double sampleRate,
                              int channels,
                              int64_t startTick,
                              int64_t lengthSamples);

    int getNumClips(int trackIndex) const; SequencerClipInfo getClipInfo(int trackIndex, int clipIndex) const; MidiClip* getClip(int trackIndex, int clipIndex = 0); MidiClip& getClip();
    int addClip(int trackIndex, int64_t startTick, int64_t lengthTicks = MidiClip::kPPQ * 4 * 4); void removeClip(int trackIndex, int clipIndex); void setClipStartTick(int trackIndex, int clipIndex, int64_t newStartTick); void setClipLengthTicks(int trackIndex, int clipIndex, int64_t newLength);
    int64_t getTrackLengthTicks(int trackIndex) const noexcept; void setTrackLengthTicks(int trackIndex, int64_t ticks);

    void processBlock(juce::MidiBuffer& outMidi, const juce::MidiBuffer& inMidi, int numSamples, double sampleRate);
    void writeToStream(juce::MemoryOutputStream& s) const; bool readFromStream(juce::MemoryInputStream& s);
    void setAbletonLink(AbletonLink* l) noexcept; void setLinkFollowsTransport(bool shouldFollow) noexcept; bool getLinkFollowsTransport() const noexcept;
    void setSfzPlayer(class SfzPlayer* player) noexcept; void setSelectedSfLiveChannels(uint16_t channelMask) noexcept; uint16_t getAllSfPlayerChannelMask() const noexcept;
    void setSelectedLiveChannel(int ch1Based) noexcept; int getSelectedLiveChannel() const noexcept; void setSelectedTrack(int trackIndex) noexcept; SelectedLiveTarget getSelectedLiveTarget() const noexcept;
    /** Note range live input may play on the selected track: 0..127 normally, the zone's range
        when an SFZ layer track is selected. Audio thread safe. */
    void getSelectedLiveNoteRange (int& lo, int& hi) const noexcept; uint16_t getSfzInstrumentChannelMask() const noexcept;
    void setRecordingTrack(int trackIndex) noexcept; int getRecordingTrackIndex() const noexcept; void drainRecordedEvents();
    static constexpr int kActivityFlagCount = 64; bool getMidiActivityAndClear(int trackIndex) noexcept;

    /** Zone-highlight feed. The SFZ-PLAYER / MULTISAMPLER note path (DysektProcessor::processMidi2)
        reports every note it receives so the Arranger can light up the SFZ layer (keyzone) child
        track whose note range contains the played key. Audio thread safe (lock-free atomics). */
    void notifySfzNoteOn (int note) noexcept; void notifySfzNoteOff (int note) noexcept; void notifySfzAllNotesOff() noexcept;
    /** Message thread. `played` = notes struck since the previous call (latched, so a note shorter
        than a UI frame still registers; cleared by this call); `held` = notes currently down.
        Bit n of word 0 = note n (0..63), of word 1 = note n+64. */
    void pollSfzNotes (uint64_t (&played)[2], uint64_t (&held)[2]) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SequencerEngine)
};
