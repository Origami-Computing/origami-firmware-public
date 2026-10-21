#include "services/firmware_update.h"
#include "services/background_network.h"
#include "services/agent_coordination.h"
#include "services/ota_client.h"
#include "core/memory.h"
#include "hal/hal_wifi.h"
#include "hal/hal_tasks.h"
#include <Arduino.h>
#include <atomic>

namespace services { namespace firmware_update {
namespace {
enum class NoticeState { Idle, Pending, Running, Complete };
std::atomic<NoticeState> noticeState{NoticeState::Idle};
bool noticeValid=false;
background_network::PeriodicWork noticeNetworkWork;
unsigned reservations=0;
uint32_t checkedMs=0;
ota_client::ReleaseChannel noticeChannel=ota_client::ReleaseChannel::Production;
ota_client::ReleaseInfo noticeRelease{};
void performNoticeCheck() {
    ota_client::ReleaseInfo result{};
    const bool ok=hal::wifi::connected() && ota_client::fetchLatestRelease(noticeChannel,&result);
    noticeRelease=result;noticeValid=ok;
    Serial.printf("[ota-notice] checked channel=%s ok=%d newer=%d\n",ota_client::releaseChannelName(noticeChannel),ok,
        ok && ota_client::isReleaseNewer(noticeChannel,result.tag));
}
}
void pollSettingsNotice() {
    // Acquire publishes the worker's release metadata to the UI task.
    if (noticeState.load(std::memory_order_acquire)==NoticeState::Complete)
        noticeState.store(NoticeState::Idle);
}
bool noticeInFlight(){const auto state=noticeState.load();return state==NoticeState::Pending || state==NoticeState::Running;}
bool noticeBusy(){pollSettingsNotice();return noticeState.load()!=NoticeState::Idle;}
void invalidateSettingsNotice(){
    if(!noticeBusy()) { noticeValid=false;checkedMs=0; }
}
static void noticeWorker(void*) {
    auto expected=NoticeState::Pending;
    if(!noticeState.compare_exchange_strong(expected,NoticeState::Running)) {
        noticeNetworkWork.release();
        vTaskDelete(nullptr);
        return;
    }
    performNoticeCheck();
    hal::tasks::recordCurrentStack(hal::tasks::Id::OtaNotice);
    noticeNetworkWork.release();
    noticeState.store(NoticeState::Complete,std::memory_order_release);
    vTaskDelete(nullptr);
}
bool settingsUpdateAvailable(){return !noticeBusy() && noticeValid &&
    noticeChannel==ota_client::releaseChannel() && ota_client::isReleaseNewer(noticeChannel,noticeRelease.tag);}
bool copySettingsRelease(ota_client::ReleaseInfo* out){if(!out || !settingsUpdateAvailable())return false;*out=noticeRelease;return true;}
void checkSettingsNotice() {
    pollSettingsNotice();
    if(noticeState.load()!=NoticeState::Idle || reservations || !hal::wifi::connected())return;
    const auto channel=ota_client::releaseChannel();const uint32_t now=millis();
    if(checkedMs && channel==noticeChannel && now-checkedMs<(noticeValid?300000u:30000u))return;
    // Admission precedes stack allocation: even an idle 10 KiB worker can
    // starve an in-flight upload's AES/DMA allocation. Busy work simply defers.
    if (!noticeNetworkWork.tryEnter()) return;
    checkedMs=now;noticeChannel=channel;
    // Optional metadata must never wait for radio ownership or run a
    // synchronous fallback on the Settings/UI task. Low memory defers it.
    noticeState.store(NoticeState::Pending,std::memory_order_release);
    if(!hal::tasks::spawn(hal::tasks::Id::OtaNotice,noticeWorker)) {
        noticeNetworkWork.release();
        noticeState.store(NoticeState::Idle);
        Serial.println("[ota-notice] deferred: no worker stack");
    }
}
Session::Result Session::reserveNetwork() {
    if (reserved_) return Result::Ready;
    if (noticeBusy()) return Result::BackgroundBusy;
    if (!background_network::suspendForPeripheralIo(1500)) return Result::BackgroundBusy;
    // Preserve OTA's longer transport timeout and keep Wi-Fi connected.
    if (!agentPause_.acquire(15000)) {
        background_network::resumeAfterPeripheralIo();
        return Result::AgentBusy;
    }
    reserved_ = true;
    ++reservations;
    core::memory::logSnapshot("ota.network.reserved");
    return Result::Ready;
}

void Session::cancel() { services::ota_client::abortFlash(); }

void Session::close() {
    cancel(); // abortFlash waits for the download worker to finish its cleanup.
    if (!reserved_) return;
    agentPause_.release();
    background_network::resumeAfterPeripheralIo();
    reserved_ = false;
    --reservations;
}
} }
