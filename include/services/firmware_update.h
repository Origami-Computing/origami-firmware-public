#pragma once
#include "services/agent_coordination.h"
#include "services/ota_client.h"

namespace services { namespace firmware_update {

// UI-task API: queue an optional check and consume its published result.
// No radio waits or HTTPS fallback on the UI task; allocation failure defers.
void checkSettingsNotice();
void pollSettingsNotice();
bool noticeBusy();
bool noticeInFlight(); // Worker-safe observation; no cleanup.
bool settingsUpdateAvailable();
bool copySettingsRelease(ota_client::ReleaseInfo* out);
void invalidateSettingsNotice();

// UI-task lifetime for an OTA operation. Keeps Wi-Fi connected while background
// workers and Agent release their network resources. The app owns confirmation,
// retries and progress presentation; services::ota_client owns the download worker.
class Session {
public:
    enum class Result { Ready, BackgroundBusy, AgentBusy };
    Session() = default;
    ~Session() { close(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Result reserveNetwork();
    // Wait for download cancellation without releasing the reservation. Used
    // by workflow reset/back; close guarantees worker exit before resumption.
    void cancel();
    void close();
private:
    bool reserved_ = false;
    agent_coordination::TransportPause agentPause_;
};

} }
