"""Actual widget pause/notification functions with a persistent modeled worker.

HTTP/SD and FreeRTOS are fakes. TSan establishes host synchronization only;
this does not prove hardware cancellation latency or Lua VM behavior.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'src/widgets/sync.cpp')
    args = parser.parse_args()
    source = args.source.read_text()
    begin = source.index('static ', source.index('constexpr uint32_t kPollIntervalMs'))
    state = source[begin:source.index('bool pollPausePending()', begin)].replace('static services::background_network::PeriodicWork s_periodicWork;', '')
    functions = '\n'.join(function(source, signature) for signature in (
        'bool pollPausePending()', 'bool pauseAtIdleCheckpoint()',
        'void requestPollingPause()', 'bool pollingPaused()', 'bool suspendPolling(uint32_t timeoutMs)', 'void resumePolling()',
        'bool takeSyncNotification()'))
    functions = functions.replace('s_periodicWork.release();', '')
    harness = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
using TaskHandle_t = void*;
int workerIdentity;
thread_local TaskHandle_t self = nullptr;
TaskHandle_t xTaskGetCurrentTaskHandle() { return self; }
constexpr bool pdTRUE = true;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
#define pdMS_TO_TICKS(x) (x)
std::mutex notificationMutex;
std::condition_variable notificationChanged;
unsigned notifications = 0;
void xTaskNotifyGive(TaskHandle_t task) {
    assert(task == &workerIdentity);
    std::lock_guard<std::mutex> lock(notificationMutex);
    ++notifications;
    notificationChanged.notify_one();
}
void ulTaskNotifyTake(bool, uint32_t) {
    std::unique_lock<std::mutex> lock(notificationMutex);
    const bool woke = notificationChanged.wait_for(lock, std::chrono::seconds(3),
        []{return notifications != 0;});
    assert(woke); notifications = 0;
}
uint32_t millis() {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void vTaskDelay(uint32_t) { std::this_thread::sleep_for(std::chrono::microseconds(50)); }
struct Logger {
    template<class... T> void printf(const char*, T...) {}
    void println(const char*) {}
} Serial;
'''
    cases = r'''
int main() {
    // Before worker publication, retain the pause so startup cannot begin I/O.
    assert(suspendPolling(0));
    assert(!takeSyncNotification());
    std::atomic<bool> stop{false}, busy{false};
    std::atomic<unsigned> iterations{0};
    std::thread worker([&] {
        self = &workerIdentity;
        s_pollTask = self;
        while (!stop.load()) {
            if (pauseAtIdleCheckpoint()) continue;
            busy = true;
            s_syncNotify = true; // Same publication as the real poll loop.
            std::this_thread::yield();
            busy = false;
            ++iterations;
        }
    });
    while (!s_pauseAcknowledged) std::this_thread::yield();
    assert(iterations == 0 && !busy);
    resumePolling();
    for (unsigned i=0; i<2000; ++i) {
        assert(suspendPolling(2000));
        assert(!busy);
        const auto before = iterations.load();
        std::this_thread::yield();
        assert(iterations == before && !busy);
        resumePolling();
        (void)takeSyncNotification();
    }
    stop = true;
    resumePolling();
    xTaskNotifyGive(&workerIdentity);
    worker.join();
    // Simulate a published worker stuck in I/O: timeout must undo admission.
    assert(!suspendPolling(1));
    assert(!s_pauseRequested);
    resumePolling(); // Idempotent release.
    s_syncNotify = true;
    assert(takeSyncNotification());
    assert(!takeSyncNotification());
    // Concurrent publication/read-clear: exercise the actual consumer under TSan.
    std::thread publisher([] {
        for (unsigned i=0; i<100000; ++i) s_syncNotify = true;
    });
    for (unsigned i=0; i<100000; ++i) (void)takeSyncNotification();
    publisher.join();
    s_syncNotify = true;
    assert(takeSyncNotification() && !takeSyncNotification());
    puts("PASS startup pause, 2000 handoffs, timeout rollback, notification consumption");
}
'''
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        cpp = directory/'main.cpp'
        cpp.write_text(harness + state + functions + cases)
        executable = directory/'check'
        subprocess.run([shutil.which('clang++') or 'c++', '-std=c++17', '-O1', '-g',
                        '-fsanitize=thread', '-pthread', str(cpp), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == '__main__':
    main()
