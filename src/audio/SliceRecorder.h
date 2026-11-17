#pragma once
// =============================================================================
//  SliceRecorder.h  -  record a take (audio input or master output) for the
//  Slicer, with MIDI-key slice markers dropped while recording.
//
//  Threading
//    * start()/stop()/getters are message-thread calls.
//    * pushInput()/pushOutput()/markNow() are audio-thread calls and never
//      allocate or lock. The take buffer is allocated up front in start().
//    * stop() flips `recording` off and waits (briefly) for an in-flight push
//      to finish before it copies the take out.
//
//  Marker timing
//    * Output source: pushOutput() runs at the END of the processor block, so a
//      marker at block offset `o` lands at writePos + o.
//    * Input source: the input tap is registered BEFORE the processor in the
//      device callback chain, so this block's input is already in the buffer by
//      the time MIDI is handled; the marker lands at writePos - blockSize + o.
// =============================================================================
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <atomic>
#include <vector>

class SliceRecorder
{
public:
    enum class Source { Input = 0, Output = 1 };

    static constexpr int    kMaxMarkers = 128;
    static constexpr double kMaxSeconds = 180.0;
    static constexpr int    kPeakBlock  = 512;     // frames per stored peak (live waveform)

    struct Take
    {
        juce::AudioBuffer<float> audio;
        std::vector<int>         markers;
        double                   sampleRate = 0.0;
        int getNumFrames() const noexcept { return audio.getNumSamples(); }
    };

    // ── Message thread ───────────────────────────────────────────────────────
    bool start (Source s, double sr)
    {
        if (recording.load (std::memory_order_acquire) || sr <= 1000.0)
            return false;

        const int frames = (int) (sr * kMaxSeconds);
        buffer.setSize (2, frames, false, true, false);
        peaks.assign ((size_t) (frames / kPeakBlock + 2), 0.0f);
        peakCount.store (0);
        markerCount.store (0);
        writePos.store (0);
        curPeak = 0.0f;
        curFill = 0;
        level.store (0.0f);
        sampleRate = sr;
        source.store ((int) s);
        full.store (false);
        recording.store (true, std::memory_order_release);
        return true;
    }

    Take stop()
    {
        recording.store (false, std::memory_order_release);
        for (int i = 0; i < 200 && busy.load (std::memory_order_acquire); ++i)
            juce::Thread::sleep (1);

        Take t;
        t.sampleRate = sampleRate;
        const int n = juce::jlimit (0, buffer.getNumSamples(), writePos.load (std::memory_order_acquire));
        t.audio.setSize (2, n);
        for (int ch = 0; ch < 2; ++ch)
            t.audio.copyFrom (ch, 0, buffer, ch, 0, n);

        const int mc = juce::jmin (kMaxMarkers, markerCount.load (std::memory_order_acquire));
        for (int i = 0; i < mc; ++i)
            t.markers.push_back (juce::jlimit (0, n, markerPos[i]));
        std::sort (t.markers.begin(), t.markers.end());
        return t;
    }

    bool   isRecording() const noexcept     { return recording.load (std::memory_order_acquire); }
    bool   isFull() const noexcept          { return full.load (std::memory_order_relaxed); }
    int    getFrames() const noexcept       { return writePos.load (std::memory_order_acquire); }
    double getSampleRate() const noexcept   { return sampleRate; }
    Source getSource() const noexcept       { return (Source) source.load(); }
    float  getLevel() const noexcept        { return level.load (std::memory_order_relaxed); }
    int    getNumMarkers() const noexcept   { return juce::jmin (kMaxMarkers, markerCount.load (std::memory_order_acquire)); }

    /** Copies the stored peaks (one per kPeakBlock frames) and marker positions. */
    void snapshot (std::vector<float>& peaksOut, std::vector<int>& markersOut) const
    {
        const int pc = juce::jmin ((int) peaks.size(), peakCount.load (std::memory_order_acquire));
        peaksOut.assign (peaks.begin(), peaks.begin() + pc);
        const int mc = getNumMarkers();
        markersOut.assign (markerPos, markerPos + mc);
    }

    void setInputChannelCount (int n) noexcept { inputChannels.store (n, std::memory_order_relaxed); }
    int  getInputChannelCount() const noexcept { return inputChannels.load (std::memory_order_relaxed); }

    // ── Audio thread ─────────────────────────────────────────────────────────
    void pushInput (const float* const* chans, int numCh, int n) noexcept
    {
        if (source.load (std::memory_order_relaxed) == (int) Source::Input)
            write (chans, numCh, n);
    }

    void pushOutput (const juce::AudioBuffer<float>& b) noexcept
    {
        if (source.load (std::memory_order_relaxed) == (int) Source::Output)
            write (b.getArrayOfReadPointers(), b.getNumChannels(), b.getNumSamples());
    }

    /** Drops a slice marker at block offset `offset` of a block of `blockSize` frames. */
    void markNow (int offset, int blockSize) noexcept
    {
        if (! recording.load (std::memory_order_acquire))
            return;
        const int c = markerCount.load (std::memory_order_relaxed);
        if (c >= kMaxMarkers)
            return;

        int p = writePos.load (std::memory_order_acquire);
        p = (source.load (std::memory_order_relaxed) == (int) Source::Input) ? p - blockSize + offset
                                                                             : p + offset;
        markerPos[c] = juce::jmax (0, p);
        markerCount.store (c + 1, std::memory_order_release);
    }

private:
    void write (const float* const* chans, int numCh, int n) noexcept
    {
        if (numCh <= 0 || n <= 0)
            return;

        struct Busy
        {
            std::atomic<bool>& f;
            explicit Busy (std::atomic<bool>& x) : f (x) { f.store (true, std::memory_order_release); }
            ~Busy() { f.store (false, std::memory_order_release); }
        } busyGuard (busy);

        if (! recording.load (std::memory_order_acquire))
            return;

        const int pos = writePos.load (std::memory_order_relaxed);
        const int num = juce::jmin (n, buffer.getNumSamples() - pos);
        if (num <= 0)
        {
            full.store (true, std::memory_order_relaxed);
            return;
        }

        const float* l = chans[0];
        const float* r = chans[numCh > 1 ? 1 : 0];
        buffer.copyFrom (0, pos, l, num);
        buffer.copyFrom (1, pos, r, num);

        float blockPeak = 0.0f;
        for (int i = 0; i < num; ++i)
        {
            const float a = juce::jmax (std::abs (l[i]), std::abs (r[i]));
            blockPeak = juce::jmax (blockPeak, a);
            curPeak   = juce::jmax (curPeak, a);
            if (++curFill >= kPeakBlock)
            {
                const int pc = peakCount.load (std::memory_order_relaxed);
                if (pc < (int) peaks.size())
                {
                    peaks[(size_t) pc] = curPeak;
                    peakCount.store (pc + 1, std::memory_order_release);
                }
                curPeak = 0.0f;
                curFill = 0;
            }
        }
        level.store (blockPeak, std::memory_order_relaxed);
        writePos.store (pos + num, std::memory_order_release);
    }

    juce::AudioBuffer<float> buffer;
    std::vector<float>       peaks;
    int                      markerPos[kMaxMarkers] {};
    double                   sampleRate = 0.0;
    float                    curPeak = 0.0f;
    int                      curFill = 0;

    std::atomic<bool>  recording { false };
    std::atomic<bool>  busy      { false };
    std::atomic<bool>  full      { false };
    std::atomic<int>   source    { (int) Source::Input };
    std::atomic<int>   writePos  { 0 };
    std::atomic<int>   peakCount { 0 };
    std::atomic<int>   markerCount { 0 };
    std::atomic<float> level     { 0.0f };
    std::atomic<int>   inputChannels { 0 };
};

// Registered with the AudioDeviceManager BEFORE the processor player so this
// block's input is already captured when the processor handles its MIDI.
class SliceInputTap final : public juce::AudioIODeviceCallback
{
public:
    explicit SliceInputTap (SliceRecorder& r) : rec (r) {}

    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override { rec.setInputChannelCount (0); }

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn,
                                           float* const* out, int numOut, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        rec.setInputChannelCount (numIn);
        if (numIn > 0)
            rec.pushInput (in, numIn, numSamples);
        for (int c = 0; c < numOut; ++c)
            if (out[c] != nullptr)
                juce::FloatVectorOperations::clear (out[c], numSamples);
    }

private:
    SliceRecorder& rec;
};
