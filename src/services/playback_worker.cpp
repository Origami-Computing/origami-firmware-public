#include "services/playback_worker.h"

namespace services { namespace audio {
PlaybackWorker* PlaybackWorker::pending_ = nullptr;

#ifdef ORIGAMI_DEV_SERIAL_INPUT
namespace {
std::atomic<uint32_t> finishDelayMs{0};
std::atomic<bool> finishDelayActive{false};
}
bool PlaybackWorker::armFinishDelayForTest(uint32_t ms) {
    if (ms > 5000 || finishDelayActive.load() || hasPendingCleanup()) return false;
    finishDelayMs.store(ms);
    return true;
}
bool PlaybackWorker::finishDelayActiveForTest() { return finishDelayActive.load(); }
void PlaybackWorker::delayFinishForTest() {
    const uint32_t ms = finishDelayMs.exchange(0);
    if (!ms) return;
    finishDelayActive.store(true);
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += 10)
        vTaskDelay(pdMS_TO_TICKS(10));
    finishDelayActive.store(false);
}
#endif

bool PlaybackWorker::acquireControl() {
    if (cleanupPending() || (active() && !running())) return false;
    controlRequested_.store(true, std::memory_order_release);
    for (int i = 0; i <= 20; ++i) {
        if (!busy_.test_and_set(std::memory_order_acquire)) return true;
        if (i < 20) vTaskDelay(pdMS_TO_TICKS(10));
    }
    controlRequested_.store(false, std::memory_order_release);
    return false;
}

bool PlaybackWorker::beginIteration() {
    if (controlRequested_.load(std::memory_order_acquire) || stopRequested()) return false;
    if (busy_.test_and_set(std::memory_order_acquire)) return false;
    if (controlRequested_.load(std::memory_order_acquire) || stopRequested()) {
        endIteration();
        return false;
    }
    return true;
}

void PlaybackWorker::deferCleanup(Cleanup cleanup, void* owner) {
    if (!cleanup) return;
    retirement_.acquire();
    if (!cleanup_) {
        next_ = pending_;
        pending_ = this;
    }
    cleanup_ = cleanup;
    owner_ = owner;
}

void PlaybackWorker::cancelCleanup() {
    if (!cleanup_) return;
    PlaybackWorker** link = &pending_;
    while (*link && *link != this) link = &(*link)->next_;
    if (*link) *link = next_;
    cleanup_ = nullptr;
    owner_ = nullptr;
    next_ = nullptr;
    retirement_.release();
}

bool PlaybackWorker::hasPendingCleanup() { return pending_ != nullptr; }

void PlaybackWorker::pollCleanup() {
    // Do not run global I2S teardown while any retiring worker still has access.
    for (PlaybackWorker* item = pending_; item; item = item->next_)
        if (item->active()) return;
    // Every old worker is quiescent; allow each UI cleanup to restore hardware.
    for (PlaybackWorker* item = pending_; item; item = item->next_)
        item->retirement_.release();
    for (PlaybackWorker* item = pending_; item;) {
        PlaybackWorker* next = item->next_;
        if (!item->active()) {
            const Cleanup cleanup = item->cleanup_;
            void* owner = item->owner_;
            item->cancelCleanup();
            // Callback completes only this owner; it must not destroy other entries.
            cleanup(owner);
        }
        item = next;
    }
}
} }
