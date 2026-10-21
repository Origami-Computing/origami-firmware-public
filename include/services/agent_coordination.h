#pragma once
#include <stdint.h>

namespace services { namespace agent_coordination {
// One operation's transport pause. Distinct owners may overlap; only the last
// release permits socket work. A timeout releases only this operation's claim.
// Success means socket cleanup has acknowledged the pause, or no worker exists.
// Keep the owner alive until Wi-Fi/resource work is done. The same owner must
// not be used concurrently; sequential handoff between tasks is allowed.
class TransportPause {
public:
    TransportPause() = default;
    ~TransportPause() { release(); }
    TransportPause(const TransportPause&) = delete;
    TransportPause& operator=(const TransportPause&) = delete;
    bool acquire(uint32_t timeoutMs);
    void release();
    bool held() const { return held_; }
private:
    bool held_ = false;
};
bool isSessionActive();

// Diagnostic worker position; does not grant or change pause ownership.
enum class WorkerPhase { NotStarted, TextPump, PauseCleanup, NetworkCleanup,
                         SocketConfigure, SocketLoop, AudioPump, Waiting };
void setWorkerPhase(WorkerPhase phase);
// Transport-worker integration. Acknowledge only after all socket calls and
// disconnect cleanup have completed. Worker lifetime remains persistent.
void workerStarted();
// Called inside the persistent transport worker, before any socket work.
void bindCurrentWorker();
// True only on the registered transport worker; used by TLS diagnostics.
bool isCurrentWorker();
bool cancelHandshakeForCurrentTask();
bool suspendRequested();
void acknowledgeSuspended();
void setSessionActive(bool active);
} }
