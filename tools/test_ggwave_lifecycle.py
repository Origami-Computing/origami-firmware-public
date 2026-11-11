"""Compile the real receiver with injected failures and verify radio/audio ownership."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    t = Path(directory)
    def write(name, text):
        p = t/name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    write('Arduino.h', r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <mutex>
#include <thread>
#include <chrono>
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
#define portEXIT_CRITICAL(m) (m)->unlock()
#define pdMS_TO_TICKS(x) (x)
inline void vTaskDelay(int){std::this_thread::sleep_for(std::chrono::milliseconds(1));}
inline uint32_t micros(){return 0;}
struct Logger{template<class... A>void printf(const char*,A...) {}};extern Logger Serial;
extern int failAt;extern bool radioDown,mic;extern int stops,restores,releases;
''')
    write('services/peripheral_reservation.h', r'''
#pragma once
#include "Arduino.h"
namespace services { namespace background_network {
class PeripheralReservation {bool held=false;public:
enum class Result {Acquired,PeripheralBusy};
Result acquire(bool){if(failAt==1)return Result::PeripheralBusy;held=true;return Result::Acquired;}
void release(){if(held){held=false;++releases;}}
};}}
''')
    write('hal/hal_wifi.h', r'''
#pragma once
#include "Arduino.h"
#include <cassert>
namespace hal { namespace wifi {
inline bool suspended(){return radioDown;}
inline void stop(){assert(!mic);radioDown=true;++stops;}
inline bool restore(){assert(!mic);radioDown=false;++restores;return true;}
}}
''')
    write('hal/hal_audio.h', r'''
#pragma once
#include "Arduino.h"
#define ESP_OK 0
namespace hal { namespace audio {
inline bool codecReady(){return failAt!=4;}
inline void hardwareReset(){}
inline bool initCodec(){return failAt!=4;}
inline bool initMic(bool){mic=true;return failAt!=5;}
inline void endMicSession(){mic=false;}
inline int readMic(void*,size_t,size_t* bytes,int){*bytes=0;return 0;}
}}
''')
    write('hal/hal_tasks.h', r'''
#pragma once
#include "Arduino.h"
extern std::thread workerThread;
namespace hal {namespace tasks {
enum class Id{GgWave};
inline void recordCurrentStack(Id){}
inline void deleteCurrent(Id){}
inline void* spawn(Id,void(*fn)(void*)){if(failAt==6)return nullptr;workerThread=std::thread(fn,nullptr);return reinterpret_cast<void*>(1);}
}}
''')
    write('esp_heap_caps.h', r'''
#pragma once
#include <cstdlib>
#include "Arduino.h"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
inline void* heap_caps_calloc(size_t n,size_t s,int){return failAt==3?nullptr:calloc(n,s);}
inline void heap_caps_free(void* p){free(p);}
''')
    write('ggwave/ggwave.h', r'''
#pragma once
#include "Arduino.h"
#include <array>
#define GGWAVE_PROTOCOL_AUDIBLE_NORMAL 0
#define GGWAVE_PROTOCOL_AUDIBLE_FAST 1
#define GGWAVE_PROTOCOL_AUDIBLE_FASTEST 2
#define GGWAVE_PROTOCOL_ULTRASOUND_NORMAL 3
#define GGWAVE_PROTOCOL_ULTRASOUND_FAST 4
#define GGWAVE_PROTOCOL_ULTRASOUND_FASTEST 5
#define GGWAVE_PROTOCOL_CUSTOM_0 6
#define GGWAVE_SAMPLE_FORMAT_I16 1
#define GGWAVE_OPERATING_MODE_RX 1
struct GGWave {
using ProtocolId=int;struct TxRxData{};
struct Protocol{int freqStart=0;};
struct Protocols{Protocol p[9];void disableAll(){}void toggle(int,bool){}
Protocol& operator[](int i){return p[i];}static Protocols& rx(){static Protocols r;return r;}};
struct Parameters{int sampleRateInp,sampleFormatInp,operatingMode;};
static void setLogFile(void*){}static Parameters getDefaultParameters(){return {};}
bool prepare(Parameters){return failAt!=2;}int heapSize(){return 0;}
void decode(void*,size_t){}bool rxReceiving(){return false;}bool rxAnalyzing(){return false;}
std::array<float,512> spectrum{};const auto& rxSpectrum(){return spectrum;}
int rxDataLength(){return 0;}const char* rxData(){return "";}int rxProtocolId(){return 0;}
void rxTakeData(TxRxData&){}
};
''')
    write('main.cpp', r'''
#include "Arduino.h"
#include "services/ggwave_receiver.h"
#include <cassert>
#include <cstdio>
Logger Serial;int failAt=0,stops=0,restores=0,releases=0;bool radioDown=false,mic=false;
std::thread workerThread;
void finish(){services::ggwave_receiver::stop();if(workerThread.joinable())workerThread.join();}
int main(){using namespace services::ggwave_receiver;
for(int f=1;f<=6;++f){
 failAt=f;stops=restores=releases=0;assert(!start(true));finish();finish();
 assert(!mic&&!radioDown&&!snapshot().listening);
 assert(stops==(f==1?0:1)&&restores==stops&&releases==stops);
}
failAt=0;
for(int i=0;i<100;++i){
 stops=restores=releases=0;assert(start(true));assert(start(true));
 assert(radioDown&&mic&&snapshot().listening&&stops==1);
 finish();finish();assert(!mic&&!radioDown&&restores==1&&releases==1);
}
// A pre-existing radio suspension belongs to another owner.
radioDown=true;stops=restores=0;assert(start(true));finish();
assert(radioDown&&stops==0&&restores==0);
puts("PASS receiver rollback at six startup boundaries, 100 start/stop cycles, idempotency, and borrowed radio pause");
}
''')
    subprocess.run(['c++', '-std=c++14', '-pthread', '-fsanitize=address,undefined',
                    '-g', '-I'+str(t), '-I'+str(ROOT/'include'), str(t/'main.cpp'),
                    str(ROOT/'src/services/ggwave_receiver.cpp'), '-o', str(t/'test')], check=True)
    subprocess.run([str(t/'test')], check=True)
