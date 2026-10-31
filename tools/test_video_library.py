"""Exercise production AVI prefetch/library with threaded HAL fakes and sanitizers."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('Arduino.h','#pragma once\n#include <thread>\n#include <chrono>\n#define pdMS_TO_TICKS(x) (x)\ninline unsigned millis(){return unsigned(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}\ninline void vTaskDelay(unsigned n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}\n')
    write('esp_heap_caps.h','#pragma once\n#include <stddef.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\nvoid* heap_caps_malloc(size_t,int);void heap_caps_free(void*);\n')
    write('hal/hal_tasks.h','#pragma once\nnamespace hal {namespace tasks {enum class Id{VideoRead};void* spawn(Id,void(*)(void*),void*);void recordCurrentStack(Id);void deleteCurrent(Id);}}')
    write('hal/hal_wifi.h','#pragma once\nextern int radios;namespace hal {namespace wifi {inline void stop(){++radios;}inline void scheduleRestore(){--radios;}}}')
    write('hal/hal_sd.h','''#pragma once
#include <stdint.h>
#include <stddef.h>
namespace hal {namespace sd {
struct StreamReader {void* handle=nullptr;size_t size=0,position=0;};
bool mounted();int listFiles(const char*,const char*,char*,int,int);
bool openStream(const char*,StreamReader*);size_t readStream(StreamReader*,uint8_t*,size_t);bool seekStream(StreamReader*,size_t);void closeStream(StreamReader*);
}}
''')
    write('services/peripheral_reservation.h','''#pragma once
extern bool admission;extern int leases;
namespace services {namespace background_network {
class PeripheralReservation {bool held_=false;public:
enum class Result {Acquired,PeripheralBusy,Pending};
Result pollAcquire(bool pause){return admission?acquire(pause,0):Result::Pending;}
Result acquire(bool pause,unsigned){if(!pause||!admission)return Result::PeripheralBusy;if(!held_)++leases;held_=true;return Result::Acquired;}
bool held()const{return held_;}void release(){if(held_)--leases;held_=false;}
};}}
''')
    write('services/wav_player.h',"""#pragma once
#include <cassert>
extern int leases;
namespace services {namespace audio {class WavPlayer {
 bool open_=false;
 public:bool openSession(){assert(leases==1);return open_=true;}
 void closeSession(){if(open_)assert(leases==1);open_=false;}
 bool sessionOpen()const{return open_;}bool startTrack(const char*){return open_;}
 void pause(){}void resume(){}bool failed()const{return false;}unsigned outputElapsedMs()const{return 0;}
};}}
""")
    exe=root/'test'
    subprocess.run(['c++','-std=c++17','-g','-fsanitize=address,undefined','-pthread','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/services/video_library.cpp'),str(ROOT/'tests/video/library.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
