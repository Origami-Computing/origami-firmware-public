#include "hal/hal_tasks.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_freertos_hooks.h>
#include <atomic>

namespace hal {
namespace tasks {

// Central task registry — one row per task.
// To change a task's core, priority, or stack: edit this table only.
// The row order MUST match the Id enum in hal_tasks.h.
static constexpr Spec kSpecs[] = {
    //  name          core        priority          stackBytes
    // Sized from on-device minimum-free watermarks on fully assembled N16R8.
    // Task stacks consume scarce internal DRAM (not the 8 MB PSRAM), so large
    // speculative margins here directly starve camera/I2S/TLS DMA allocations.
    { "ui",           CORE_UI,    PRIO_REALTIME,    12288 },  // measured minFree 4204B at 14336
    { "wifi_mgr",     CORE_SVC,   PRIO_SERVICE,      4096 },  // Id::WifiMgr
    { "svc",          CORE_SVC,   PRIO_SERVICE,      6144 },  // quote persistence needs SD/JSON call depth; replaces 16KB quote worker
    // The former 4 KB stack buffer now lives on the heap. 10 KB retains 2 KB
    // more call-depth room than the old overflowing 8 KB layout while leaving
    // a contiguous internal-RAM block for TLS/AES allocations.
    { "uploadq",      CORE_SVC,   PRIO_BACKGROUND,  10240 },  // Id::UploadQ
    { "kami_ws",      CORE_SVC,   PRIO_NETWORK,      7168 },  // measured minFree 3668B at 8192
    { "qr_scan",      CORE_SVC,   PRIO_SERVICE,      8192 },  // Id::QrScan: 22KB decode scratch moved to PSRAM
    // Decoder input/PCM buffers live in PSRAM/static storage. 3584 B leaves
    // measured runtime margin while remaining spawnable after TLS/I2S heap
    // fragmentation during Music app re-entry.
    { "mp3_play",     CORE_SVC,   PRIO_REALTIME,     3584 },  // Id::Mp3Play
    { "wav_play",     CORE_SVC,   PRIO_REALTIME,     3584 },  // Id::WavPlay
    // Fleet recovery: the telemetry snapshot now lives in PSRAM instead of
    // consuming ~3 KB inside the TLS call chain. The former 16 KB worker had
    // 7272 B free at its TLS low-water mark, but could not restart when the
    // largest internal block was 12276 B. 10 KB retains the measured headroom
    // after moving the batch and fits alongside the audio/WebSocket workers.
    // The verified-certificate failure path in this architecture overflowed
    // staging's 7424 B stack on N16R8. Keep the TLS-tested 10 KB allocation;
    // low-memory restart must defer rather than run with insufficient stack.
    { "net_poll",     CORE_SVC,   PRIO_BACKGROUND,  10240 },
    // Widget downloads are background work. Share CPU with net_poll so repeated
    // TLS parsing cannot indefinitely preempt its progress/watchdog checks.
    { "widget_poll",  CORE_SVC,   PRIO_BACKGROUND,  10240 },  // 7680B overflowed during bulk-sync TLS certificate verification
    // Preserve certificate-verification headroom in both OTA TLS consumers.
    { "ota_download", CORE_SVC,   PRIO_BACKGROUND,  12288 },
    { "ota_notice",   CORE_SVC,   PRIO_BACKGROUND,  10240 },
    { "haptic",       CORE_SVC,   PRIO_REALTIME,     3072 },
    { "video_write",  CORE_SVC,   PRIO_BACKGROUND,   4096 },
    { "video_read",   CORE_SVC,   PRIO_BACKGROUND,   4096 },
    // Probe WAV finalization left only 336 B free at 4096 on rev4; retain
    // additional headroom for SD and logging error paths.
    { "camera_audio", CORE_SVC,   PRIO_REALTIME,     6144 },
    { "video_capture",CORE_SVC,   PRIO_SERVICE,      4096 },
    { "video_preview",CORE_SVC,   PRIO_BACKGROUND,   6144 },
};

static_assert(sizeof(kSpecs) / sizeof(kSpecs[0]) == (size_t)Id::_Count,
              "kSpecs row count must match hal::tasks::Id enum");

struct Observation {
    std::atomic<TaskHandle_t> handle{nullptr};
    std::atomic<uint32_t> completedMinimumPlusOne{0};
};
static Observation s_observations[(size_t)Id::_Count];
// These workers alternate under background_network admission; OTA holds a
// firmware_update network reservation while downloading.
// Camera acquisition holds a peripheral reservation excluding UploadQ/NetPoll
// and borrows this storage without another permanent stack or a fragmented
// 4 KiB allocation. The HAL slot also excludes concurrent OTA stack use;
// ordinary feature admission still applies.
// Reserve their one internal stack so TLS/SD heap fragmentation cannot prevent
// either from restarting. Do not reduce TLS stack headroom or use PSRAM stacks.
static constexpr size_t kNetworkStackBytes = 12288;
alignas(16) static StackType_t s_networkStack[kNetworkStackBytes / sizeof(StackType_t)];
static constexpr size_t kPlaybackStackBytes = 3584;
alignas(16) static StackType_t s_playbackStack[kPlaybackStackBytes / sizeof(StackType_t)];
enum class StackSlot { Free, Active, Retiring, Quarantined };
struct RetainedStack {
    explicit RetainedStack(StackType_t* storage) : stack(storage) {}
    StackType_t* stack;
    StaticTask_t tcb;
    std::atomic<StackSlot> state{StackSlot::Free};
    bool hookRegistered = false; // Accessed only by this slot's claimant.
};
static RetainedStack s_networkSlot(s_networkStack), s_playbackSlot(s_playbackStack);
static_assert(kSpecs[(size_t)Id::UploadQ].stackBytes <= kNetworkStackBytes &&
              kSpecs[(size_t)Id::NetPoll].stackBytes <= kNetworkStackBytes &&
              kSpecs[(size_t)Id::UploadQ].core == CORE_SVC &&
              kSpecs[(size_t)Id::NetPoll].core == CORE_SVC &&
              kSpecs[(size_t)Id::OtaDownload].stackBytes == kNetworkStackBytes &&
              kSpecs[(size_t)Id::OtaDownload].core == CORE_SVC &&
              kSpecs[(size_t)Id::VideoCapture].stackBytes <= kNetworkStackBytes &&
              kSpecs[(size_t)Id::VideoCapture].core == CORE_SVC,
              "Shared network stack must fit all registered workers");
static bool usesNetworkStack(Id id) { return id == Id::UploadQ || id == Id::NetPoll || id == Id::OtaDownload || id == Id::VideoCapture; }
static_assert(kSpecs[(size_t)Id::Mp3Play].stackBytes == kPlaybackStackBytes &&
              kSpecs[(size_t)Id::WavPlay].stackBytes == kPlaybackStackBytes &&
              kSpecs[(size_t)Id::Mp3Play].core == CORE_SVC &&
              kSpecs[(size_t)Id::WavPlay].core == CORE_SVC,
              "Playback workers must fit the shared internal stack");
static RetainedStack* retainedStack(Id id) {
    if (usesNetworkStack(id)) return &s_networkSlot;
    if (id == Id::Mp3Play || id == Id::WavPlay) return &s_playbackSlot;
    return nullptr;
}
static bool reclaimStack(RetainedStack& slot) {
    // IDF 4.4's same-core idle task cleans deleted TCBs before idle hooks.
    // Use TWO observations: retirement can preempt an idle iteration after its
    // cleanup but before this hook. The next iteration necessarily cleans it.
    auto state = slot.state.load();
    if (state == StackSlot::Retiring)
        slot.state.compare_exchange_strong(state, StackSlot::Quarantined);
    else if (state == StackSlot::Quarantined)
        slot.state.compare_exchange_strong(state, StackSlot::Free);
    return true;
}
static bool reclaimNetworkStack() { return reclaimStack(s_networkSlot); }
static bool reclaimPlaybackStack() { return reclaimStack(s_playbackSlot); }
static bool bootLifetime(Id id) {
    return id == Id::Ui || id == Id::WifiMgr || id == Id::Services ||
           id == Id::KamiSend || id == Id::WidgetPoll || id == Id::Haptic;
}

const Spec& spec(Id id) {
    return kSpecs[(size_t)id];
}

TaskHandle_t spawn(Id id, TaskFunction_t fn, void* arg) {
    const Spec& s = spec(id);
    const size_t dramFreeBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t dramLargestBefore =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    TaskHandle_t handle = nullptr;
    BaseType_t ok = pdFAIL;
    if (auto* slot = retainedStack(id)) {
        auto available = StackSlot::Free;
        if (!slot->state.compare_exchange_strong(available, StackSlot::Active)) return nullptr;
        if (!slot->hookRegistered) {
            slot->hookRegistered = esp_register_freertos_idle_hook_for_cpu(
                usesNetworkStack(id) ? reclaimNetworkStack : reclaimPlaybackStack, CORE_SVC) == ESP_OK;
        }
        if (slot->hookRegistered) {
            handle = xTaskCreateStaticPinnedToCore(fn, s.name, s.stackBytes, arg,
                         s.priority, slot->stack, &slot->tcb, s.core);
        }
        if (handle) ok = pdPASS;
        else slot->state.store(StackSlot::Free);
    } else {
        ok = xTaskCreatePinnedToCore(fn, s.name, s.stackBytes, arg, s.priority, &handle, s.core);
    }
    if (ok != pdPASS) {
        Serial.printf("[tasks] FAILED to spawn '%s' (core=%d prio=%u stack=%u) "
                      "dramFree=%u dramLargest=%u\n",
                      s.name, (int)s.core, (unsigned)s.priority,
                      (unsigned)s.stackBytes, (unsigned)dramFreeBefore,
                      (unsigned)dramLargestBefore);
        return nullptr;
    }
    Serial.printf("[tasks] spawned '%s' core=%d prio=%u stack=%uB "
                  "dramFreeBefore=%u dramLargestBefore=%u\n",
                  s.name, (int)s.core, (unsigned)s.priority,
                  (unsigned)s.stackBytes, (unsigned)dramFreeBefore,
                  (unsigned)dramLargestBefore);
    // Restartable workers may self-delete. Only boot-lifetime handles remain
    // valid for later cross-task watermark queries.
    if (bootLifetime(id)) s_observations[(size_t)id].handle.store(handle);
    return handle;
}

void deleteCurrent(Id id) {
    if (auto* slot = retainedStack(id)) slot->state.store(StackSlot::Retiring);
    // No blocking operation between publication and deletion: all shared-slot workers are
    // pinned above idle priority on CORE_SVC. The idle hook owns actual reuse.
    vTaskDelete(nullptr);
}

void recordCurrentStack(Id id) {
    const uint32_t minimum = uxTaskGetStackHighWaterMark(nullptr);
    s_observations[(size_t)id].completedMinimumPlusOne.store(minimum + 1u);
}

void dumpStackWatermarks() {
    for (size_t i = 0; i < (size_t)Id::_Count; ++i) {
        const Spec& s = kSpecs[i];
        if (!bootLifetime((Id)i)) {
            const uint32_t saved = s_observations[i].completedMinimumPlusOne.load();
            if (saved) Serial.printf("[tasks] %-11s configured=%uB minFree=%uB completed=1\n",
                                     s.name, (unsigned)s.stackBytes, (unsigned)(saved - 1u));
            else Serial.printf("[tasks] %-11s watermark unavailable configured=%uB\n",
                               s.name, (unsigned)s.stackBytes);
            continue;
        }
        TaskHandle_t h = s_observations[i].handle.load();
        if (!h) {
            Serial.printf("[tasks] %-11s not running configured=%uB\n",
                          s.name, (unsigned)s.stackBytes);
            continue;
        }
        Serial.printf("[tasks] %-11s configured=%uB minFree=%uB\n",
                      s.name, (unsigned)s.stackBytes,
                      (unsigned)uxTaskGetStackHighWaterMark(h));
    }
}

}  // namespace tasks
}  // namespace hal
