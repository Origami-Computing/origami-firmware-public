#include "ui/camera_preview.h"
#include "ui/camera_photographer_view.h"
#include "hal/hal_display.h"
#include <esp_heap_caps.h>
#include <string.h>
#include <limits.h>
#include <JPEGDecoder.h>
namespace ui {
namespace {
struct CameraCanvas {
 uint16_t* pixels;
 uint16_t at(int x,int y)const {uint16_t p=pixels[(279-x)*240+y];return (p>>8)|(p<<8);}
 void fillRect(int x,int y,int w,int h,uint16_t ink){
  uint16_t p=(ink>>8)|(ink<<8);
  for(int xx=x;xx<x+w;xx++)for(int yy=y;yy<y+h;yy++)
   if(xx>=0&&xx<280&&yy>=0&&yy<240)pixels[(279-xx)*240+yy]=p;
 }
};
}
CameraPreview::~CameraPreview() { close(); }
void CameraPreview::closeDecoder() { decoder_.close(); }
void CameraPreview::close() {
    closeDecoder();
    heap_caps_free(frame_);
    heap_caps_free(composite_);
    frame_=composite_=nullptr;
    frameReady_=false;
}
bool CameraPreview::prepareSurfaces() {
    close();
    constexpr size_t bytes=280u*240u*sizeof(uint16_t);
    frame_=static_cast<uint16_t*>(heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    composite_=static_cast<uint16_t*>(heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (!hasSurfaces()) { close(); return false; }
    return true;
}
bool CameraPreview::prepareDecoder() { return !hasSurfaces() || decoder_.prepare(); }
CameraPreview::Result CameraPreview::decode(const uint8_t* jpeg, size_t length, ShouldAbort abort, void* user) {
    if (!ready() || length>INT32_MAX) return Result::Invalid;
    const auto result=decoder_.decode(jpeg,length,frame_,abort,user);
    if (result==Result::Decoded) frameReady_=true;
    return result;
}
CameraPreview::Presentation CameraPreview::present(unsigned phase, int pulseAgeMs, bool tutorial) {
    if (!hasFrame()) return {};
    memcpy(composite_,frame_,280u*240u*sizeof(uint16_t));
    CameraCanvas canvas{composite_};
    if(!tutorial)camera_photographer::overlays(canvas,phase);
    if (pulseAgeMs>=0 && pulseAgeMs<140) {
        const int alpha=pulseAgeMs<35?15:(140-pulseAgeMs)*15/105;
        for (int i=0;i<280*240;++i) {
            uint16_t p=composite_[i];p=(p>>8)|(p<<8);
            p=ui::raster::blend(0xFFFF,p,alpha);
            composite_[i]=(p>>8)|(p<<8);
        }
    }
    // Keep the instruction static even through the white shutter pulse.
    if(tutorial)camera_photographer::overlays(canvas,phase,true);
    return presentNativeFrame(composite_);
}
bool CameraPreview::presentFallback(const uint8_t* jpeg, size_t length) {
  if (!JpegDec.decodeArray(const_cast<uint8_t*>(jpeg), length)) return false;

  auto& d = hal::display::directTft();
  uint16_t mcu_w = JpegDec.MCUWidth;
  uint16_t mcu_h = JpegDec.MCUHeight;
  d.setViewport(0,0,d.width(),d.height()-28,false);
  while (JpegDec.readSwappedBytes()) {
    uint16_t* pImg = (uint16_t*)JpegDec.pImage;
    uint32_t dx = JpegDec.MCUx * mcu_w;
    uint32_t dy = JpegDec.MCUy * mcu_h;
    if (dx < (uint32_t)d.width() && dy < (uint32_t)d.height()) {
      d.pushImage(dx, dy, mcu_w, mcu_h, pImg);
    }
  }
  d.resetViewport();
  hal::display::markAllDirty();
  return true;
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool CameraPreview::copyCompositeForDevelopment(TFT_eSprite& target)const{
 if(!hasFrame()||!target.created())return false;
 CameraCanvas c{composite_};
 for(int y=0;y<240;++y)for(int x=0;x<280;++x)target.drawPixel(x,y,c.at(x,y));
 return true;
}
#endif

}
