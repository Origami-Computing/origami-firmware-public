"""Exercise production HAL frame borrowing with joined camera DMA payloads."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/hal/hal_camera.cpp').read_text()
def function(signature):
 start=source.index(signature);end=source.index('{',start)+1;depth=1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
code=r'''#include "core/jpeg_envelope.h"
#include <cassert>
#include <cstdint>
#include <vector>
constexpr int PIXFORMAT_JPEG=4;
struct camera_fb_t {uint8_t* buf;size_t len;int width,height,format;struct{long tv_sec,tv_usec;}timestamp;};
struct CameraFrame {uint8_t* buf=nullptr;size_t len=0;int width=0,height=0,format=0;uint64_t dmaStartUs=0;};
bool s_initialized=true;camera_fb_t* s_lastFb=nullptr;camera_fb_t* queued=nullptr;unsigned returned=0,taken=0;
camera_fb_t* esp_camera_fb_get(){++taken;auto* f=queued;queued=nullptr;return f;}
void esp_camera_fb_return(camera_fb_t* f){assert(f);++returned;}
void returnFrame();
'''+function('bool getFrame(CameraFrame* out)')+'\n'+function('void returnFrame() {')+r'''
int main(){
 std::vector<uint8_t> jpeg={255,216,255,192,0,11,8,1,224,2,128,1,1,17,0,
 255,218,0,8,1,1,0,0,63,0,42,255,0,42,255,208,42,255,217};
 auto joined=jpeg;joined.insert(joined.end(),jpeg.begin(),jpeg.end());
 camera_fb_t fb{joined.data(),joined.size(),640,480,PIXFORMAT_JPEG,{12,3456}};
 CameraFrame frame;queued=&fb;assert(!getFrame(nullptr)&&taken==0);
 s_initialized=false;assert(!getFrame(&frame)&&taken==0);s_initialized=true;
 assert(getFrame(&frame));assert(frame.len==jpeg.size()&&frame.buf==fb.buf);
 assert(frame.width==640&&frame.height==480&&frame.dmaStartUs==12003456);
 assert(core::images::singleJpegEnvelope(frame.buf,frame.len));
 assert(!core::images::singleJpegEnvelope(fb.buf,fb.len));
 assert(!getFrame(&frame)&&taken==1&&returned==0);returnFrame();assert(returned==1&&!s_lastFb);
 returnFrame();assert(returned==1);
 fb.len=jpeg.size();queued=&fb;assert(getFrame(&frame)&&frame.len==jpeg.size());returnFrame();
 fb.len=jpeg.size()-1;queued=&fb;assert(!getFrame(&frame)&&!s_lastFb&&returned==3);
 joined[0]=0;fb.len=joined.size();queued=&fb;assert(!getFrame(&frame)&&!s_lastFb&&returned==4);
 // Non-JPEG modes retain their byte range and ordinary ownership contract.
 fb.format=0;queued=&fb;assert(getFrame(&frame)&&frame.len==joined.size());returnFrame();
 assert(!getFrame(&frame)&&!s_lastFb&&returned==5);
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS HAL camera JPEG boundary: joined frames, capture timestamp, malformed/truncated release, borrowed ownership and non-JPEG preservation')
