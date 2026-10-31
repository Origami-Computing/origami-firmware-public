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
 uint16_t readPixel(int x,int y)const {
  return x>=0&&x<280&&y>=0&&y<240?at(x,y):0;
 }
 void fillRect(int x,int y,int w,int h,uint16_t ink){
  uint16_t p=(ink>>8)|(ink<<8);
  for(int xx=x;xx<x+w;xx++)for(int yy=y;yy<y+h;yy++)
   if(xx>=0&&xx<280&&yy>=0&&yy<240)pixels[(279-xx)*240+yy]=p;
 }
};
}
CameraPreview::~CameraPreview() { close(); }
CameraPreview::Result CameraPreview::copyRgb565(const uint8_t* bytes,size_t length,ShouldAbort abort,void* user) {
    frameReady_=false;
    if (!hasSurfaces() || !bytes || length!=320u*240u*2u) return Result::Invalid;
    // Small tiles keep both the row-major sensor data and portrait output hot
    // without reserving internal RAM or changing display-transfer ownership.
    for (int y0=0;y0<240;y0+=16) {
        if (abort && abort(user)) return Result::Interrupted;
        for (int x0=0;x0<280;x0+=8) for(int x=x0;x<x0+8;++x) {
            auto* dst=frame_+(279-x)*240;
            for(int y=y0;y<y0+16;++y) {
                const auto* src=bytes+((y*320+x+20)*2);
                dst[y]=uint16_t(src[0])|(uint16_t(src[1])<<8);
            }
        }
    }
    if (abort && abort(user)) return Result::Interrupted;
    frameReady_=true;
    return Result::Decoded;
}
bool CameraPreview::copyDecoded(const uint16_t* pixels,bool video) {
    if (!hasSurfaces() || !pixels) return false;
    memcpy(video?composite_:frame_,pixels,280u*240u*sizeof(uint16_t));frameReady_=true;
    return true;
}
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
    frame_=static_cast<uint16_t*>(heap_caps_aligned_calloc(16,1,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    composite_=static_cast<uint16_t*>(heap_caps_aligned_calloc(16,1,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (!hasSurfaces()) { close(); return false; }
    return true;
}
bool CameraPreview::prepareDecoder(bool video, bool cameraVga, bool reduced) {
    if (!hasSurfaces()) return true;
    if (reduced) return decoder_.prepare();
    return decoder_.prepareNative(!video && cameraVga) || (!video && decoder_.prepare());
}
CameraPreview::Result CameraPreview::decode(const uint8_t* jpeg, size_t length, ShouldAbort abort, void* user, bool fit) {
    if (!ready() || length>INT32_MAX) return Result::Invalid;
    // Decoding overwrites the retained image in place. Cancellation or failure
    // can leave mixed old/new pixels; keep the panel's last complete frame until
    // another decode succeeds, including animation/status-only redraws.
    frameReady_=false;
    const auto result=decoder_.decode(jpeg,length,frame_,abort,user,fit);
    if (result==Result::Decoded) frameReady_=true;
    return result;
}
CameraPreview::Presentation CameraPreview::present(unsigned phase, int pulseAgeMs,
                                                   const char* status, bool recording, bool tutorial) {
    if (!hasFrame()) return {};
    memcpy(composite_,frame_,280u*240u*sizeof(uint16_t));
    CameraCanvas canvas{composite_};
    if (!tutorial) camera_photographer::overlays(canvas,phase);
    if (!tutorial && status && *status) {
        canvas.fillRect(52, 196, 180, 20, 0x0000);
        char caption[29];
        unsigned columns=28;
        do {
            ui::pixel_text::shorten(status,caption,columns);
#ifdef ORIGAMI_INSTRUMENT_FONT
            const int width=ui::instrument_text::width(caption,ui::instrument_text::cell(1));
#else
            const int width=int(strlen(caption))*6;
#endif
            if (width<=172 || columns==3) break;
            --columns;
        } while (true);
        ui::pixel_text::text(canvas, caption, 56, 202, 1, recording ? 0xF800 : 0xFFFF);
    }
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

CameraPreview::Result CameraPreview::decodeVideo(const uint8_t* jpeg, size_t length) {
    if (!ready()) return Result::Invalid;
    // Video owns this presentation surface until the next successful decode.
    // Never expose a partially replaced frame after decoder failure.
    frameReady_=false;
    const auto result=decoder_.decode(jpeg,length,composite_,nullptr,nullptr,true);
    if (result==Result::Decoded) frameReady_=true;
    return result;
}
CameraPreview::Presentation CameraPreview::presentVideo(const char* title, const char* controls, unsigned progress) {
    if (!hasFrame()) return {};
    CameraCanvas canvas{composite_};
    // The fitted image occupies y=15..224. Restore both black text bars on
    // every presentation, including paused/held frames and shorter labels.
    canvas.fillRect(0,0,280,15,0);
    canvas.fillRect(0,225,280,15,0);
    ui::pixel_text::text(canvas,title,20,4,1,0xFFFF);
    ui::pixel_text::text(canvas,controls,20,228,1,0xFFFF);
    canvas.fillRect(20,222,240,2,0x4208);
    canvas.fillRect(20,222,int(240*(progress>1000?1000:progress)/1000),2,0xFFFF);
    return presentNativeFrame(composite_);
}

}
