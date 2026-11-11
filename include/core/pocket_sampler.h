#pragma once
#include <stddef.h>
#include <stdint.h>

namespace core { namespace audio {
// Four-voice 16 kHz mono sequencer. No allocations, I/O, locks or callbacks.
// One audio owner calls all methods; the service must serialize UI changes with
// rendering. Samples are borrowed immutable PCM and must outlive render calls.
// This is the DSP core, not an audio/HAL session or a persistence owner.
class PocketSampler {
public:
 static constexpr uint32_t sampleRate=16000,maxSampleFrames=16000;
 static constexpr unsigned tracks=4,steps=16,maxBlockFrames=1024;
 bool sample(unsigned track,const int16_t* pcm,uint32_t count){
  if(track>=tracks||count>maxSampleFrames||(!pcm&&count))return false;
  bank_[track].pcm=pcm;bank_[track].count=count;position_[track]=count<<12;return true;
 }
 bool pattern(unsigned track,uint16_t mask){if(track>=tracks)return false;pattern_[track]=mask;return true;}
 bool tempo(unsigned bpm){if(bpm<40||bpm>240)return false;bpm_=bpm;return true;}
 bool pitch(unsigned track,unsigned q12){if(track>=tracks||q12<1024||q12>8192)return false;pitch_[track]=q12;return true;}
 void start(){stop();running_=true;step_=0;phase_=0;trigger();}
 void stop(){running_=false;for(unsigned t=0;t<tracks;++t)position_[t]=bank_[t].count<<12;}
 bool running()const{return running_;}
 unsigned step()const{return step_;}
 // Reject oversized blocks rather than monopolize the playback worker.
 bool render(int16_t* out,size_t frames){
  if(!out||frames>maxBlockFrames)return false;
  for(size_t f=0;f<frames;++f){int32_t mix=0;
   if(running_){
    for(unsigned t=0;t<tracks;++t){auto b=bank_[t];uint32_t i=position_[t]>>12;
     if(i>=b.count)continue;
     int32_t a=b.pcm[i],next=i+1<b.count?b.pcm[i+1]:0;
     int32_t v=a+(next-a)*int32_t(position_[t]&4095)/4096;
     // Short edge ramps tame arbitrary microphone capture boundaries.
     uint32_t gain=i<8?i:8;if(b.count-i<16){uint32_t tail=(b.count-i)/2;if(tail<gain)gain=tail;}
     mix+=v*int32_t(gain)/8;position_[t]+=pitch_[t];
    }
    phase_+=bpm_*4;if(phase_>=sampleRate*60){phase_-=sampleRate*60;step_=(step_+1)%steps;trigger();}
   }
   // Fixed headroom for overlapping voices, then saturate rather than wrap.
   mix/=2;if(mix>32767)mix=32767;if(mix< -32768)mix=-32768;out[f]=int16_t(mix);
  }return true;
 }
private:
 struct Sample {const int16_t* pcm=nullptr;uint32_t count=0;} bank_[tracks];
 uint32_t position_[tracks]={},pitch_[tracks]={4096,4096,4096,4096};
 uint16_t pattern_[tracks]={};unsigned bpm_=120,step_=0;uint32_t phase_=0;bool running_=false;
 void trigger(){for(unsigned t=0;t<tracks;++t)if(pattern_[t]&(1u<<step_))position_[t]=0;}
};
}}
