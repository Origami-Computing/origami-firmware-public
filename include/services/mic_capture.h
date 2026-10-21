#pragma once

#include <stdint.h>
#include "core/recording_waveform.h"

namespace services {
namespace audio {

/**
 * Microphone capture with 48 kHz → 16 kHz downsampling.
 *
 * Usage:
 *   MicCapture mic;
 *   mic.prepare();          // one-time TLV320 codec init + configure for RX
 *   mic.startStream(sink, user); // init I2S RX and drain startup frames
 *   while (recording) mic.tick(); // burst drain + downsample
 *   mic.stop();             // log peak, endMicSession (I2S → speaker path)
 *   // sink receives 16-bit mono @ 16 kHz
 */
class MicCapture {
public:
    using SampleSink = bool (*)(const int16_t* samples, uint32_t count, void* user);

    static constexpr uint32_t kMicRate = 48000;
    static constexpr uint32_t kOutRate = 16000;
    static constexpr uint32_t kDS      = kMicRate / kOutRate;  // downsample ratio = 3

    /** Init TLV320 audio codec + configure I2S for recording. Idempotent. */
    void prepare();

    /** Init I2S RX, drain startup frames, stream samples to sink. */
    bool startStream(SampleSink sink, void* user);

    /** Burst-drain I2S FIFO and average-decimate 48k → 16k into the sample sink. */
    void tick();

    /** Log peak amplitude and deinit I2S RX. */
    void stop();

    uint32_t samplesN()  const { return n_; }
    bool     isPrepared() const { return prepared_; }
    bool     isStreaming() const { return streaming_; }
    const core::audio::RecordingWaveform& waveform() const { return waveform_; }

private:
    core::audio::RecordingWaveform waveform_;
    uint32_t  n_        = 0;
    int32_t   peak_     = 0;
    SampleSink sink_    = nullptr;
    void*     sinkUser_ = nullptr;
    bool      streaming_ = false;
    bool      prepared_ = false;
};

}  // namespace audio
}  // namespace services
