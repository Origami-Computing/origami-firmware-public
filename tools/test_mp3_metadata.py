"""Run the real MP3 metadata/start/seek methods and decoder against synthetic files.

SD/worker/I2S fakes isolate metadata offsets and failed-start cleanup. Run with
--negative-control to put the decoder back at the tag and prove the regression
check fails. No downloaded music or hardware is required.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/services/mp3_player.cpp').read_text()

def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

fs = r'''
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
namespace fs {
class File {
public:
 std::shared_ptr<std::vector<uint8_t>> bytes;
 unsigned pos=0; bool opened=false; int failSeek=-1;
 std::vector<unsigned> seeks;
 File()=default;
 explicit File(const std::vector<uint8_t>& b):bytes(new std::vector<uint8_t>(b)),opened(true){}
 explicit operator bool()const{return opened;}
 unsigned size()const{return bytes?bytes->size():0;}
 unsigned position()const{return pos;}
 int available()const{return opened?size()-pos:0;}
 int read(uint8_t* out,unsigned n){n=std::min(n,(unsigned)available());if(n)memcpy(out,bytes->data()+pos,n);pos+=n;return n;}
 bool seek(unsigned n){seeks.push_back(n);if(!opened||n>size()||int(n)==failSeek)return false;pos=n;return true;}
 void close(){opened=false;}
};
}
using fs::File;
'''
code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include "FS.h"
#include "core/mp3_codec.h"
struct Log {void println(const char*){} template<class... T>void printf(const char*,T...){}} Serial;
std::vector<uint8_t> fixture;
struct Storage {File open(const char*){return File(fixture);}} SD;
int locks=0,txStarts=0;
namespace hal {namespace bus {void lockSpi(){++locks;}void unlockSpi(){--locks;}}namespace audio {bool txBegin(unsigned){++txStarts;return true;}}}
namespace core {namespace usage {void emit(const char*,const char*){}} namespace dsp {template<class... T>void pushStereoWithVolumeBasisPoints(T...){}}}
struct Worker {bool cleanupPending(){return false;}};
struct PlaybackWorker {struct Control {Control(Worker&){}explicit operator bool(){return true;}};};
struct Ring {int16_t* buf=(int16_t*)1;void reset(){}int free(){return 0;}};
struct Progress {unsigned ms=0;void reset(){ms=0;}void add(int n,int hz){ms+=n*1000/hz;}void seek(unsigned n,int){ms=n;}};
struct Mp3Player {
 Worker worker_; Ring ring_; Progress progress_;core::audio::Mp3Codec codec_;
 uint8_t input[16384]{};uint8_t* inBuf_=input;int16_t decodeBuf_[MINIMP3_MAX_SAMPLES_PER_FRAME]{};
 File file_;bool fileOpen_=false,playing_=false,usageTrackActive_=false;
 char currentPath_[192]{};unsigned fileSize_=0,audioStart_=0,lastGoodFilePos_=0,decodeNoProgressCount_=0;
 int detectedRate_=0;unsigned estBitrate_=0;
 struct Info {int hz=0,channels=0;unsigned bitrateKbps=0,durationMs=0;char title[64]{},artist[64]{};} info_;
 Mp3Player(){codec_.init(input,sizeof(input));}
 bool startTask(){return true;}void stopTrackUnlocked(){playing_=false;fileOpen_=false;file_.close();}
 bool parseID3_();bool startTrack(const char*);void seekRelativeSeconds(int);
 int decodeOneFrame_(){return 0;}int scaledVolumeBasisPoints(){return 1000;}
 unsigned elapsedMs(){return progress_.ms;}
};
'''
for signature in ('bool Mp3Player::parseID3_()', 'bool Mp3Player::startTrack(', 'void Mp3Player::seekRelativeSeconds('):
    body = function(signature)
    if '--negative-control' in sys.argv and 'parseID3_' in signature:
        body = body.replace('return file_.seek(audioStart_);', 'return file_.seek(0);')
    code += body + '\n'
code += r'''
using Bytes=std::vector<uint8_t>;
void size32(Bytes& b,unsigned at,unsigned size,bool sync){for(int i=3;i>=0;--i){b[at+i]=size&(sync?127:255);size>>=sync?7:8;}}
Bytes audio(unsigned frames=4){Bytes b;for(unsigned i=0;i<frames;++i){unsigned p=b.size();b.resize(p+417);b[p]=0xff;b[p+1]=0xfb;b[p+2]=0x90;}return b;}
void append(Bytes& a,const Bytes& b){a.insert(a.end(),b.begin(),b.end());}
Bytes frame(const char* id,unsigned size,int version){Bytes b(10+size);memcpy(b.data(),id,4);size32(b,4,size,version==4);return b;}
Bytes textFrame(const char* id,const char* text,int version){auto b=frame(id,1+strlen(text),version);b[10]=3;memcpy(b.data()+11,text,strlen(text));return b;}
Bytes tag(Bytes body,int version=3,unsigned flags=0){Bytes b={'I','D','3',(uint8_t)version,0,(uint8_t)flags,0,0,0,0};size32(b,6,body.size(),true);auto footer=b;append(b,body);if(version==4&&(flags&16)){footer[0]='3';footer[1]='D';footer[2]='I';append(b,footer);}return b;}
void playable(const Bytes& prefix,unsigned offset,const char* title=nullptr,const char* artist=nullptr){
 fixture=prefix;append(fixture,audio());Mp3Player p;assert(p.startTrack("/music/fixture.mp3"));
 assert(p.audioStart_==offset&&p.info_.hz==44100&&p.playing_&&locks==0);
 assert(p.info_.durationMs==audio().size()*8000u/128000u);
 if(title)assert(!strcmp(p.info_.title,title));if(artist)assert(!strcmp(p.info_.artist,artist));
}
int main(){
 playable({},0); // Untagged MP3 still starts from byte zero.
 for(int version:{2,3,4}){
  Bytes b=tag(Bytes(32768),version);playable(b,b.size());
 }
 // Artwork before title/artist, and early text extraction before large padding.
 for(int version:{3,4}){
  Bytes body=frame("APIC",70000,version);append(body,textFrame("TIT2","Large artwork",version));append(body,textFrame("TPE1","Artist",version));
  auto b=tag(body,version);playable(b,b.size(),"Large artwork","Artist");
  body=textFrame("TIT2","Early title",version);append(body,textFrame("TPE1","Artist",version));body.resize(40000);
  b=tag(body,version);playable(b,b.size(),"Early title","Artist");
 }
 auto b=tag(frame("APIC",40000,4),4,16);playable(b,b.size()); // Footer excluded too.
 for(unsigned flag:{64,128}){b=tag(Bytes(40000),3,flag);playable(b,b.size());}
 // Invalid frame sizes cannot escape tag bounds or wrap the extraction loop.
 b=frame("APIC",1,3);size32(b,4,0xffffffff,false);b=tag(b);playable(b,b.size());
 b=frame("TIT2",1,4);b[4]=0x80;b=tag(b,4);playable(b,b.size());
 // Extraction work is bounded; audio start is independent of the frame count.
 Bytes body;for(int i=0;i<1000;++i)append(body,frame("PRIV",1,3));b=tag(body);playable(b,b.size());
 // Bad tag bounds and synchsafe/version fields fail before opening I2S.
 for(int bad=0;bad<5;++bad){
  fixture=tag(Bytes(32));append(fixture,audio());
  if(bad==0)size32(fixture,6,fixture.size()+1,true);
  if(bad==1)fixture[6]=0x80;
  if(bad==2)fixture[3]=5;
  if(bad==3){fixture=tag(Bytes(32),4,16);fixture.resize(fixture.size()-1);}
  if(bad==4)fixture=tag(Bytes(32)); // Tag only, no audio.
  Mp3Player p;const int before=txStarts;assert(!p.startTrack("/bad.mp3"));assert(!p.fileOpen_&&!p.playing_&&txStarts==before&&locks==0);
 }
 // Failed SD seek propagates instead of probing stale metadata.
 {Mp3Player p;fixture=tag(Bytes(40000));auto end=fixture.size();append(fixture,audio());p.file_=File(fixture);p.fileSize_=fixture.size();p.file_.failSeek=end;assert(!p.parseID3_());}
 // Seeking and estimated duration use audio bytes, never leading artwork.
 fixture=tag(Bytes(70000));auto offset=fixture.size();append(fixture,audio(1000));
 {Mp3Player p;assert(p.startTrack("/seek.mp3"));p.progress_.ms=0;p.file_.seeks.clear();p.seekRelativeSeconds(3);
  assert(p.file_.seeks.front()==offset+48000);p.file_.seeks.clear();p.seekRelativeSeconds(-100);
  assert(p.file_.seeks.front()==offset&&p.progress_.ms==0);}
 // A short track cannot underflow when seeking beyond its estimated duration.
 fixture=tag(Bytes(40000));offset=fixture.size();append(fixture,audio());
 {Mp3Player p;assert(p.startTrack("/short.mp3"));p.file_.seeks.clear();p.seekRelativeSeconds(99);assert(p.file_.seeks.front()==offset&&p.progress_.ms==0);}
 puts("PASS MP3 startup/ID3 bounds, metadata extraction, real MPEG decode, failed-start cleanup and metadata-aware seeking");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    directory = Path(tmp)
    (directory / 'FS.h').write_text(fs)
    (directory / 'SD.h').write_text('#include "FS.h"\n')
    (directory / 'esp_heap_caps.h').write_text('#include <cstdlib>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n#define MALLOC_CAP_INTERNAL 4\ninline void* heap_caps_malloc(size_t n,int){return malloc(n);}\n')
    (directory / 'test.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++17', '-fsanitize=address,undefined',
                    '-I'+str(directory), '-I'+str(ROOT/'include'),
                    str(directory/'test.cpp'), str(ROOT/'src/core/mp3_codec.cpp'),
                    '-o', str(directory/'test')], check=True)
    subprocess.run([str(directory/'test')], check=True)
