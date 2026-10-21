#pragma once
#include <atomic>
#include "hal/hal_tasks.h"
#include "hal/audio_retirement.h"

namespace services { namespace audio {

// Shared MP3/WAV worker lifecycle. UI-task start/stop calls are serialized;
// the worker publishes finish only after its final access to playback state.
// A stop timeout retains Stopping and prevents a second worker from starting.
// UI-task deferred cleanup runs only after acknowledgment. Audio admission
// belongs to the surrounding handoff contract. Pending owners must stay alive.
class PlaybackWorker {
public:
    PlaybackWorker() = default;
    PlaybackWorker(const PlaybackWorker&) = delete;
    PlaybackWorker& operator=(const PlaybackWorker&) = delete;

    bool start(hal::tasks::Id id, TaskFunction_t entry, void* context) {
        if (cleanupPending()) return false;
        State expected = State::Idle;
        if (!state_.compare_exchange_strong(expected, State::Running))
            return expected == State::Running;
        if (hal::tasks::spawn(id, entry, context)) return true;
        state_.store(State::Idle, std::memory_order_release);
        return false;
    }

    // Existing 200 ms wait plus one yield after acknowledgment. False means
    // the caller must retain any state/resources the worker could still use.
    bool stop() {
        State expected = State::Running;
        state_.compare_exchange_strong(expected, State::Stopping);
        if (expected == State::Idle) return true;
        for (int i = 0; i < 20 && active(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
        if (active()) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
        return true;
    }

    bool active() const { return state_.load(std::memory_order_acquire) != State::Idle; }
    bool running() const { return state_.load(std::memory_order_acquire) == State::Running; }
    bool stopRequested() const { return !running(); }
    // Worker-only, after all resource accesses/logging; no object access follows.
    // The task may still execute vTaskDelete(nullptr), which uses no owner state.
    void finish() {
#ifdef ORIGAMI_DEV_SERIAL_INPUT
        delayFinishForTest();
#endif
        state_.store(State::Idle, std::memory_order_release);
    }
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    // One-shot diagnostic: delay acknowledgment, never actual driver work.
    // UI task arms at most 5 seconds; zero cancels an unused request.
    static bool armFinishDelayForTest(uint32_t ms);
    static bool finishDelayActiveForTest();
#endif

    // UI-task control excludes a whole worker iteration. A timed-out request
    // changes no playback state and lets the worker continue. Never nest guards.
    class Control {
    public:
        explicit Control(PlaybackWorker& worker) : worker_(worker), held_(worker.acquireControl()) {}
        ~Control() { if (held_) worker_.releaseControl(); }
        explicit operator bool() const { return held_; }
        Control(const Control&) = delete;
        Control& operator=(const Control&) = delete;
    private:
        PlaybackWorker& worker_;
        bool held_;
    };
    // Worker-only: pair each admitted iteration with endIteration before delay/finish.
    bool beginIteration();
    void endIteration() { busy_.clear(std::memory_order_release); }

    using Cleanup = void (*)(void*);
    // UI-task only. Repeated registration updates this owner's callback once.
    void deferCleanup(Cleanup cleanup, void* owner);
    void cancelCleanup();
    bool cleanupPending() const { return cleanup_ != nullptr; }
    static void pollCleanup();
    static bool hasPendingCleanup();

private:
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    static void delayFinishForTest();
#endif
    bool acquireControl();
    void releaseControl() {
        busy_.clear(std::memory_order_release);
        controlRequested_.store(false, std::memory_order_release);
    }
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> controlRequested_{false};
    enum class State { Idle, Running, Stopping };
    std::atomic<State> state_{State::Idle};
    hal::audio::RetirementBlock retirement_;
    Cleanup cleanup_ = nullptr;
    void* owner_ = nullptr;
    PlaybackWorker* next_ = nullptr;
    static PlaybackWorker* pending_;

};

} }
