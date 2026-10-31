#include "ui/qr_preview.h"
#include "ui/qr_scanner_view.h"
#include "ui/native_frame.h"
#include "services/qr_decode.h"
#include "hal/hal_display.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
namespace ui {
namespace {
// Center VGA at native scale. Recognition retains the service's wider 320x240
// crop; preview clips to 280x240. Decoder blocks are borrowed, never modified.
void compositeBlock(uint16_t* pixels,int w,int h,int x,int y,void*) {
    if (!pixels || w<=0 || h<=0) return;
    x+=(280-640)/2; y+=(240-480)/2;
    const int left=max(0,x), top=max(0,y);
    const int right=min(280,x+w), bottom=min(240,y+h);
    if (left>=right || top>=bottom) return;
    auto& canvas=hal::display::canvas();
    const bool swap=canvas.getSwapBytes(); canvas.setSwapBytes(false);
    if (left==x && right==x+w && top==y && bottom==y+h)
        canvas.pushImage(x,y,w,h,pixels);
    else for (int row=top;row<bottom;++row)
        canvas.pushImage(left,row,right-left,1,pixels+(row-y)*w+left-x);
    canvas.setSwapBytes(swap);
}
struct Canvas {
    TFT_eSprite& frame;
    uint16_t at(int x,int y) const { return frame.readPixel(x,y); }
    uint16_t readPixel(int x,int y) const { return frame.readPixel(x,y); }
    void fillRect(int x,int y,int w,int h,uint16_t ink) { frame.fillRect(x,y,w,h,ink); }
};
}
QrPreview::~QrPreview() { close(); }
void QrPreview::close() { heap_caps_free(portrait_); portrait_=nullptr; }
bool QrPreview::render(const uint8_t* jpeg,size_t length) {
    const bool buildGray=!services::qr::scanBusy();
    if (!portrait_) portrait_=static_cast<uint16_t*>(heap_caps_malloc(280u*240u*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if (!portrait_) return false;
    if (!services::qr::decodeScannerJpeg(jpeg,length,compositeBlock,nullptr,buildGray)) {
        static uint32_t lastFailLog=0;
        const uint32_t now=millis();
        if (now-lastFailLog>=2000) {
            Serial.printf("[wifi-app] JPEG decode failed (len=%u)\n",unsigned(length));
            lastFailLog=now;
        }
        return false;
    }
    auto& frame=hal::display::canvas();
    Canvas canvas{frame}; qr_scanner_view::overlay(canvas);
    const auto* pixels=static_cast<const uint16_t*>(frame.getPointer());
    for (int x=0;x<280;++x) for (int y=0;y<240;++y)
        portrait_[(279-x)*240+y]=pixels[y*280+x];
    presentNativeFrame(portrait_);
    return buildGray;
}
}
