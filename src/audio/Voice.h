#pragma once
#include "AdsrEnvelope.h"
#include <chowdsp_filters/chowdsp_filters.h>
#include <memory>
#include <vector>

// Forward declare to avoid including heavy header in every TU
namespace signalsmith { namespace stretch {
    template<typename Sample, class RandomEngine> struct SignalsmithStretch;
}}




struct Voice
{
    bool         active       = false;
    int          sliceIdx     = -1;
    double       position     = 0.0;
    double       speed        = 1.0;
    int          direction    = 1;       // 1=forward, -1=reverse
    int          midiNote     = -1;
    float        velocity     = 0.0f;
    AdsrEnvelope envelope;
    int          startSample  = 0;
    int          endSample    = 0;
    bool         pingPong     = false;
    int          muteGroup    = 1;  // default matches slice creation default; always overwritten in startVoice()
    bool         looping      = false;
    float        volume       = 1.0f;
    bool         releaseTail  = false;
    bool         oneShot      = false;
    int          bufferEnd    = 0;       // actual end of sample buffer (for release tail)
    int          outputBus    = 0;       // output bus index (0-15)

    // Pan — constant-power, calculated at note-on
    float        panL         = 1.0f;
    float        panR         = 1.0f;

    // Filter — 2-pole resonant low-pass (TPT state-variable). filterCoeff == 1.0 means bypass.
    float        filterCoeff  = 1.0f;
    float        filterRes    = 0.0f;
    float        filterStateL = 0.0f;   // kept for layout compatibility (unused by the SVF)
    float        filterStateR = 0.0f;
    float        filterA1 = 0.0f, filterA2 = 0.0f, filterA3 = 0.0f;   // TPT coefficients
    float        filterIc1L = 0.0f, filterIc2L = 0.0f, filterIc1R = 0.0f, filterIc2R = 0.0f;

    // ── Per-slice 3-band EQ (chowdsp SVF filters) ────────────────────────────
    // Prepared and reset at note-on. processSample(ch, x) is called per sample.
    chowdsp::SVFLowShelf<float>  eqLowShelf;
    chowdsp::SVFBell<float>      eqMid;
    chowdsp::SVFHighShelf<float> eqHighShelf;
    bool         eqActive  = false;  // true when at least one band is non-zero

    // Signalsmith stretch fields
    bool         stretchActive = false;
    std::shared_ptr<signalsmith::stretch::SignalsmithStretch<float, void>> stretcher;
    double       stretcherCfgSr = 0.0;   // sample rate the stretcher was configured for (0 = not yet)
    std::vector<float> stretchInBufL, stretchInBufR;
    std::vector<float> stretchOutBufL, stretchOutBufR;
    int          stretchOutReadPos  = 0;
    int          stretchOutAvail    = 0;
    double       stretchSrcPos      = 0.0;
    float        stretchTimeRatio   = 1.0f;
    float        stretchPitchSemis  = 0.0f;


    bool         startedViaChromaticLegato = false; // used by legato voice stealing
};
