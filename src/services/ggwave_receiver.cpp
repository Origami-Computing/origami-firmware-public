#include "services/ggwave_receiver.h"
#include "services/peripheral_reservation.h"
#include "hal/hal_audio.h"
#include "hal/hal_tasks.h"
#include "hal/hal_wifi.h"
#include <ggwave/ggwave.h>
#include <Arduino.h>
#include <atomic>
#include <new>
#include <string.h>
#include <math.h>
#include <esp_heap_caps.h>

namespace services { namespace ggwave_receiver {
namespace {
background_network::PeripheralReservation reservation;
GGWave* decoder = nullptr;
struct Scratch { int32_t raw[512]; int16_t pcm[1024]; };
Scratch* scratch = nullptr;
std::atomic<bool> running{false}, cancel{false};
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
Snapshot state;
bool ownsMic = false;
bool ownsRadioPause = false;
bool sensitiveMode = false;
char pendingPayload[141] = {};
const char* lastError = "";

void worker(void*) {
    auto& raw = scratch->raw;
    // Upstream 0.4.3 does not accumulate partial frames correctly without
    // resampling. Feed complete 1024-sample frames, preserving every HAL sample.
    auto& pcm = scratch->pcm;
    size_t buffered = 0;
    bool wasReceiving = false;
    float lowDb = -120, midDb = -120, highDb = -120;
    while (!cancel.load()) {
        size_t bytes = 0;
        const auto result = hal::audio::readMic(raw, sizeof(raw), &bytes, pdMS_TO_TICKS(10));
        if (result != ESP_OK || !bytes) { vTaskDelay(1); continue; }
        const size_t count = bytes / sizeof(raw[0]);
        uint16_t peak = 0;
        uint32_t clipped = 0, starts = 0, decodeUs = 0;
        for (size_t i = 0; i < count; ++i) {
            const int16_t value = static_cast<int16_t>(raw[i] >> 16);
            pcm[buffered++] = value;
            const uint16_t magnitude = value < 0 ? -int(value) : value;
            if (magnitude > peak) peak = magnitude;
            if (magnitude >= 32760) ++clipped;
            if (buffered == 1024) {
                const uint32_t began = micros();
                decoder->decode(pcm, sizeof(pcm));
                decodeUs = micros() - began;
                const bool receiving = decoder->rxReceiving();
                if (receiving && !wasReceiving) ++starts;
                wasReceiving = receiving;
                const auto& spectrum = decoder->rxSpectrum();
                float low = 0, mid = 0, high = 0;
                for (int bin = 6; bin <= 170; ++bin) low += spectrum[bin];
                for (int bin = 192; bin < 288; ++bin) mid += spectrum[bin];
                for (int bin = 320; bin < 416; ++bin) high += spectrum[bin];
                lowDb = 10*log10f(low/(1024*1024) + 1e-12f);
                midDb = 10*log10f(mid/(1024*1024) + 1e-12f);
                highDb = 10*log10f(high/(1024*1024) + 1e-12f);
                buffered = 0;
            }
        }
        const int length = decoder->rxDataLength();
        const bool receiving = decoder->rxReceiving() || decoder->rxAnalyzing();
        portENTER_CRITICAL(&mutex);
        state.samples += count;
        state.peak = peak;
        state.receiving = receiving;
        state.starts += starts;
        state.clipped += clipped;
        if (decodeUs > state.maxDecodeUs) state.maxDecodeUs = decodeUs;
        state.lowDb = lowDb; state.midDb = midDb; state.highDb = highDb;
        if (length < 0) ++state.failures;
        if (length > 0) {
            const int n = length > 140 ? 140 : length;
            char* destination = sensitiveMode ? pendingPayload : state.text;
            for (int i = 0; i < n; ++i) {
                const unsigned char ch = decoder->rxData()[i];
                destination[i] = ch >= 32 && ch <= 126 ? char(ch) : ' ';
            }
            destination[n] = 0;
            ++state.messages;
            state.protocol = int(decoder->rxProtocolId());
        }
        portEXIT_CRITICAL(&mutex);
        if (length != 0) {
            GGWave::TxRxData ignored;
            decoder->rxTakeData(ignored);
        }
        vTaskDelay(1);
    }
    hal::tasks::recordCurrentStack(hal::tasks::Id::GgWave);
    running.store(false); // No session data or hardware access after release.
    hal::tasks::deleteCurrent(hal::tasks::Id::GgWave);
}
}

Snapshot snapshot() {
    portENTER_CRITICAL(&mutex);
    const Snapshot copy = state;
    portEXIT_CRITICAL(&mutex);
    return copy;
}
const char* error() { return lastError; }
bool takePayload(char* out, unsigned capacity) {
    if (!out || capacity < sizeof(pendingPayload)) return false;
    portENTER_CRITICAL(&mutex);
    const bool available = pendingPayload[0] != 0;
    if (available) memcpy(out, pendingPayload, sizeof(pendingPayload));
    memset(pendingPayload, 0, sizeof(pendingPayload));
    portEXIT_CRITICAL(&mutex);
    return available;
}
void stop() {
    cancel.store(true);
    while (running.load()) vTaskDelay(1);
    if (ownsMic) { hal::audio::endMicSession(); ownsMic = false; }
    delete decoder;
    decoder = nullptr;
    heap_caps_free(scratch);
    scratch = nullptr;
    if (ownsRadioPause) {
        ownsRadioPause = false;
        hal::wifi::restore();
    }
    reservation.release();
    portENTER_CRITICAL(&mutex);
    state.listening = false;
    state.receiving = false;
    memset(pendingPayload, 0, sizeof(pendingPayload));
    if (sensitiveMode) memset(state.text, 0, sizeof(state.text));
    portEXIT_CRITICAL(&mutex);
}
bool start(bool sensitive) {
    if (running.load() && sensitiveMode == sensitive) return true;
    stop();
    sensitiveMode = sensitive;
    if (sensitive) { portENTER_CRITICAL(&mutex); memset(state.text,0,sizeof(state.text)); portEXIT_CRITICAL(&mutex); }
    lastError = "";
    if (reservation.acquire(true) != background_network::PeripheralReservation::Result::Acquired) {
        lastError = "Audio busy. Press C to retry.";
        return false;
    }
    // Wi-Fi provisioning receives credentials offline. Pausing only socket
    // workers leaves radio/DMA pools allocated and can starve microphone DMA
    // after sustained networking. Restore only the pause this session owns.
    if (!hal::wifi::suspended()) {
        hal::wifi::stop();
        ownsRadioPause = true;
    }
    GGWave::setLogFile(nullptr);
    GGWave::Protocols::rx().disableAll();
    GGWave::Protocols::rx().toggle(GGWAVE_PROTOCOL_AUDIBLE_NORMAL, true);
    GGWave::Protocols::rx().toggle(GGWAVE_PROTOCOL_AUDIBLE_FAST, true);
    GGWave::Protocols::rx().toggle(GGWAVE_PROTOCOL_AUDIBLE_FASTEST, true);
    GGWave::Protocols::rx().toggle(GGWAVE_PROTOCOL_ULTRASOUND_NORMAL, true);
    GGWave::Protocols::rx().toggle(GGWAVE_PROTOCOL_ULTRASOUND_FAST, true);
    GGWave::Protocols::rx().toggle(GGWAVE_PROTOCOL_ULTRASOUND_FASTEST, true);
    // Same six-tone protocol at 9–13.45 kHz: above most speech, with more
    // laptop-speaker bandwidth margin than the standard 15–19.45 kHz band.
    for (int i = 0; i < 3; ++i) {
        auto& custom = GGWave::Protocols::rx()[GGWave::ProtocolId(GGWAVE_PROTOCOL_CUSTOM_0+i)];
        custom = GGWave::Protocols::rx()[GGWave::ProtocolId(GGWAVE_PROTOCOL_ULTRASOUND_NORMAL+i)];
        custom.freqStart = 192;
    }
    auto p = GGWave::getDefaultParameters();
    p.sampleRateInp = 48000;
    p.sampleFormatInp = GGWAVE_SAMPLE_FORMAT_I16;
    p.operatingMode = GGWAVE_OPERATING_MODE_RX;
    decoder = new (std::nothrow) GGWave;
    if (!decoder || !decoder->prepare(p)) {
        lastError = "Decoder memory unavailable.";
        stop(); return false;
    }
    scratch = static_cast<Scratch*>(heap_caps_calloc(1, sizeof(Scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!scratch) {
        lastError = "Audio buffer unavailable.";
        stop(); return false;
    }
    if (!hal::audio::codecReady()) {
        hal::audio::hardwareReset();
        if (!hal::audio::initCodec()) {
            lastError = "Microphone codec unavailable.";
            stop(); return false;
        }
    }
    ownsMic = true; // Unwind partial init through the same HAL owner.
    if (!hal::audio::initMic(true)) {
        lastError = "Microphone busy. C to retry.";
        stop(); return false;
    }
    portENTER_CRITICAL(&mutex);
    state.listening = true;
    state.receiving = false;
    state.samples = 0;
    state.peak = 0;
    state.starts = state.clipped = state.maxDecodeUs = 0;
    portEXIT_CRITICAL(&mutex);
    cancel.store(false);
    running.store(true);
    if (!hal::tasks::spawn(hal::tasks::Id::GgWave, worker)) {
        running.store(false);
        lastError = "Worker memory unavailable.";
        stop(); return false;
    }
    Serial.printf("[ggwave] listening rate=48000 arena=%d\n", decoder->heapSize());
    return true;
}
} }
