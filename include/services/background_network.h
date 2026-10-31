#pragma once
#include "services/agent_coordination.h"
#include <stdint.h>

namespace services { namespace background_network {

// Single HTTP batch across periodic polling, uploads and Settings OTA notices.
// Nonblocking admission:
// a busy caller defers work instead of waiting inside TLS and expiring its watchdog.
// Release before idle/pause acknowledgment. Notice admission precedes worker
// allocation, with ownership transferred via its atomic state. Not an Agent lock.
class PeriodicWork {
public:
    PeriodicWork() = default;
    ~PeriodicWork() { release(); }
    PeriodicWork(const PeriodicWork&) = delete;
    PeriodicWork& operator=(const PeriodicWork&) = delete;
    bool tryEnter();
    // At a completed HTTP boundary, yield a long batch to a waiting peer.
    // Caller must delay before retrying; never call with live HTTP/SD resources.
    bool yieldIfContended();
    static bool hasWaitingPeer();
    void release();
private:
    bool entered_ = false;
};

// UploadQueue callbacks: admission runs on svc_task; release runs on uploadq.
// Own Agent/poller/widget handoff and defer poller restart until uploadq exits.
bool acquireUploadNetwork(uint32_t timeoutMs);
void releaseUploadNetwork();
// svc_task, immediately after uploads::tick(), under the same ServiceWork guard.
void tick();

/** Upload-worker boundary for RadioPause, with svc_task admission already closed.
 * Does not initialize the queue or require SD. No poller/Agent side effects. */
bool pauseWorkerForRadio(uint32_t timeoutMs);
/** Re-enable uploads and defer poller restoration to the next service tick. */
void resumeAfterRadioPause();

// Audio's upload gate is deliberately weaker than peripheral exclusivity.
// NotRequested includes early upload failure and Music before queue init.
// StopPending means net_poll received stop but had not exited by the deadline.
// This result is an observation, not a guarantee that all background work stops.
struct AudioSuspension {
  enum class Poller { NotRequested, Stopped, StopPending };
  bool uploadsReady;
  Poller poller;
};

/** Request cooperative network teardown without waiting or touching SD.
 * Capture starts locally; the transport waits for kamiVoiceNetworkReady().
 * Pair every successful request with resumeAfterKamiVoice(). */
AudioSuspension suspendForKamiVoice(uint32_t timeoutMs = 3000);
void resumeAfterKamiVoice();
/** Idle Agent connections yield to the bounded onboarding registration window.
 * Explicit voice sessions retain their existing foreground priority. */
bool kamiVoiceNetworkReady();
bool voicePriorityActive();
bool audioPriorityActive();
// UI-task ringing lifecycle. Cooperative/nonblocking: abort background
// transfers at their existing cancellation boundaries; resume on snooze/exit.
// Independent of voice ownership so an overlay cannot release its parent's gate.
void suspendForAlertAudio();
void resumeAfterAlertAudio();

/** Same best-effort net_poll request. Before queue init Music returns
 * uploadsReady=true, NotRequested. Pair every Music session with resume,
 * including unsuccessful preparation (the existing Music lifetime contract). */
AudioSuspension suspendForMusicAudio(uint32_t timeoutMs = 1500);
void resumeAfterMusicAudio();

/**
 * Stop upload and periodic-network workers while a peripheral needs exclusive
 * SD, DMA, or radio ownership. The caller must pair a successful suspension
 * with resumeAfterPeripheralIo(). UI-task only. Nested successful suspensions
 * share ownership until the final paired resume. Service admission remains
 * closed throughout; Agent transport policy remains with the caller.
 */
bool suspendForPeripheralIo(uint32_t timeoutMs = 5000);
// UI-task paired request for preview workflows. Request returns immediately;
// poll until ready, or resume to cancel. Each request needs one resume even
// when canceled before readiness. No SD/radio use is admitted before ready.
void requestPeripheralIo();
bool peripheralIoReady();
void resumeAfterPeripheralIo();


/** One svc_task iteration. Admission closes while a radio pause is held.
 * The service task must not perform quote/upload work without this guard.
 * Single service-task caller; no worker, allocation or driver calls here. */
class ServiceWork {
public:
    ServiceWork();
    ~ServiceWork();
    explicit operator bool() const { return entered_; }
    ServiceWork(const ServiceWork&) = delete;
    ServiceWork& operator=(const ServiceWork&) = delete;
private:
    bool entered_;
};

/** UI-task ownership of quote cancellation and svc_task admission.
 * Drain quote cancellation before closing admission, then wait for the current
 * service iteration to finish. This does not stop independent worker tasks.
 * One owner across radio and peripheral pauses; failure releases only our work.
 */
class ServicePause {
public:
    enum class Result { Acquired, AlreadyOwned, QuoteBusy, ServiceBusy };
    ServicePause() = default;
    ~ServicePause() { release(); }
    ServicePause(const ServicePause&) = delete;
    ServicePause& operator=(const ServicePause&) = delete;
    Result acquire(uint32_t timeoutMs);
    // UI-task nonblocking handoff. Busy retains acquired gates until another
    // poll or release (do not switch to acquire while pending). Existing
    // HTTP/cache work must acknowledge before Acquired.
    Result pollAcquire();
    void release();
private:
    bool owned_ = false, held_ = false, quote_ = false, gate_ = false;
};

/** UI-task radio handoff, after foreground apps release their operations.
 * Stops quote work, svc_task admission, uploads, periodic HTTP, widget polling
 * and Agent transport. Does not require SD or change Wi-Fi itself.
 * Success permits radio-off until release; failure rolls back every acquired
 * pause and leaves the radio untouched. Restore Wi-Fi (or queue manager restoration) before release on wake.
 * Consumers must still respect suspended/connectivity during asynchronous restore.
 * Pending audio retirement refuses before acquiring anything.
 * Only one owner; not composable with a foreground peripheral reservation. */
class RadioPause {
public:
    enum class Result { Acquired, AlreadyOwned, QuoteBusy, ServiceBusy,
                        UploadBusy, PollerBusy, WidgetBusy, AgentBusy, PlaybackBusy };
    RadioPause() = default;
    ~RadioPause() { release(); }
    RadioPause(const RadioPause&) = delete;
    RadioPause& operator=(const RadioPause&) = delete;
    Result acquire(uint32_t timeoutMs = 3000);
    void release();
private:
    ServicePause service_;
    bool held_ = false;
    bool uploads_ = false, pollerRequested_ = false, widgets_ = false;
    agent_coordination::TransportPause agent_;
};

const char* resultName(RadioPause::Result result);

} }
