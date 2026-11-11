#include "services/camera_grid.h"
#include "services/camera_session.h"
#include "services/camera_snapshot.h"
#include "services/camera_preview_decode.h"
#include <Arduino.h>
#include <esp_timer.h>
namespace services { namespace camera_grid {
namespace {
camera::Session cameraSession;
camera::Snapshot snapshot;
camera::PreviewDecode decoder;
State current=State::Closed;Image image{};const char* reason="";uint32_t started=0;
void release(){decoder.close();snapshot.clear();cameraSession.stop();cameraSession.restoreUi(camera::Session::Resume::DeferredWithInputRecovery);}
void fail(const char* message){release();reason=message;current=State::Failed;}
}
bool begin(){close();current=State::Waiting;return true;}
void close(){release();current=State::Closed;reason="";}
State state(){return current;}
const char* error(){return reason;}
bool copy(Image& result){if(current!=State::Ready)return false;result=image;return true;}
void poll(){
 if(current==State::Waiting){
  hal::camera::Tuning tuning;
  if(cameraSession.prepareStill(camera::CaptureResolution::Vga,tuning)!=camera::Session::StillResult::Ready){fail("Camera unavailable. Retry.");return;}
  if(snapshot.captureVga(cameraSession,esp_timer_get_time())!=camera::Snapshot::Result::Ready){fail("No photo. Retry.");return;}
  if(!decoder.begin(true)||!decoder.submit(snapshot.data(),snapshot.size(),snapshot.timing().dmaUs)){fail("Photo memory unavailable");return;}
  snapshot.clear();cameraSession.stop();started=millis();current=State::Decoding;
 }else if(current==State::Decoding){
  camera::PreviewDecode::Frame frame;
  if(decoder.acquire(frame)){
   // Decoder's native portrait buffer is 240 wide x 280 high, big-endian
   // RGB565. Sample cell centres; do not reinterpret it as a landscape stride.
   for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
    unsigned landscapeX=(2*x+1)*280/(2*width),landscapeY=(2*y+1)*240/(2*height);
    unsigned index=(279-landscapeX)*240+landscapeY;
    uint16_t raw=frame.pixels[index],c=uint16_t((raw>>8)|(raw<<8));
    unsigned r=((c>>11)&31)*255/31,g=((c>>5)&63)*255/63,b=(c&31)*255/31;
    image.color[y*width+x]=c;image.luminance[y*width+x]=(r*77+g*150+b*29)>>8;
   }
   decoder.release();release();current=State::Ready;
  }else if(decoder.failed()||millis()-started>5000)fail("Photo decode failed");
 }
}
}}
