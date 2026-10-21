#include "services/diagnostic_tone.h"
#include "core/audio_dsp.h"
#include "hal/hal_audio.h"
#include "services/media_volume.h"
#include <Arduino.h>

namespace services { namespace diagnostics {
void Tone::start() {
    hal::audio::hardwareReset();
    delay(50);
    (void)hal::audio::initCodec(0x18);
    (void)hal::audio::configureForPlayback();
    (void)hal::audio::txBegin(44100);
    phase_ = 0.0f;
}
void Tone::writeBlock() {
    if (!hal::audio::txReady()) return;
    int16_t samples[256];
    const float peakGain = hal::audio::getEffectiveOutputRoute() == hal::audio::OutputRoute::HeadphonesOnly
        ? 0.5f : audio::media_volume::kSpeakerMaxGainBasisPoints / 10000.0f;
    core::dsp::generateSine(samples, 256, phase_, 440.0f, 44100.0f, peakGain);
    size_t written = 0;
    (void)hal::audio::txWrite(samples, 256, &written, portMAX_DELAY);
}
void Tone::stop() { hal::audio::txEnd(); }
} }
