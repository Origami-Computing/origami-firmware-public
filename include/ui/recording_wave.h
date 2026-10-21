#pragma once
#include "ui/scene.h"
#include "config/ui_theme.h"
#include "core/recording_waveform.h"
#include <math.h>
#include <stdio.h>

namespace ui {
constexpr int kRecordingWaveBars = core::audio::RecordingWaveform::kBars;
inline void recordingWave(scene::Scene& scene, int width, const uint8_t* wave) {
    const int waveW = kRecordingWaveBars * 7 - 3;
    const int left = (width - waveW) / 2;
    scene.addLine("record.baseline", left, 145, left + waveW - 1, 145, kUiFg, 2);
    for (int i = 0; i < kRecordingWaveBars; ++i) {
        const int h = 2 + (int)(72.f * sqrtf(wave[i] / 255.f));
        char id[32]; snprintf(id, sizeof(id), "record.wave.%d", i);
        scene.addRect(id, {(int16_t)(left + i * 7), (int16_t)(145 - h / 2), 4, (int16_t)h},
                      kUiFg, 2, 2);
    }
}
}
