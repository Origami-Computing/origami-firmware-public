"""Exercise the real transport pause protocol with a deterministic worker clock."""
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory)
        def write(name, text):
            p = temp/name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
        write('Arduino.h', '''#pragma once
#include <stdint.h>
uint32_t millis();
struct SerialStub { template<class... T> void printf(const char*,T...) {} };
extern SerialStub Serial;
''')
        write('freertos/FreeRTOS.h', '#define pdMS_TO_TICKS(x) (x)\n')
        write('freertos/task.h', '#include <stdint.h>\nusing TaskHandle_t=void*;\nTaskHandle_t xTaskGetCurrentTaskHandle();\nvoid vTaskDelay(uint32_t);\n')
        write('esp_arduino_version.h', '#define ESP_ARDUINO_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n#define ESP_ARDUINO_VERSION ESP_ARDUINO_VERSION_VAL(2,0,17)\n')
        write('mbedtls/ssl.h', '#pragma once\nstruct mbedtls_ssl_context {};\n#define MBEDTLS_ERR_SSL_INTERNAL_ERROR -0x6C00\n')
        write('main.cpp', r'''
#include "services/agent_coordination.h"
#include <Arduino.h>
#include <cassert>
#include <initializer_list>
#include <functional>
#include <type_traits>
#include <mbedtls/ssl.h>
bool pollCancel=false;
namespace services {namespace net_poll {bool cancelHandshakeForCurrentTask(){return pollCancel;}}}
namespace control=services::agent_coordination;
SerialStub Serial;
uint32_t now=0; bool workerResponds=false;
std::function<void()> duringWait;
int workerIdentity, otherIdentity;
void* task=&otherIdentity;
void* xTaskGetCurrentTaskHandle() { return task; }
int calls=0, result=0;
mbedtls_ssl_context context;
extern "C" int __wrap_mbedtls_ssl_handshake(mbedtls_ssl_context*);
extern "C" int __real_mbedtls_ssl_handshake(mbedtls_ssl_context* ssl) {
    assert(ssl==&context); ++calls; return result;
}
void checkHandshake(bool cancel) {
    for (int code : {0, -0x6900, -0x6880, -0x7000}) {
        result=code; const int before=calls;
        assert(__wrap_mbedtls_ssl_handshake(&context)==(cancel?MBEDTLS_ERR_SSL_INTERNAL_ERROR:code));
        assert(calls==before+(cancel?0:1));
    }
}
uint32_t millis() { return now; }
void vTaskDelay(uint32_t ms) {
    now+=ms;
    if(duringWait){auto callback=duringWait;duringWait=nullptr;callback();}
    if (control::suspendRequested()) {
        task=&otherIdentity; checkHandshake(false);
        task=&workerIdentity; checkHandshake(true);
        // Cancellation alone is not an acknowledgment: the nonresponding
        // case must still time out until worker cleanup explicitly acks.
        if (workerResponds) control::acknowledgeSuspended();
        task=&otherIdentity;
    }
}
int main() {
    using Pause=control::TransportPause;
    static_assert(!std::is_copy_constructible<Pause>::value,"pause ownership is unique");
    static_assert(!std::is_move_constructible<Pause>::value,"addresses retain ownership");
    {
        Pause beforeStart;
        assert(beforeStart.acquire(0));
        assert(beforeStart.held()&&control::suspendRequested());
        // Starting a worker later still sees the retained request before sockets.
        control::workerStarted();
        assert(control::suspendRequested());
    }
    assert(!control::suspendRequested());
    assert(!control::isSessionActive());
    control::setSessionActive(true); assert(control::isSessionActive());
    control::setSessionActive(false); assert(!control::isSessionActive());
    checkHandshake(false); // No registered worker.
    pollCancel=true; checkHandshake(true);
    pollCancel=false; checkHandshake(false);
    task=&workerIdentity; control::bindCurrentWorker(); checkHandshake(false);
    task=&otherIdentity;
    {
      Pause pending;
      const auto before=now;
      assert(!pending.pollAcquire() && pending.held() && control::suspendRequested());
      assert(!pending.pollAcquire() && now==before);
      pending.release();assert(!control::suspendRequested());
      assert(!pending.pollAcquire());control::acknowledgeSuspended();
      assert(pending.pollAcquire() && now==before);
    }
    assert(!control::suspendRequested());
    Pause first,second,unused;
    assert(!first.acquire(20)); // Never acknowledge.
    assert(now==20&&!first.held()&&!control::suspendRequested());
    task=&workerIdentity; checkHandshake(false); task=&otherIdentity;
    workerResponds=true;
    assert(first.acquire(20));
    const auto acknowledgedAt=now;
    assert(first.acquire(0)); // Same owner does not add another claim.
    assert(second.acquire(0)&&now==acknowledgedAt); // Existing cleanup is shared.
    unused.release();assert(control::suspendRequested());
    first.release();first.release();assert(control::suspendRequested());
    // Ownership may be passed sequentially to a different cleanup task.
    task=&workerIdentity;second.release();checkHandshake(false);task=&otherIdentity;
    assert(!control::suspendRequested());
    // A new first owner must wait, even if the worker never observed the gap.
    workerResponds=false;
    assert(!first.acquire(20));assert(!control::suspendRequested());
    control::acknowledgeSuspended(); // Late cleanup with no owner leaves no stale ack.
    assert(!first.acquire(0));
    now=UINT32_MAX-5;
    assert(!first.acquire(20)); // millis rollover.
    assert(now==14&&!control::suspendRequested());
    // Timeout of a second, unacknowledged owner cannot cancel the first wait.
    duringWait=[&]{
        assert(control::suspendRequested());
        assert(!second.acquire(0)&&!second.held());
        second.release();assert(control::suspendRequested());
        workerResponds=true;
    };
    assert(first.acquire(20)&&first.held());
    first.release();assert(!control::suspendRequested());
    for(int cycle=0;cycle<200;++cycle){
        assert(first.acquire(20));
        {Pause nested;assert(nested.acquire(0));first.release();assert(control::suspendRequested());}
        assert(!control::suspendRequested());
        workerResponds=false;assert(!first.acquire(0));workerResponds=true;
    }
}

''')
        subprocess.run([shutil.which('c++') or 'c++', '-std=c++11', '-fsanitize=address,undefined', '-I'+str(temp), '-I'+str(ROOT/'include'),
                        str(ROOT/'src/services/agent_coordination.cpp'), str(ROOT/'src/services/agent_tls_handshake.cpp'), str(temp/'main.cpp'), '-o', str(temp/'check')], check=True)
        subprocess.run([str(temp/'check')], check=True)
        write('concurrent.cpp', r'''
#include "services/agent_coordination.h"
#include <Arduino.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>
namespace services {namespace net_poll {bool cancelHandshakeForCurrentTask(){return false;}}}
namespace control=services::agent_coordination;
SerialStub Serial;
const auto epoch=std::chrono::steady_clock::now();
uint32_t millis(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-epoch).count();}
void vTaskDelay(uint32_t){std::this_thread::yield();}
void* xTaskGetCurrentTaskHandle(){return nullptr;}
int main(){
    std::atomic<bool> done{false},socketWork{false};
    control::workerStarted();
    std::thread worker([&]{
        while(!done.load()){
            if(control::suspendRequested()){
                // Models cleanup before ack and no socket calls in this branch.
                assert(!socketWork.load());
                control::acknowledgeSuspended();
            }else{
                socketWork.store(true);
                std::this_thread::yield();
                socketWork.store(false);
            }
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> clients;
    for(int task=0;task<4;++task)clients.emplace_back([&]{
        for(int cycle=0;cycle<2000;++cycle){
            control::TransportPause first,second;
            assert(first.acquire(5000));
            assert(control::suspendRequested()&&!socketWork.load());
            assert(second.acquire(0));
            first.release();first.release();
            std::this_thread::yield();
            assert(control::suspendRequested()&&!socketWork.load());
        }
    });
    for(auto& client:clients)client.join();
    assert(!control::suspendRequested());
    done.store(true);worker.join();
}
''')
        subprocess.run([shutil.which('c++') or 'c++', '-std=c++11', '-pthread', '-fsanitize=address,undefined',
                        '-I'+str(temp), '-I'+str(ROOT/'include'), str(ROOT/'src/services/agent_coordination.cpp'),
                        str(temp/'concurrent.cpp'), '-o', str(temp/'concurrent')], check=True)
        subprocess.run([str(temp/'concurrent')], check=True, timeout=30)

    print('PASS 8,000 concurrent owner cycles with modeled socket worker under sanitizers')
    print('PASS owned transport pauses: startup, idempotence, overlap, timeout isolation, cross-task release, fresh acknowledgment, rollover, destructor cleanup and scoped TLS cancellation under sanitizers')


if __name__ == '__main__':
    main()
