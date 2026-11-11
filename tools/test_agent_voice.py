"""Compile the voice session against faultable microphone, memory and policies."""
from pathlib import Path
import tempfile
import shutil
import subprocess
ROOT=Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory() as directory:
        temp=Path(directory)
        def write(name,text):
            p=temp/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
        write('Arduino.h','#pragma once\n#include <stdint.h>\ninline uint32_t millis(){static uint32_t now=0;return now+=10;}\ninline void vTaskDelay(unsigned){}\n#define pdMS_TO_TICKS(x) (x)\nstruct SerialStub { void println(const char*) {} template<class... T> void printf(const char*,T...){} }; extern SerialStub Serial;')
        write('core/menu_config.h','#define BLE_DEVICE_NAME "test"')
        write('core/memory.h','namespace core { namespace memory { inline void logSnapshot(const char*) {} } }')
        write('hal/hal_audio.h','namespace hal { namespace audio { void restoreForUiPlayback(); } }')
        write('hal/hal_ble.h','namespace hal { namespace ble { bool started(); void deinit(); bool init(const char*); } }')
        write('hal/hal_wifi.h','namespace hal { namespace wifi { bool connected(); bool connect(); void applyModemSleepPolicy(); } }')
        write('services/agent_transport.h','namespace services { namespace agent_transport { void detachAudio(); } }')
        write('services/background_network.h','namespace services { namespace background_network { struct AudioSuspension {bool uploadsReady;}; AudioSuspension suspendForKamiVoice(); void resumeAfterKamiVoice(); bool kamiVoiceNetworkReady(); } }')
        write('core/agent_slab.h',r'''
#pragma once
#include <stdint.h>
extern bool allocationOk,detached; extern int allocations;
#include <cassert>
namespace core {
struct AgentSlab {
 int16_t value=0; bool held=false;
 bool acquire() { ++allocations; held=allocationOk; if(held)detached=false; return held; }
 void release() { assert(detached); held=false; }
 int16_t* pcm() { return held ? &value : nullptr; }
 uint32_t pcmMax() { return held ? 100 : 0; }
}; }
''')
        write('services/mic_capture.h',r'''
#pragma once
#include <stdint.h>
extern bool micOk; extern int starts,stops;
namespace core { namespace audio {struct RecordingWaveform {};}}
namespace services { namespace audio {
using core::audio::RecordingWaveform;
struct MicCapture {
 using SampleSink=bool(*)(const int16_t*,uint32_t,void*);
 uint32_t n=0; RecordingWaveform wave;
 bool startStream(SampleSink,void*) { ++starts; n=0; return micOk; }
 void tick() { n+=160; }
 void stop() { ++stops; }
 uint32_t samplesN() const { return n; }
 const RecordingWaveform& waveform() const { return wave; }
}; } }
''')
        write('main.cpp',r'''
#include "services/agent_voice.h"
#include "hal/audio_retirement.h"
#include "services/background_network.h"
#include <Arduino.h>
#include <cassert>
SerialStub Serial;
bool detached=true;
bool allocationOk=true,micOk=true,backgroundOk=true,wifi=false,ble=true;
int allocations=0,starts=0,stops=0,pauses=0,resumes=0,bleStarts=0,restores=0;
namespace hal {
namespace audio { void restoreForUiPlayback(){++restores;} }
namespace ble { bool started(){return ::ble;} void deinit(){::ble=false;} bool init(const char*){++bleStarts;::ble=true;return true;} }
namespace wifi { bool connected(){return ::wifi;} bool connect(){return true;} void applyModemSleepPolicy(){} }
}
namespace services { namespace agent_transport { void detachAudio(){detached=true;} } namespace background_network {
AudioSuspension suspendForKamiVoice(){++pauses;return {backgroundOk};}
void resumeAfterKamiVoice(){++resumes;}
bool kamiVoiceNetworkReady(){return true;}
} }
bool sink(const int16_t*,uint32_t,void*){return true;}
int main(){
 namespace voice=services::agent_voice; using Result=voice::StartResult;
 {hal::audio::RetirementBlock retiring;retiring.acquire();
 assert(!voice::prepare() && pauses==0 && resumes==0 && allocations==0 && starts==0 && ble);}
 backgroundOk=false; assert(!voice::prepare()); assert(ble && starts==0);
 backgroundOk=true; allocationOk=false;
 assert(voice::prepare()); assert(!ble);
 assert(voice::start(sink)==Result::MemoryUnavailable);
 assert(ble && resumes==1 && starts==0);
 allocationOk=true;micOk=false;
 assert(voice::prepare());assert(voice::start(sink)==Result::MicUnavailable);
 assert(!voice::pcm() && resumes==2);
 assert(voice::finish()==0 && stops==0);
 micOk=true;
 assert(voice::prepare()); assert(voice::start(sink)==Result::Started);
 const int started=starts, allocated=allocations;
 assert(voice::start(sink)==Result::Started && starts==started && allocations==allocated);
 voice::tick();assert(voice::samples()==160);
 voice::requestRelease();voice::poll();assert(voice::pcm() && resumes==2);
 // Request is deferred while microphone is producing into the ring.
 voice::finish();assert(stops==1 && !voice::pcm() && resumes==3);
 assert(voice::prepare());assert(voice::start(sink)==Result::Started);
 voice::tick();
 assert(voice::finish()==160 && stops==2 && voice::pcm());
 assert(voice::finish()==160 && stops==2); // Regression: never stop MicCapture twice.
 voice::tick();assert(voice::samples()==160);
 wifi=true;const int bleBefore=bleStarts;
 voice::release();voice::release();assert(resumes==4 && bleStarts==bleBefore);
 assert(!voice::pcm() && voice::capacity()==0);
 voice::resetForEntry();assert(voice::samples()==0);
 voice::finish();assert(stops==2);
 voice::restorePlayback();assert(restores==1);
}
''')
        subprocess.run([shutil.which('c++') or 'c++','-std=c++11','-I'+str(temp),'-I'+str(ROOT/'include'),str(ROOT/'src/services/agent_voice.cpp'),str(temp/'main.cpp'),str(ROOT/'src/hal/audio_retirement.cpp'),'-o',str(temp/'check')],check=True)
        subprocess.run([str(temp/'check')],check=True)
    print('PASS voice acquisition rollback, BLE policy, retained PCM, repeated start/stop and cleanup')


if __name__=='__main__': main()
