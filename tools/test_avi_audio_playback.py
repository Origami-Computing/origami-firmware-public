"""Exercise actual WAV/AVI header parsing and PCM delivery into the production ring."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/services/wav_player.cpp').read_text()
header=(ROOT/'include/services/wav_player.h').read_text()
def function(text,signature):
 a=text.index(signature);b=text.index('{',a)+1;depth=1
 while depth:depth+=(text[b]=='{')-(text[b]=='}');b+=1
 return text[a:b]
code=r'''
#include "core/mjpeg_avi.h"
#include "core/audio_dsp.h"
#include "core/playback_progress.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
#include <iterator>
using Bytes=std::vector<uint8_t>;
Bytes data;int locks=0,seeks=0,readLimit=100000;bool failReopen=false;
constexpr int FILE_READ=0;
struct SerialStub {template<class... T>void printf(const char*,T...) {}} Serial;
namespace hal {namespace bus {void lockSpi(){assert(!locks);++locks;}void unlockSpi(){assert(locks);--locks;}}}
namespace hal {namespace sd {struct StreamReader {
 void* handle=reinterpret_cast<void*>(1);uint32_t position=0;size_t size=data.size();
};
size_t readStream(StreamReader* f,uint8_t* p,size_t n){assert(locks&&f->handle);n=std::min(n,data.size()-f->position);n=std::min(n,size_t(readLimit));memcpy(p,data.data()+f->position,n);f->position+=n;return n;}
bool seekStream(StreamReader* f,size_t p){assert(locks);++seeks;if(p>data.size())return false;f->position=p;return true;}
void closeStream(StreamReader* f){assert(locks);f->handle=nullptr;f->position=f->size=0;}
bool openStream(const char*,StreamReader* f){assert(locks);*f=StreamReader{};if(failReopen)f->handle=nullptr;return f->handle;}
}}
'''
code+=function(source,'uint16_t rd16(')+function(source,'uint32_t rd32(')
code+='class WavPlayer {public:\n'+function(header,'struct ParsedHeader')+';\n'
code+=r'''
 static bool parseHeader_(hal::sd::StreamReader&,ParsedHeader&);
 bool prepareAviAudioChunk_();bool failAviStream_();bool pumpFileToRing_();bool atEndOfFile_();bool recoverSdFile_();
 hal::sd::StreamReader file_;bool fileOpen_=true;char currentPath_[192]="/test.avi";
 uint32_t dataStart_=0,dataEnd_=0,bytesPerFrame_=2,lastGoodFilePos_=0;
 core::avi::Info avi_{};uint32_t aviChunkEnd_=0,aviSamplesRead_=0;
 std::atomic<bool> streamFailed_{false},playing_{true};
 struct {int hz=48000,channels=1;} info_;
 core::audio::PlaybackProgress progress_;
 core::dsp::PcmRing ring_{};int16_t pcm[1024],storage[8192];int16_t* pcmBuf_=pcm;int pcmBufSamples_=1024;
 WavPlayer(){ring_.buf=storage;ring_.cap=8192;ring_.reset();}
 uint32_t elapsedMs(){return progress_.elapsedMs();}
 int scaledVolumeBasisPoints(){return 10000;}
 void analyzeVis_(const int16_t*,int,int){}
 bool open(){ParsedHeader p;hal::bus::lockSpi();bool ok=parseHeader_(file_,p);hal::bus::unlockSpi();if(!ok)return false;
  info_.hz=p.hz;info_.channels=p.channels;dataStart_=p.dataStart;dataEnd_=p.dataEnd;bytesPerFrame_=p.bytesPerFrame;
  avi_=p.avi;aviChunkEnd_=dataStart_;lastGoodFilePos_=dataStart_;return true;
 }
};
'''
for sig in ['bool WavPlayer::parseHeader_(', 'bool WavPlayer::recoverSdFile_(', 'bool WavPlayer::atEndOfFile_(', 'bool WavPlayer::failAviStream_(', 'bool WavPlayer::prepareAviAudioChunk_(', 'bool WavPlayer::pumpFileToRing_(']:code+=function(source,sig)+'\n'
code+=r'''
Bytes readFile(const char* path){std::ifstream f(path,std::ios::binary);return Bytes((std::istreambuf_iterator<char>(f)),{});}
Bytes play(WavPlayer& p){Bytes out;for(int loop=0;loop<1000;++loop){
 p.pumpFileToRing_();int16_t samples[2048];int n=p.ring_.pop(samples,2048);
 for(int i=0;i<n;i+=2){assert(samples[i]==samples[i+1]);out.push_back(uint8_t(samples[i]));out.push_back(uint8_t(uint16_t(samples[i])>>8));}
 if(p.streamFailed_ || (p.atEndOfFile_()&&!p.ring_.used()))break;
}return out;}
int main(int argc,char** argv){assert(argc==3);Bytes original=readFile(argv[1]),expected=readFile(argv[2]);data=original;
 {WavPlayer p;assert(p.open());assert(play(p)==expected&&!p.streamFailed_&&p.elapsedMs()==800);}
 // Preserve ordinary PCM WAV delivery as well as AVI delivery.
 data.assign(44+expected.size(),0);memcpy(data.data(),"RIFF",4);core::avi::u32(data.data()+4,data.size()-8);
 memcpy(data.data()+8,"WAVEfmt ",8);core::avi::u32(data.data()+16,16);core::avi::u32(data.data()+20,0x00010001);
 core::avi::u32(data.data()+24,48000);core::avi::u32(data.data()+28,96000);core::avi::u32(data.data()+32,0x00100002);
 memcpy(data.data()+36,"data",4);core::avi::u32(data.data()+40,expected.size());memcpy(data.data()+44,expected.data(),expected.size());
 {WavPlayer p;assert(p.open());assert(play(p)==expected&&!p.streamFailed_&&p.elapsedMs()==800);}
 // Reopen preserves physical chunk position; PCM is neither repeated nor lost.
 data=original;{WavPlayer p;assert(p.open());p.file_.handle=nullptr;assert(play(p)==expected&&!p.streamFailed_);}
 // Valid header with inconsistent stream length cannot silently truncate audio.
 for(uint32_t samples:{38399u,38401u}){data=original;core::avi::u32(data.data()+264,samples);
  WavPlayer p;assert(p.open());play(p);assert(p.streamFailed_&&!p.playing_);
 }
 data=original;{WavPlayer p;assert(p.open());readLimit=1;play(p);assert(p.streamFailed_&&!p.playing_);readLimit=100000;}
 // Skipping many video chunks is bounded per pump and can resume on the next iteration.
 data.assign(324+21*12,0);
 for(int i=0;i<21;++i)core::avi::chunk(data.data()+324+i*12,4,i==20);
 {WavPlayer p;p.avi_.headerBytes=324;p.avi_.movieEnd=data.size();p.avi_.audioSamples=2;p.dataStart_=324;p.dataEnd_=data.size();p.aviChunkEnd_=324;p.file_.position=324;
  seeks=0;hal::bus::lockSpi();assert(!p.prepareAviAudioChunk_()&&!p.streamFailed_&&seeks==16);assert(p.prepareAviAudioChunk_()&&seeks==20);hal::bus::unlockSpi();
 }
 assert(!locks);
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code);(p/'frame.jpg').write_bytes(b'\xff\xd8\x42\xff\xd9')
 common=['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include')]
 subprocess.run(common+[str(ROOT/'tests/video/container.cpp'),'-o',str(p/'fixture')],check=True)
 subprocess.run([str(p/'fixture'),str(p/'frame.jpg'),str(p/'clip.avi'),str(p/'expected.pcm')],check=True)
 subprocess.run(common+[str(p/'test.cpp'),str(ROOT/'src/core/pcm_ring.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p/'clip.avi'),str(p/'expected.pcm')],check=True)
print('PASS production PCM playback: WAV/AVI parity, exact samples, bounded interleave scan, reopen and malformed-stream failure')
