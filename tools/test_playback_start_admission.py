"""Execute both real startTrack bodies with worker allocation failure injected.

File/codec/I2S fakes isolate the admission boundary; device tests cover decoding.
"""
from pathlib import Path
import subprocess
import tempfile
import sys
ROOT=Path(__file__).resolve().parents[1]
negative = '--negative-control' in sys.argv
def source(path):
    if negative:
        return subprocess.check_output(['git','show','63412728:'+path],cwd=ROOT,text=True)
    return (ROOT/path).read_text()
def function(text, signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
code=r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
bool spawnOk=false;int opens=0,txStarts=0,starts=0,usageEvents=0;
namespace core{namespace usage{void emit(const char*,const char*){++usageEvents;}}}
struct Log{void println(const char*){}template<class...T>void printf(const char*,T...) {}} Serial;
struct File{operator bool()const{return true;}unsigned size(){return 4096;}unsigned position(){return 0;}void close(){}};
struct Storage{File open(const char*,int=0){++opens;return {};}} SD;
namespace hal{namespace sd{bool openStream(const char*,File* f){++opens;*f={};return true;}}}
#define FILE_READ 0
#define MINIMP3_MAX_SAMPLES_PER_FRAME 4
namespace hal{namespace bus{void lockSpi(){}void unlockSpi(){}}namespace audio{bool txBegin(uint32_t){++txStarts;return true;}}}
struct mp3dec_frame_info_t{int hz=48000,channels=2,bitrate_kbps=64;};
struct Ring{void* buf=(void*)1;int free(){return 0;}void reset(){}};
namespace core{namespace dsp{template<class...T>void pushStereoWithVolumeBasisPoints(T...) {}}}
struct Codec{void reset(){}void fillInput(File&){}int decodeFrame(int16_t*,int,mp3dec_frame_info_t* p){*p={};return 0;}};
struct Progress{void reset(){}void add(int,int){}};
struct Worker{bool runningFlag=false,pending=false;bool running()const{return runningFlag;}bool cleanupPending()const{return pending;}};
struct PlaybackWorker{struct Control{Control(Worker&){}explicit operator bool()const{return true;}};};
struct Base{
 Worker worker_;Ring ring_;Codec codec_;Progress progress_,outputProgress_;File file_;
 struct Avi{} avi_;unsigned aviChunkEnd_=0,aviSamplesRead_=0,outputSampleRemainder_=0;
 std::atomic<bool> streamFailed_{false};
 void* inBuf_=(void*)1;int16_t decodeBuf_[4]{};
 std::atomic<bool> playing_{false};bool fileOpen_=false,backgroundIoPaused_=true,usageTrackActive_=false;
 char currentPath_[128]{};unsigned fileSize_=0,audioStart_=0,lastGoodFilePos_=0,decodeNoProgressCount_=0,detectedRate_=0,estBitrate_=0;
 unsigned dataStart_=0,dataEnd_=0,bytesPerFrame_=0;bool wantsNext_=false;
 struct Info{int hz=48000,channels=2;unsigned bitrateKbps=64,durationMs=1000;char title[64]{};} info_;
 struct ParsedHeader{Avi avi;int hz=48000,channels=2;unsigned durationMs=1000,dataStart=44,dataEnd=4096,bytesPerFrame=4;};
 bool startTask(){++starts;return worker_.runningFlag=spawnOk&&!worker_.pending;}
 void stopTrackUnlocked(){playing_=false;fileOpen_=false;}
 bool parseID3_(){return true;}int decodeOneFrame_(){return 0;}int scaledVolumeBasisPoints(){return 1000;}
 bool ready(){return true;}bool parseHeader_(File&,ParsedHeader&){return true;}
 void clearSpectrum(){}bool pumpFileToRing_(){return false;}
};
void nameFromPath(const char*,char* out,unsigned size){snprintf(out,size,"fixture");}
'''
for cls,file in [('Mp3Player','mp3'),('WavPlayer','wav')]:
    header=source(f'include/services/{file}_player.h')
    code+=f'struct {cls}:Base{{bool startTrack(const char*);'+(function(header,'void resume()') if negative else 'void resume();')+'};\n'
    if not negative:code+=function(source(f'src/services/{file}_player.cpp'),f'void {cls}::resume()')+'\n'
    code+=function(source(f'src/services/{file}_player.cpp'),f'bool {cls}::startTrack(')+'\n'
code+=r'''
template<class Player>void check(){
 Player p;spawnOk=false;opens=txStarts=starts=usageEvents=0;
 assert(!p.startTrack("/fixture")&&!p.playing_&&!p.fileOpen_);
 assert(starts==1&&opens==0&&txStarts==0&&usageEvents==0);
 // A later user selection may recover after allocation becomes possible.
 spawnOk=true;assert(p.startTrack("/fixture")&&p.playing_&&p.fileOpen_);
 assert(starts==2&&opens==1&&txStarts==1&&usageEvents==1);
 p.playing_=false;p.worker_.runningFlag=false;p.resume();assert(!p.playing_);
 p.worker_.runningFlag=true;p.worker_.pending=true;p.resume();assert(!p.playing_);
 p.worker_.pending=false;p.fileOpen_=false;p.resume();assert(!p.playing_);
 assert(usageEvents==1);p.fileOpen_=true;p.resume();assert(p.playing_&&usageEvents==2);
 p.resume();assert(usageEvents==2);
}
int main(){check<Mp3Player>();check<WavPlayer>();puts("PASS actual MP3/WAV startTrack: failed admission opens no file, starts no I2S, never plays; later retry and resume guards");}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory);(path/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++14','-fsanitize=address,undefined',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    result=subprocess.run([str(path/'test')],capture_output=True,text=True)
    if negative:
        assert result.returncode != 0 and 'p.startTrack' in result.stderr, result.stdout+result.stderr
        print('PASS negative control: actual pre-fix source falsely starts playback after failed worker allocation')
    else:
        result.check_returncode();print(result.stdout,end='')
