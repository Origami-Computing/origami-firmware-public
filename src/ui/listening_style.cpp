#include "ui/listening_style.h"
#include "config/display_safe_area.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "core/store.h"
#include "hal/hal_display.h"
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

namespace ui { namespace listening_style {
namespace {
constexpr int W=280, H=240, N=W*H;
int s_selected=-1, s_mapStyle=-1, s_lastWidth=-1;
uint16_t* s_base=nullptr;
uint16_t* s_previewChrome=nullptr;
int s_previewStyle=-1;
struct Pixel { uint8_t distance, mix; };
Pixel* s_map=nullptr;
uint8_t s_colors[256][3];
uint32_t s_renderUs=0;
float clamp(float x) { return fmaxf(0.f,fminf(1.f,x)); }
float smooth(float x) { x=clamp(x); return x*x*(3.f-2.f*x); }
uint16_t swap(uint16_t x) { return (x>>8)|(x<<8); }
float borderDistance(float x,float y) {
    const auto r=ui::safe_area::opening(display_safe_area::kProfile);
    const float qx=fabsf(x-(r.x+r.w*.5f))-(r.w*.5f-r.radius);
    const float qy=fabsf(y-(r.y+r.h*.5f))-(r.h*.5f-r.radius);
    return fmaxf(0.f,-(hypotf(fmaxf(qx,0.f),fmaxf(qy,0.f))+fminf(fmaxf(qx,qy),0.f)-r.radius));
}
uint8_t colorMix(const Style& v,float x,float y) {
    const float low=fminf(0.f,v.ax)+fminf(0.f,v.ay);
    return lroundf(255.f*clamp((v.ax*x/W+v.ay*y/H-low)/(fabsf(v.ax)+fabsf(v.ay))));
}
void paletteColor(const Style& v,int mix,uint8_t* color) {
    const float u=smooth(mix/255.f);
    for(int c=0;c<3;++c) color[c]=lroundf(v.colors[0][c]*(1-u)+v.colors[1][c]*u);
}
unsigned borderAlpha(const Style& v,float distance,float width,float opacity) {
    return lroundf(255.f*v.opacity*clamp(opacity)*(1.f-smooth((distance-width*v.solid)/(width*(1.f-v.solid)))));
}
uint16_t composite(uint16_t base,const uint8_t* color,unsigned a) {
    const unsigned r=((base>>11)&31)*(255-a)*255+31*a*color[0];
    const unsigned g=((base>>5)&63)*(255-a)*255+63*a*color[1];
    const unsigned b=(base&31)*(255-a)*255+31*a*color[2];
    return uint16_t(((r+32512)/65025)<<11 | ((g+32512)/65025)<<5 | ((b+32512)/65025));
}
void preparePalette(void (*serviceMic)()) {
    const auto& v=kStyles[selected()];
    for(int t=0;t<256;++t) paletteColor(v,t,s_colors[t]);
    for(int y=0;y<H;++y) {
        for(int x=0;x<W;++x)
            s_map[y*W+x].mix=colorMix(v,x+.5f,y+.5f);
        if(serviceMic && y%16==15) serviceMic();
    }
    s_mapStyle=selected(); s_lastWidth=-1;
}
}
int selected() {
    if(s_selected<0) { const int stored=core::store::getU8("settings","agent_style",0); s_selected=stored<kCount?stored:0; }
    return s_selected;
}
bool select(int index, bool persist) {
    if(index<0 || index>=kCount) return false;
    if(persist && !core::store::putU8("settings","agent_style",index)) return false;
    s_selected=index;
    return true;
}
bool ready() { return s_base && s_map; }
uint32_t lastRenderUs() { return s_renderUs; }
void end() {
    free(s_base); s_base=nullptr; s_lastWidth=-1;
}
void clearBase() {
    if(s_base) memset(s_base,0xff,N*sizeof(uint16_t));
    s_lastWidth=-1;
}
void releaseCache() {
    end(); free(s_map); s_map=nullptr; s_mapStyle=-1;
}
bool prepare(void (*serviceMic)()) {
    if(s_map) {
        if(s_mapStyle!=selected()) preparePalette(serviceMic);
        return true;
    }
    s_map=static_cast<Pixel*>(heap_caps_malloc(N*sizeof(Pixel),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!s_map) return false;
    for(int y=0;y<H;++y) {
        for(int x=0;x<W;++x) {
            s_map[y*W+x].distance=lroundf(fminf(255.f,borderDistance(x+.5f,y+.5f)*4.f));
        }
        if(serviceMic && y%8==7) serviceMic();
    }
    preparePalette(serviceMic);
    return true;
}
bool begin(void (*serviceMic)()) {
    end();
    auto* canvas=static_cast<uint16_t*>(hal::display::canvas().getPointer());
    if(!canvas || !prepare(serviceMic)) return false;
    s_base=static_cast<uint16_t*>(heap_caps_malloc(N*sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!s_base) return false;
    memcpy(s_base,canvas,N*sizeof(uint16_t));
    return true;
}
float paint(float envelope, void (*serviceMic)(), float opacity, bool force, bool overlay) {
    const auto& v=kStyles[selected()];
    // Quarter-pixel widths prevent distracting discrete steps for soft speech.
    const int widthQ=lroundf(4.f*(v.minimum+(v.maximum-v.minimum)*clamp(envelope)));
    const float width=widthQ*.25f;
    if(!ready()) return width;
    if(s_mapStyle!=selected()) preparePalette(serviceMic);
    if(widthQ==s_lastWidth && !force) return width;
    const uint32_t started=micros();
    uint8_t alpha[256];
    for(int d=0;d<256;++d) alpha[d]=borderAlpha(v,d*.25f,width,opacity);
    auto* dst=static_cast<uint16_t*>(hal::display::canvas().getPointer());
    for(int y=0;y<H;++y) {
        for(int x=0;x<W;++x) {
            const int i=y*W+x;
            const unsigned a=alpha[s_map[i].distance];
            const uint16_t source=overlay?dst[i]:s_base[i];
            if(!a) { dst[i]=source; continue; }
            const uint16_t base=swap(source);
            const auto* color=s_colors[s_map[i].mix];
            dst[i]=swap(composite(base,color,a));
        }
        if(serviceMic && y%16==15) serviceMic();
    }
    s_lastWidth=widthQ; s_renderUs=micros()-started;
    hal::display::markAllDirty();
    return width;
}
static void renderPreview(TFT_eSprite& canvas, int index, int rows) {
    if(index<0 || index>=kCount || canvas.width()!=W || canvas.height()!=H) return;
    auto* pixels=static_cast<uint16_t*>(canvas.getPointer());
    if(!pixels) return;
    const auto& v=kStyles[index];
    const float width=lroundf(4.f*(v.minimum+(v.maximum-v.minimum)*.65f))*.25f;
    uint8_t colors[256][3], alpha[256];
    for(int i=0;i<256;++i) {
        paletteColor(v,i,colors[i]);
        alpha[i]=borderAlpha(v,i*.25f,width,1.f);
    }
    for(int y=0;y<rows;++y) for(int x=0;x<W;++x) {
        const int distance=lroundf(fminf(255.f,borderDistance(x+.5f,y+.5f)*4.f));
        const unsigned a=alpha[distance];
        if(!a) continue;
        const int i=y*W+x;
        pixels[i]=swap(composite(swap(pixels[i]),colors[colorMix(v,x+.5f,y+.5f)],a));
    }
}
void exitPreview() {
    free(s_previewChrome); s_previewChrome=nullptr; s_previewStyle=-1;
}
void drawPreview(TFT_eSprite& canvas, int index) {
    if(index<0 || index>=kCount || canvas.width()!=W || canvas.height()!=H || !canvas.getPointer()) return;
    renderPreview(canvas,index,H);
    if(!s_previewChrome) s_previewChrome=static_cast<uint16_t*>(
        heap_caps_malloc(W*kSystemStatusBarH*sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(s_previewChrome) memcpy(s_previewChrome,canvas.getPointer(),W*kSystemStatusBarH*sizeof(uint16_t));
    s_previewStyle=index;
}
void restorePreviewChrome(TFT_eSprite& canvas, int index) {
    if(index<0 || index>=kCount || canvas.width()!=W || canvas.height()!=H || !canvas.getPointer()) return;
    if(s_previewChrome && s_previewStyle==index) {
        memcpy(canvas.getPointer(),s_previewChrome,W*kSystemStatusBarH*sizeof(uint16_t));
    } else {
        // Allocation failure still restores a clean underlay. Recompute only
        // the status band, never the menu or live microphone geometry/cache.
        canvas.fillRect(0,0,W,kSystemStatusBarH,kUiBg);
        renderPreview(canvas,index,kSystemStatusBarH);
    }
}
}}
