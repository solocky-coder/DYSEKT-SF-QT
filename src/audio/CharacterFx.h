#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

// CharacterFx - lo-fi / vintage-converter colour for the master bus (runs before the Global EQ).
//
// Modes (see GlobalEqPanel "CHARACTER" strip):
//   0 Off
//   1 Bit Crush            p1 = bit depth (16 -> 4)         p2 = dither noise
//   2 Smooth Downsample    p1 = sample rate (full -> ~4 %)  p2 = smoothing filter
//   3 Gritty Lo-Fi         p1 = sample rate                 p2 = grit (saturation, fewer bits, noise)
//   4 12-Bit Drum Sampler  p1 = converter drive             p2 = filter tone
//        fixed 26.04 kHz sample-and-hold, 12-bit linear quantiser, anti-alias and
//        reconstruction low-pass filters. An approximation of a 12-bit drum sampler
//        signal path, not a circuit model.
//
// The decimation and quantisation ideas follow the approach of Airwindows' public-domain-style
// "Bitcrush"/"DeRez" plug-ins (MIT, Chris Johnson); this code is an independent implementation.
class CharacterFx
{
public:
    static constexpr int kNumModes = 5;

    void prepare (double sampleRate)
    {
        sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        reset();
    }

    void reset()
    {
        for (auto& c : ch) c = Chan {};
        noise = 22222u;
    }

    // buffers are processed in place; mix 0..1
    void process (float* l, float* r, int n, int mode, float p1, float p2, float mix)
    {
        if (mode != lastMode) { reset(); lastMode = mode; }
        if (mode <= 0 || mix <= 0.0001f || n <= 0) return;
        p1 = std::min (1.0f, std::max (0.0f, p1));
        p2 = std::min (1.0f, std::max (0.0f, p2));
        mix = std::min (1.0f, mix);

        float* bufs[2] = { l, r };
        for (int c = 0; c < 2; ++c)
        {
            if (bufs[c] == nullptr || (c == 1 && r == l)) continue;
            auto& s = ch[c];
            float* d = bufs[c];
            for (int i = 0; i < n; ++i)
            {
                const float dry = d[i];
                float wet = dry;
                switch (mode)
                {
                    case 1: wet = bitCrush (s, dry, p1, p2); break;
                    case 2: wet = smoothDown (s, dry, p1, p2); break;
                    case 3: wet = grittyLoFi (s, dry, p1, p2); break;
                    case 4: wet = drumSampler (s, dry, p1, p2); break;
                    default: break;
                }
                d[i] = dry + (wet - dry) * mix;
            }
        }
    }

private:
    struct Svf
    {
        float ic1 = 0, ic2 = 0;
        float tick (float x, float a1, float a2, float a3) noexcept
        {
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2 * v1 - ic1;
            ic2 = 2 * v2 - ic2;
            return v2;
        }
    };
    struct Chan
    {
        double phase = 0.0;
        float held = 0, prev = 0, next = 0, lp = 0;
        Svf aa1, aa2, rc1, rc2;
    };

    static constexpr float kPi = 3.14159265358979f;

    float rnd() noexcept            // white noise -1..1
    {
        noise = noise * 1664525u + 1013904223u;
        return (float) (int32_t) noise * (1.0f / 2147483648.0f);
    }
    static float quant (float x, float levels) noexcept { return std::round (x * levels) / levels; }

    void coeffs (float fc, float& a1, float& a2, float& a3) const noexcept
    {
        fc = std::min (fc, (float) sr * 0.45f);
        const float g = std::tan (kPi * fc / (float) sr);
        const float k = 1.41421356f;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    float bitCrush (Chan&, float x, float p1, float p2) noexcept
    {
        const float bits   = 16.0f - p1 * 12.0f;
        const float levels = std::exp2 (bits - 1.0f);
        const float dither = (rnd() + rnd()) * 0.5f * p2 / levels;
        return quant (x + dither, levels);
    }

    float smoothDown (Chan& s, float x, float p1, float p2) noexcept
    {
        const double ratio = std::pow (0.04, (double) p1);   // target rate / sr
        s.phase += ratio;
        if (s.phase >= 1.0) { s.phase -= 1.0; s.prev = s.next; s.next = x; }
        const float t = (float) std::min (1.0, s.phase);
        float y = s.prev + (s.next - s.prev) * t;
        if (p2 > 0.01f)
        {
            const float fc = (float) (ratio * sr) * (0.5f - 0.3f * p2);
            const float a = 1.0f - std::exp (-2.0f * kPi * std::max (20.0f, fc) / (float) sr);
            s.lp += a * (y - s.lp);
            y = s.lp;
        }
        return y;
    }

    float grittyLoFi (Chan& s, float x, float p1, float p2) noexcept
    {
        const double ratio = std::pow (0.08, (double) p1);
        s.phase += ratio;
        if (s.phase >= 1.0)
        {
            s.phase -= 1.0;
            const float drive = 1.0f + p2 * 6.0f;
            float v = std::tanh (x * drive) / std::tanh (drive);
            const float bits = 12.0f - p2 * 6.0f;
            v = quant (v + rnd() * p2 * 0.004f, std::exp2 (bits - 1.0f));
            s.held = v;
        }
        return s.held;
    }

    float drumSampler (Chan& s, float x, float p1, float p2) noexcept
    {
        const float fc = 6000.0f + p2 * 7000.0f;
        float a1, a2, a3;
        coeffs (fc, a1, a2, a3);

        const float drive = 1.0f + p1 * 3.0f;
        float in = std::tanh (x * drive);                             // soft converter clip

        in = s.aa2.tick (s.aa1.tick (in, a1, a2, a3), a1, a2, a3);   // anti-alias (4-pole)

        s.phase += 26040.0 / sr;
        if (s.phase >= 1.0)
        {
            s.phase -= 1.0;
            s.held = quant (in, 2048.0f);                             // 12-bit linear
        }
        const float out = s.rc2.tick (s.rc1.tick (s.held, a1, a2, a3), a1, a2, a3);   // reconstruction
        return out / std::sqrt (drive);
    }

    double sr = 44100.0;
    Chan ch[2];
    uint32_t noise = 22222u;
    int lastMode = 0;
};
