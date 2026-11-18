"""Exercise the production startup body with frame/AE faults and camera ownership mocks."""
from pathlib import Path
import tempfile,subprocess
R=Path(__file__).resolve().parents[1]
s=(R/'src/services/cine_camera_video.cpp').read_text()
body=s[s.index('bool Session::beginClip('):s.index('bool Session::previewClip(')]
code=r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include "core/cine_frame.h"
std::string trace; uint64_t clockUs=0;uint32_t millis(){return clockUs/1000;}uint64_t esp_timer_get_time(){return clockUs;}
struct {template<class...T>void printf(const char*,T...){} } Serial;
constexpr int FRAMESIZE_HD=11,FRAMESIZE_VGA=8,PIXFORMAT_JPEG=4;
bool configured=false,postBad=false;bool noisy=false,readFail=false,badDimensions=false,oldFrames=false,shutterFail=false;
unsigned samples=0;
namespace hal{namespace camera{
struct Tuning{bool allowNightMode=true,lenc=true;int wb_mode=0;};
struct CameraFrame{uint64_t dmaStartUs=0;int format=4;unsigned width=640,height=480;uint8_t*buf=nullptr;size_t len=0;};
struct ShutterInfo{bool valid=true;unsigned actualUs=10000,gainQ4=16;};
bool readJpegShutter(ShutterInfo*v){++samples;v->actualUs=noisy?(samples%2?10000:20000):10000;return !shutterFail;}
}}
namespace services{namespace camera{struct VideoRecording{
 bool ok=true;template<class T>bool beginCamera(T&,unsigned w,unsigned h,bool mic){assert((w==640&&h==480)||(w==1280&&h==720));trace+="record;";return ok;}
};}namespace cine_camera{
struct Camera{bool warm=true,startOk=true,active=true,hd=false,held=false;
 bool setVideoPreviewSize(bool value){hd=value;trace+="resize;";return warm;}
 void returnFrame(){held=false;trace+="return;";}void stop(){trace+="stop;";active=false;}
 bool running(){return active;}bool start(int size,int fmt,hal::camera::Tuning*t,int q){assert(fmt==4&&((size==11&&q==16)||(size==8&&q==12)));assert(!t->allowNightMode&&!t->lenc&&t->wb_mode==1);trace+="start;";return active=startOk;}
 void settle(unsigned n){assert(n==700);clockUs+=n*1000;trace+="settle;";}
 bool read(hal::camera::CameraFrame*f){assert(!held);clockUs+=40000;if(readFail)return false;held=true;
  static uint8_t jpg[]={255,216,255,192,0,8,8,1,224,2,128,0,255,217};
  unsigned w=hd?1280:640,h=hd?720:480;jpg[7]=h>>8;jpg[8]=h;jpg[9]=(badDimensions||(postBad&&configured))?0:w>>8;jpg[10]=w;
  f->buf=jpg;f->len=sizeof(jpg);f->width=w;f->height=h;f->dmaStartUs=oldFrames?1:clockUs;return true;}
};
struct Session{Camera camera_;unsigned autoFrameVts_=0;bool restoreOk=true;uint64_t shutterReadyUs_=0;
 struct Selected{unsigned colorMode=2;bool autoExposure=true;}selected;
 struct{unsigned manual=0,actualUs=10000,frameUs=40000;}shutter_;
 Selected captureSettings(){return selected;}void releaseIspPreview(){trace+="decoder;";}
 bool restoreSettings(Selected s){assert(s.colorMode==2);trace+="restore;";configured=true;shutterReadyUs_=clockUs+80000;return restoreOk;}
 bool beginClip(camera::VideoRecording&,bool,bool);
};
'''+body+r'''
}}
void reset(){trace.clear();clockUs=0;samples=0;configured=postBad=noisy=readFail=badDimensions=oldFrames=shutterFail=false;}
int main(){using namespace services;
 for(bool hd:{false,true})for(bool warm:{false,true})for(bool audio:{false,true})for(bool automatic:{false,true}){
  reset();cine_camera::Session s;s.camera_.warm=warm;s.selected.autoExposure=automatic;camera::VideoRecording r;
  assert(s.beginClip(r,hd,audio));assert(!s.camera_.held);
  assert(trace.find(std::string("decoder;return;resize;")+(warm?"":"stop;start;settle;restore;"))==0);
  assert(trace.find("restore;")<trace.find("record;"));
  assert(trace.rfind("record;")==trace.size()-7);
  if(warm){assert(clockUs>=40000&&clockUs<700000);assert(samples==(automatic?4u:0u));}else assert(clockUs==700000);
 }
 for(int fault=0;fault<5;++fault){reset();cine_camera::Session s;camera::VideoRecording r;
  if(fault==0)readFail=true;if(fault==1)badDimensions=true;if(fault==2)oldFrames=true;if(fault==3)noisy=true;if(fault==4)shutterFail=true;
  assert(s.beginClip(r,false,true)==(fault>=3));assert(!s.camera_.held);
  if(fault>=1)assert(clockUs>=1100000&&clockUs<=1220000);if(fault<3)assert(trace.find("record;")==std::string::npos);
 }
 reset();cine_camera::Session s;camera::VideoRecording r;s.camera_.warm=false;s.camera_.startOk=false;assert(!s.beginClip(r,false,true)&&trace=="decoder;return;resize;stop;start;");
 s.camera_.startOk=true;s.restoreOk=false;trace.clear();assert(!s.beginClip(r,false,true)&&trace.find("record;")==std::string::npos);
 reset();cine_camera::Session stale;postBad=true;assert(!stale.beginClip(r,false,true)&&!stale.camera_.held&&trace.find("record;")==std::string::npos);
 reset();cine_camera::Session failed; r.ok=false;assert(!failed.beginClip(r,false,true)&&!failed.camera_.held);
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(R/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS actual Cine startup: warm/cold VGA/HD + mic/manual/auto, JPEG quality, fresh dimensions, stale/read/AE failures, bounded noisy-light wait, failure ordering')
