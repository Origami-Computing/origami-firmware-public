#include "services/pocket_band.h"
#include "services/microphone_stream.h"
#include "services/peripheral_reservation.h"
#include "services/wav_player.h"
#include "core/pocket_sampler.h"
#include <esp_heap_caps.h>
#include <string.h>
#include <Arduino.h>

namespace services { namespace pocket_band {
namespace {
using Mixer=core::audio::PocketSampler;
audio::MicrophoneStream mic;
audio::WavPlayer player; // Firmware-lifetime owner; deferred playback never calls module code.
background_network::PeripheralReservation reservation;
Mixer mixer;State state=State::Closed;
int16_t* samples=nullptr;int16_t* rendered=nullptr;
uint16_t patterns[4]={0x1111,0x1010,0xAAAA,0x0100};unsigned bpm=120;
uint32_t lengths[4]={},recorded=0,renderedFrames=0,totalFrames=0;bool warming=false;unsigned recording=0;uint32_t recordStarted=0;
const char* error="";
void fail(const char* text){error=text;state=State::Error;}
bool receive(const int16_t* pcm,uint32_t count,void*){
 unsigned room=16000-recorded;if(count>room)count=room;
 memcpy(rendered+recorded,pcm,count*sizeof(int16_t));recorded+=count;return true;
}
void factorySamples(){
 uint32_t random=12345;
 for(unsigned t=0;t<4;++t){lengths[t]=t==0?8000:4000;for(unsigned i=0;i<lengths[t];++i){
  random=random*1664525u+1013904223u;int v=t==0?int((i%160)*400)-32000:t==3?int((i%80)*800)-32000:int(random>>16)-32768;
  unsigned decay=lengths[t]-i;samples[t*16000+i]=int16_t(v*int(decay)/int(lengths[t])/3);
 }}
}
void endCapture(){mic.close();reservation.release();}
}
bool open(){
 if(state!=State::Closed)return true;
 if(player.sessionOpen())return false;
 samples=static_cast<int16_t*>(heap_caps_malloc(4*16000*sizeof(int16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 rendered=static_cast<int16_t*>(heap_caps_malloc(96000*sizeof(int16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 if(!samples||!rendered){heap_caps_free(samples);heap_caps_free(rendered);samples=rendered=nullptr;return false;}
 patterns[0]=0x1111;patterns[1]=0x1010;patterns[2]=0xAAAA;patterns[3]=0x0100;bpm=120;
 factorySamples();error="";state=State::Editing;return true;
}
void close(){
 endCapture();player.closeSession();
 // WavPlayer copied its source: these buffers are never borrowed by its worker.
 heap_caps_free(samples);heap_caps_free(rendered);samples=rendered=nullptr;mixer=Mixer{};state=State::Closed;
}
void stop(){if(state==State::Closed)return;endCapture();player.closeSession();state=State::Editing;error="";}
bool record(unsigned track){
 if(track>=4||state==State::Closed)return false;
 stop();if(player.sessionOpen()){fail("Audio is stopping. Retry.");return false;}
 if(reservation.acquire(true)!=background_network::PeripheralReservation::Result::Acquired){reservation.release();fail("Microphone busy. Retry.");return false;}
 recording=track;recorded=0;
 if(!mic.open(receive)){endCapture();fail("Microphone unavailable");return false;}
 recordStarted=millis();state=State::Recording;return true;
}
bool toggle(unsigned track,unsigned step){if(track>=4||step>=16||state==State::Closed||state==State::Recording)return false;stop();patterns[track]^=uint16_t(1u<<step);return true;}
bool tempo(unsigned value){if(value<40||value>240||state==State::Closed||state==State::Recording)return false;stop();bpm=value;return true;}
bool play(){
 if(state==State::Closed||state==State::Recording)return false;
 stop();if(player.sessionOpen()){fail("Audio is stopping. Retry.");return false;}
 mixer=Mixer{};mixer.tempo(bpm);
 for(unsigned t=0;t<4;++t){mixer.sample(t,samples+t*16000,lengths[t]);mixer.pattern(t,patterns[t]);}
 mixer.start();totalFrames=16000*60*4/bpm;renderedFrames=0;warming=true;state=State::Rendering;return true;
}
void poll(){
 if(state==State::Recording){mic.poll();if(recorded==16000){endCapture();memcpy(samples+recording*16000,rendered,recorded*sizeof(int16_t));lengths[recording]=recorded;state=State::Editing;}else if(millis()-recordStarted>4000){endCapture();fail("Recording timed out");}}
 else if(state==State::Rendering){unsigned n=totalFrames-renderedFrames;if(n>1024)n=1024;mixer.render(rendered+renderedFrames,n);renderedFrames+=n;
  if(renderedFrames==totalFrames){if(warming){warming=false;renderedFrames=0;return;}if(!player.openSession()||!player.startPcm(rendered,totalFrames,16000,true)){player.closeSession();fail("Playback unavailable");}else state=State::Playing;}
 }
}
Snapshot snapshot(){Snapshot s{};s.state=state;s.bpm=bpm;s.error=error;memcpy(s.pattern,patterns,sizeof(patterns));s.progress=state==State::Recording?recorded*100/16000:state==State::Rendering&&totalFrames?(warming?0:50)+renderedFrames*50/totalFrames:0;return s;}
}}
