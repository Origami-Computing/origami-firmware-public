#include "hal/audio_retirement.h"
#include "services/agent_voice.h"
#include "services/agent_transport.h"
#include <Arduino.h>
#include <atomic>
#include "core/agent_slab.h"
#include "services/background_network.h"
#include "core/memory.h"
#include "core/menu_config.h"
#include "hal/hal_audio.h"
#include "hal/hal_ble.h"
#include "hal/hal_wifi.h"
namespace services { namespace agent_voice {
namespace {
core::AgentSlab slab;
services::audio::MicCapture mic;
bool capturing=false;
std::atomic<bool> releaseRequested{false};
bool s_held = false;
bool s_blePaused = false;

bool acquireResources() {
    if (s_held) return true;

    const auto suspension = services::background_network::suspendForKamiVoice();
    // Voice historically permits a pending net_poll stop once uploads are paused.
    if (!suspension.uploadsReady) return false;

    if (hal::ble::started()) {
        hal::ble::deinit();
        s_blePaused = true;
        Serial.println("[agent-res] BLE paused for voice TLS");
    }

    s_held = true;
    core::memory::logSnapshot("voice.resources.acquired");
    return true;
}

void releaseResourcesImpl() {
    if (!s_held) return;

    // Only bring BLE back if WiFi is down. Device-wide rule: BLE stays off while
    // WiFi is up (it starves internal RAM / TLS). When WiFi is up the wifi-task
    // owns BLE and keeps it down; re-initing here would just flap it.
    if (s_blePaused) {
        if (!hal::wifi::connected() && hal::ble::init(BLE_DEVICE_NAME)) {
            hal::wifi::applyModemSleepPolicy();
            Serial.println("[agent-res] BLE resumed (WiFi down)");
        }
        s_blePaused = false;
    }

    services::background_network::resumeAfterKamiVoice();
    s_held = false;
    core::memory::logSnapshot("voice.resources.released");
}

} // namespace
bool prepare() {
    if (hal::audio::retirementPending()) return false;
    poll();
    // Capture can buffer while Wi-Fi reconnects; connection is not an entry gate.
    if (!hal::wifi::connected()) (void)hal::wifi::connect();
    return acquireResources();
}
StartResult start(services::audio::MicCapture::SampleSink sink, void* user) {
    if (capturing) return StartResult::Started;
    core::memory::logSnapshot("ptt.acquire.pre");
    const bool ok=slab.acquire();
    core::memory::logSnapshot(ok ? "ptt.acquire.post" : "ptt.acquire.fail");
    if (!ok) { releaseResourcesImpl(); return StartResult::MemoryUnavailable; }
    bool started=mic.startStream(sink,user);
    if(!started) {
        const uint32_t waitStarted=millis();
        while(!background_network::kamiVoiceNetworkReady() && uint32_t(millis()-waitStarted)<1500u)
            vTaskDelay(pdMS_TO_TICKS(10));
        if(background_network::kamiVoiceNetworkReady()) {
            vTaskDelay(pdMS_TO_TICKS(1));
            started=mic.startStream(sink,user);
            Serial.printf("[voice-entry] mic_retry=%u wait_ms=%lu\n",started,(unsigned long)(millis()-waitStarted));
        }
    }
    if (!started) {
        releaseBuffers(); releaseResourcesImpl(); return StartResult::MicUnavailable;
    }
    capturing=true;
    return StartResult::Started;
}
bool isCapturing() { return capturing; }
void tick() { if (capturing) mic.tick(); }
uint32_t finish() {
    if (capturing) { mic.stop(); capturing=false; }
    poll();
    return mic.samplesN();
}
void release() {
    if (capturing) { requestRelease(); return; }
    releaseRequested.store(false);
    releaseBuffers(); releaseResourcesImpl();
}
void requestRelease() { releaseRequested.store(true); }
void poll() { if (!capturing && releaseRequested.exchange(false)) release(); }
void resetForEntry() {
    releaseResourcesImpl();
    mic=services::audio::MicCapture{};
    capturing=false;
}
void restorePlayback() { hal::audio::restoreForUiPlayback(); }
const core::audio::RecordingWaveform& waveform() { return mic.waveform(); }
uint32_t samples() { return mic.samplesN(); }
int16_t* pcm() { return slab.pcm(); }
uint32_t capacity() { return slab.pcmMax(); }
void releaseBuffers() { services::agent_transport::detachAudio(); slab.release(); }
void releaseResources() { releaseResourcesImpl(); }
} }
