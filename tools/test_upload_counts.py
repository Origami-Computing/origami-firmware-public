"""Exercise actual queue count/debug methods with a contended host mutex.

Network/storage methods are omitted; this tests snapshot ownership and status
semantics, not FreeRTOS scheduling or complete queue thread safety.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/upload_queue.cpp').read_text()
guard = source[source.index('struct MutexGuard {'):source.index('}  // namespace')]
methods = source[source.index('UploadQueue::Counts UploadQueue::counts()'):source.index('void UploadQueue::setUploadPaused')]

methods += source[source.index('bool UploadQueue::workerRunning()'):source.index('void UploadQueue::notifyWorker()')]

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'freertos').mkdir()
    (root / 'Arduino.h').write_text('#pragma once\n#include <cstdint>\n#include <cstddef>\n')
    (root / 'freertos/FreeRTOS.h').write_text('''#pragma once
#include <mutex>
#include <atomic>
#include <cassert>
using TickType_t = unsigned;
constexpr unsigned portMAX_DELAY = ~0u;
constexpr int pdTRUE = 1;
struct TestMutex { std::mutex mutex; std::atomic<int> attempts{0}; };
using SemaphoreHandle_t = TestMutex*;
using TaskHandle_t = void*;
thread_local TestMutex* heldMutex = nullptr;
bool refuseLock = false;
int xSemaphoreTake(TestMutex* m, unsigned) {
    assert(heldMutex != m); // Fail immediately on recursive diagnostic locking.
    ++m->attempts;
    if (refuseLock) return 0;
    m->mutex.lock(); heldMutex=m; return 1;
}
void xSemaphoreGive(TestMutex* m) { assert(heldMutex==m); heldMutex=nullptr; m->mutex.unlock(); }
''')
    for name in ['semphr.h', 'task.h']:
        (root / 'freertos' / name).write_text('#include "FreeRTOS.h"\n')
    (root / 'freertos/portmacro.h').write_text('#pragma once\n#include <mutex>\nusing portMUX_TYPE=std::mutex;\n#define portMUX_INITIALIZER_UNLOCKED {}\n#define portENTER_CRITICAL(p) (p)->lock()\n#define portEXIT_CRITICAL(p) (p)->unlock()\n')
    code = r'''
#include <future>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <string>
#include "freertos/FreeRTOS.h"
#define private public
#include "upload_queue.h"
#undef private
#include "upload_queue_policy.h"
unsigned millis() { return 100; }
struct SerialStub {
    std::string text;
    void println(const char* s) { text+=s; text+='\n'; }
    void printf(const char* format,...) {
        char buffer[512]; va_list args; va_start(args,format);
        vsnprintf(buffer,sizeof(buffer),format,args); va_end(args); text+=buffer;
    }
} Serial;
'''+guard+methods+r'''
int main() {
    UploadQueue queue;
    assert(queue.counts().total==0); // Before setup / unavailable mutex.
    TestMutex mutex; queue.m_mutex=&mutex;
    assert(queue.counts().total==0); // Failed backing allocation.
    QueueItem items[4]{}; queue.m_items=items; queue.m_count=4;
    items[0].status=QSTATUS_PENDING; items[1].status=QSTATUS_FAILED;
    items[2].status=QSTATUS_UPLOADING; items[3].status=QSTATUS_COMPLETED;
    auto counts=queue.counts();
    assert(counts.total==4 && counts.pending==1 && counts.failed==1 && counts.active==1);
    assert(queue.size()==4 && queue.pendingCount()==1 && queue.failedCount()==1);
    queue.dumpDebugState(); // Already owns lock; must use unlocked aggregation.
    assert(Serial.text.find("count=4 pending=1 worker=0")!=std::string::npos);
    refuseLock=true; assert(queue.counts().total==0); refuseLock=false;

    // Hold a writer transaction half-finished. A reader must wait for its end.
    xSemaphoreTake(&mutex,portMAX_DELAY);
    items[0].status=QSTATUS_FAILED;
    const int attempts=mutex.attempts;
    auto read=std::async(std::launch::async,[&] { return queue.counts(); });
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(mutex.attempts==attempts) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::yield();
    }
    assert(read.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
    items[1].status=QSTATUS_PENDING; queue.m_count=2;
    xSemaphoreGive(&mutex);
    counts=read.get();
    assert(counts.total==2 && counts.pending==1 && counts.failed==1 && counts.active==0);
    assert(heldMutex==nullptr);
}
'''
    (root / 'test.cpp').write_text(code)
    binary = root / 'test'
    subprocess.run(['c++', '-std=c++11', '-pthread', '-fsanitize=address,undefined',
                    '-include', 'thread', '-I'+str(root), '-I'+str(ROOT/'include'),
                    str(root/'test.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS queue snapshot waits for writer, preserves status counts, handles unavailable storage/lock, and debug output avoids recursive locking')
