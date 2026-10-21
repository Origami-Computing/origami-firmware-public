#pragma once
/**
 * @file core/audio_dsp.h
 * @brief Lightweight audio DSP primitives (header-only).
 *
 * Shared across apps that produce or process PCM audio: MP3 player,
 * voice recorder, hardware test tone, future synth apps.
 */
#include <stdint.h>
#include <cmath>
#include "core/pcm_ring.h"

namespace core {
namespace dsp {

// Scale a PCM sample by gain in basis points: 10000 = 100%, 100 = 1%, 1 = 0.01%.
inline int16_t applyVolumeBasisPoints(int16_t sample, int gainBasisPoints) {
    int32_t v = ((int32_t)sample * gainBasisPoints) / 10000;
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// Extract peak amplitude from a mono PCM buffer and map to 0..255 with a
// nonlinear knee that lifts quiet speech so it remains visible on a VU meter.
// kneeThreshold (default 70) is the scaled value below which gain is boosted.
inline uint8_t peakLevel(const int16_t* pcm, int n, int kneeThreshold = 70) {
    if (!pcm || n <= 0) return 0;
    int32_t pk = 0;
    for (int i = 0; i < n; i++) {
        int32_t v = pcm[i];
        if (v < 0) v = -v;
        if (v > pk) pk = v;
    }
    uint32_t scaled = (uint32_t)((pk * 255ull) / 32768ull);
    if (scaled > 255) scaled = 255;
    if (scaled < (uint32_t)kneeThreshold)
        scaled = (scaled * 130u) / (uint32_t)kneeThreshold;
    if (scaled > 255) scaled = 255;
    return (uint8_t)scaled;
}

// Sine wave generator: write `stereoCount` int16_t stereo-interleaved samples
// (L, R identical) into buf.  `phase` is updated in-place and wraps at 2π.
// peakGain is a fraction of full scale, defaulting to 50% for legacy callers.
// stereoCount must be even (each pair is one stereo frame).
inline void generateSine(int16_t* buf, int stereoCount,
                         float& phase, float hz, float sampleRate, float peakGain = 0.5f) {
    const float step = 2.0f * (float)M_PI * hz / sampleRate;
    const float amp  = peakGain * 32767.0f;
    for (int i = 0; i < stereoCount; i += 2) {
        int16_t v = (int16_t)(sinf(phase) * amp);
        buf[i]   = v;
        buf[i+1] = v;
        phase += step;
        if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    }
}

// Push `samples` decoded PCM frames into a PcmRing as stereo-interleaved pairs,
// applying software volume.  If channels == 1 the sample is duplicated L+R.
// If channels == 2 the source is already interleaved (L0,R0,L1,R1,...).
// The caller must check ring.free() >= samples*2 before calling.
inline void pushStereoWithVolumeBasisPoints(const int16_t* decoded, int samples,
                                            int channels, int gainBasisPoints,
                                            PcmRing& ring) {
    if (channels == 1) {
        for (int i = 0; i < samples; i++) {
            int16_t s       = applyVolumeBasisPoints(decoded[i], gainBasisPoints);
            int16_t pair[2] = { s, s };
            ring.push(pair, 2);
        }
    } else {
        for (int i = 0; i < samples; i++) {
            int16_t l       = applyVolumeBasisPoints(decoded[i * 2],     gainBasisPoints);
            int16_t r       = applyVolumeBasisPoints(decoded[i * 2 + 1], gainBasisPoints);
            int16_t pair[2] = { l, r };
            ring.push(pair, 2);
        }
    }
}

}  // namespace dsp
}  // namespace core
