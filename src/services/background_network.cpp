#include "hal/audio_retirement.h"
#include "hal/hal_sd.h"
#include "services/background_network.h"
#include "services/uploads.h"
#include "services/firmware_update.h"
#include "services/morning_quote.h"
#include "services/agent_coordination.h"
#include "services/net_poll.h"
#include "core/watchdog.h"
#include "widgets/sync.h"
#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace services { namespace background_network {
namespace {
std::atomic<bool> periodicOwned{false}, periodicContended{false};
ServicePause s_peripheralPause;
uint32_t s_peripheralHolders = 0; // UI-task nested successful reservations.
agent_coordination::TransportPause uploadAgentPause;
PeriodicWork uploadNetworkWork;
constexpr unsigned kBusy = 1, kPaused = 2;
std::atomic<unsigned> serviceState{0};
std::atomic<bool> pauseOwner{false};
std::atomic<bool> voicePriority{false}, uploadAcquiring{false};
std::atomic<bool> alertPriority{false};
void requestAudioPause() {
  if(auto* queue=uploads::readyQueue()) queue->requestStopForVoice();
  services::net_poll::requestStop();
  widgets::sync::requestPollingPause();
}
void resumeAudioIfIdle() {
  if(audioPriorityActive() || pauseOwner.load()) return;
  widgets::sync::resumePolling();
  services::net_poll::requestStart();
  if(auto* queue=uploads::readyQueue()) if(!queue->resumeWorker()) Serial.println("[queue] Failed to resume upload worker");
}
}

bool PeriodicWork::tryEnter() {
    if (entered_) return true;
    if (audioPriorityActive()) return false;
    bool idle = false;
    entered_ = periodicOwned.compare_exchange_strong(idle, true);
    if (!entered_) periodicContended.store(true);
    return entered_;
}
bool PeriodicWork::hasWaitingPeer() { return periodicContended.load(); }
bool PeriodicWork::yieldIfContended() {
    if (!entered_ || !periodicContended.exchange(false)) return false;
    release();
    return true;
}
void PeriodicWork::release() {
    if (entered_) { entered_ = false; periodicOwned.store(false); }
}

ServiceWork::ServiceWork() {
    unsigned idle = 0;
    entered_ = serviceState.compare_exchange_strong(idle, kBusy);
}
ServiceWork::~ServiceWork() {
    if (entered_) serviceState.fetch_and(~kBusy);
}

ServicePause::Result ServicePause::acquire(uint32_t timeoutMs) {
    if (held_) return Result::Acquired;
    // Optional OTA metadata already in flight owns its TLS request until it
    // completes. Refuse a radio/peripheral handoff without blocking the UI.
    if (firmware_update::noticeInFlight()) return Result::ServiceBusy;
    bool available = false;
    if (!pauseOwner.compare_exchange_strong(available, true)) return Result::AlreadyOwned;
    owned_ = true;
    auto fail = [this](Result result) { release(); return result; };

    // Quote cancellation is serviced by svc_task: let it drain before closing
    // admission. Then wait out any iteration (including upload acquisition).
    core::watchdog::feed();
    if (!morning_quote::suspendForPeripheralIo(timeoutMs)) return fail(Result::QuoteBusy);
    quote_ = true;
    serviceState.fetch_or(kPaused);
    gate_ = true;
    const uint32_t started = millis();
    while (serviceState.load() & kBusy) {
        if (uint32_t(millis() - started) >= timeoutMs) return fail(Result::ServiceBusy);
        core::watchdog::feed();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    held_ = true;
    return Result::Acquired;
}

void ServicePause::release() {
    if (!owned_) return;
    if (quote_) morning_quote::resumeAfterPeripheralIo();
    if (gate_) serviceState.fetch_and(~kPaused);
    owned_ = held_ = quote_ = gate_ = false;
    pauseOwner.store(false);
}

RadioPause::Result RadioPause::acquire(uint32_t timeoutMs) {
    if (held_) return Result::Acquired;
    if (hal::audio::retirementPending()) return Result::PlaybackBusy;
    switch (service_.acquire(timeoutMs)) {
    case ServicePause::Result::AlreadyOwned: return Result::AlreadyOwned;
    case ServicePause::Result::QuoteBusy: return Result::QuoteBusy;
    case ServicePause::Result::ServiceBusy: return Result::ServiceBusy;
    case ServicePause::Result::Acquired: break;
    }
    auto fail = [this](Result result) { release(); return result; };
    core::watchdog::feed();
    if (!pauseWorkerForRadio(timeoutMs)) return fail(Result::UploadBusy);
    uploads_ = true;
    core::watchdog::feed();
    pollerRequested_ = true; // Even timeout leaves net_poll's stop request set.
    if (!services::net_poll::stop(timeoutMs)) return fail(Result::PollerBusy);
    core::watchdog::feed();
    if (!widgets::sync::suspendPolling(timeoutMs)) return fail(Result::WidgetBusy);
    widgets_ = true;
    core::watchdog::feed();
    if (!agent_.acquire(timeoutMs)) return fail(Result::AgentBusy);
    held_ = true;
    return Result::Acquired;
}

void RadioPause::release() {
    agent_.release();
    if (widgets_ && !audioPriorityActive()) widgets::sync::resumePolling();
    if (uploads_ || pollerRequested_) resumeAfterRadioPause();
    // Reopen admission only after restoring all consumer permissions.
    service_.release();
    held_ = uploads_ = pollerRequested_ = widgets_ = false;
}

const char* resultName(RadioPause::Result result) {
    switch (result) {
    case RadioPause::Result::Acquired: return "ready";
    case RadioPause::Result::PlaybackBusy: return "playback-retiring";
    case RadioPause::Result::AlreadyOwned: return "already-owned";
    case RadioPause::Result::QuoteBusy: return "quote";
    case RadioPause::Result::ServiceBusy: return "service";
    case RadioPause::Result::UploadBusy: return "upload";
    case RadioPause::Result::PollerBusy: return "net-poll";
    case RadioPause::Result::WidgetBusy: return "widget";
    case RadioPause::Result::AgentBusy: return "agent";
    }
    return "unknown";
}


bool acquireUploadNetwork(uint32_t timeoutMs) {
  struct Guard { Guard(){uploadAcquiring.store(true);} ~Guard(){uploadAcquiring.store(false);} } guard;
  if(audioPriorityActive()) return false;
  if (services::agent_coordination::isSessionActive()) return false;
  if (!uploadAgentPause.acquire(timeoutMs)) return false;

  if (!services::net_poll::stop(timeoutMs)) {
    services::net_poll::requestStart();
    uploadAgentPause.release();
    return false;
  }
  if (!widgets::sync::suspendPolling(timeoutMs)) {
    services::net_poll::requestStart();
    uploadAgentPause.release();
    return false;
  }

  if(audioPriorityActive()){uploadAgentPause.release();return false;}
  // Settings OTA notices use the same nonblocking HTTP admission before their
  // worker stack is allocated. Stopping periodic workers alone does not exclude
  // an already admitted notice.
  if (!uploadNetworkWork.tryEnter()) {
    widgets::sync::resumePolling();
    services::net_poll::requestStart();
    uploadAgentPause.release();
    return false;
  }
  Serial.println("[queue] Exclusive network slot acquired");
  return true;
}

void releaseUploadNetwork() {
  uploadNetworkWork.release();
  if(!audioPriorityActive()) widgets::sync::resumePolling();
  // The callback runs on uploadq, whose stack may occupy the only block large
  // enough for net_poll. Restart from the service tick after uploadq deletes.
  if(!audioPriorityActive()) services::net_poll::requestStart();
  uploadAgentPause.release();
  Serial.println("[queue] Exclusive network slot released");
}

void tick() {
  if(audioPriorityActive()) return;
  auto* queue = uploads::readyQueue();
  if ((!queue || !queue->workerRunning()) && services::net_poll::startRequested()) {
    Serial.println("[queue] net_poll restarted after upload worker exit");
  }
}

bool pauseWorkerForRadio(uint32_t timeoutMs) {
  auto* queue = uploads::readyQueue();
  return !queue || queue->stopWorkerForVoice(timeoutMs);
}

void resumeAfterRadioPause() {
  if(audioPriorityActive()) return;
  services::net_poll::requestStart();
  if (auto* queue = uploads::readyQueue()) (void)queue->resumeWorker();
}

bool voicePriorityActive() { return voicePriority.load(); }
bool audioPriorityActive() { return voicePriority.load() || alertPriority.load(); }
void suspendForAlertAudio() {
  alertPriority.store(true);
  requestAudioPause();
}
void resumeAfterAlertAudio() {
  if(!alertPriority.exchange(false)) return;
  resumeAudioIfIdle();
}
AudioSuspension suspendForKamiVoice(uint32_t) {
  if (hal::audio::retirementPending()) return {false, AudioSuspension::Poller::NotRequested};
  voicePriority.store(true);
  requestAudioPause();
  Serial.println("[queue] Voice priority requested; capture can start locally");
  return {true,AudioSuspension::Poller::StopPending};
}
bool kamiVoiceNetworkReady() {
  if(!voicePriority.load()) return !services::net_poll::onboardingRegistrationPriority();
  widgets::sync::requestPollingPause();
  auto* queue=uploads::readyQueue();
  return !firmware_update::noticeInFlight() && !uploadAcquiring.load() && (!queue || !queue->workerRunning()) &&
      !services::net_poll::running() && widgets::sync::pollingPaused();
}
void resumeAfterKamiVoice() {
  voicePriority.store(false);
  resumeAudioIfIdle();
}

AudioSuspension suspendForMusicAudio(uint32_t timeoutMs) {
  if (hal::audio::retirementPending()) return {false, AudioSuspension::Poller::NotRequested};
  auto* queue = uploads::readyQueue();
  if (!queue) return {true, AudioSuspension::Poller::NotRequested};
  if (!queue->stopWorkerForVoice(timeoutMs)) return {false, AudioSuspension::Poller::NotRequested};
  const bool pollerStopped = services::net_poll::stop(timeoutMs);
  Serial.println("[queue] Worker stopped for Music audio");
  return {true, pollerStopped ? AudioSuspension::Poller::Stopped : AudioSuspension::Poller::StopPending};
}

void resumeAfterMusicAudio() {
  if(audioPriorityActive()) return;
  services::net_poll::requestStart();
  auto* queue = uploads::readyQueue();
  if (!queue) return;
  if (!queue->resumeWorker()) {
    Serial.println("[queue] Failed to resume upload worker after Music");
  }
}

bool suspendForPeripheralIo(uint32_t timeoutMs) {
  if (hal::audio::retirementPending()) return false;
  if (!uploads::ensureInit()) return false;
  if (s_peripheralHolders) { ++s_peripheralHolders; return true; }
  if (s_peripheralPause.acquire(timeoutMs) != background_network::ServicePause::Result::Acquired)
    return false;
  if (!uploads::queue().stopWorkerForVoice(timeoutMs)) {
    s_peripheralPause.release();
    return false;
  }
  // Let one in-flight 5-second HTTP request finish and release its resources.
  if (!services::net_poll::stop(timeoutMs + 2000u)) {
    services::net_poll::requestStart();
    (void)uploads::queue().resumeWorker();
    Serial.println("[queue] Peripheral I/O pause timed out stopping net_poll");
    s_peripheralPause.release();
    return false;
  }
  if (!widgets::sync::suspendPolling(timeoutMs)) {
    services::net_poll::requestStart();
    (void)uploads::queue().resumeWorker();
    Serial.println("[queue] Peripheral I/O pause timed out stopping widget_poll");
    s_peripheralPause.release();
    return false;
  }
  s_peripheralHolders = 1;
  Serial.println("[queue] Background I/O stopped for peripheral");
  return true;
}

void resumeAfterPeripheralIo() {
  if (!s_peripheralHolders || --s_peripheralHolders) return;
  if(audioPriorityActive()) { s_peripheralPause.release(); return; }
  widgets::sync::resumePolling();
  services::net_poll::requestStart();
  if (!uploads::queue().resumeWorker()) {
    Serial.println("[queue] Failed to resume worker after peripheral I/O");
  }
  s_peripheralPause.release();
}
} }
