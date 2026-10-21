#include "services/mic_capture.h"

#include <Arduino.h>
#include <Wire.h>
#include <esp_err.h>

#include "Pins.h"
#include "hal/hal_audio.h"

namespace services {
namespace audio {

namespace {
bool startMicSession(bool streaming = false) {
    if (!hal::audio::initMic(streaming)) return false;
    // Drain startup frames (stale/quiet) so PTT feels instant.
    for (int w = 0; w < 10; w++) {
        int32_t dump[128]; size_t br = 0;
        hal::audio::readMic(dump, sizeof(dump), &br, pdMS_TO_TICKS(2));
        if (br == 0 && w > 2) break;
    }
    return true;
}
}  // namespace

void MicCapture::prepare() {
    // The shared HAL owns codec validity. Home re-entry and other audio apps
    // must not cause a redundant 150 ms hardware reset on every PTT press.
    if (hal::audio::codecReady()) { prepared_ = true; return; }
    // Accessory I2C is initialized during board bring-up; restarting it here
    // would disrupt the IMU and any active camera session sharing Wire1.
    hal::audio::hardwareReset();
    delay(40);
    bool ok = hal::audio::initCodec(0x18);
    if (!ok) {
        static const uint8_t kAlts[] = {0x19, 0x1A, 0x1B};
        for (uint8_t a : kAlts) {
            if (hal::audio::initCodec(a)) { ok = true; break; }
        }
    }
    if (ok) hal::audio::configureForRecording();
    else Serial.println("[mic] TLV320 not found — I2S-only path");
    prepared_ = true;
}

bool MicCapture::startStream(SampleSink sink, void* user) {
    waveform_.reset();
    prepare();
    if (!sink || !startMicSession(true)) return false;
    n_         = 0;
    peak_      = 0;
    sink_      = sink;
    sinkUser_  = user;
    streaming_ = true;
    return true;
}

void MicCapture::tick() {
    // Average-decimate each 48 kHz block → 16 kHz: avoids aliasing from naive
    // pick-every-Nth. Gentle digital gain (~1.25×) to avoid clipping noise floor.
    constexpr int32_t kGainN = 5, kGainD = 4;
    constexpr int kMaxBursts = 16;
    int drained = 0;
    while (streaming_ && drained < kMaxBursts) {
        drained++;
        int32_t raw[512]; size_t br = 0;
        if (hal::audio::readMic(raw, sizeof(raw), &br, pdMS_TO_TICKS(0)) != ESP_OK || br == 0) break;
        const int cnt = (int)(br / sizeof(int32_t));
        waveform_.ingestRaw(raw, (size_t)cnt);
        int16_t out[512 / kDS];
        uint32_t outN = 0;
        for (int i = 0; i + (int)kDS <= cnt; i += (int)kDS) {
            int64_t acc = 0;
            for (uint32_t k = 0; k < kDS; k++) acc += (int64_t)((int32_t)(raw[i + (int)k] >> 16));
            int32_t v = (int32_t)(acc / (int64_t)kDS);
            v = (int32_t)(((int64_t)v * kGainN) / kGainD);
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            int32_t a = v; if (a < 0) a = -a;
            if (a > peak_) peak_ = a;
            out[outN++] = (int16_t)v;
            n_++;
        }
        if (streaming_ && outN > 0 && sink_) {
            (void)sink_(out, outN, sinkUser_);
        }
    }
}

void MicCapture::stop() {
    Serial.printf("[mic] %lu samples @16kHz peak=%ld\n", (unsigned long)n_, (long)peak_);
    sink_ = nullptr;
    sinkUser_ = nullptr;
    streaming_ = false;
    hal::audio::endMicSession();
}

}  // namespace audio
}  // namespace services
