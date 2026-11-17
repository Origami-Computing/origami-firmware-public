from pathlib import Path
import tempfile,subprocess,json
R=Path(__file__).resolve().parents[1];artifacts=tempfile.TemporaryDirectory(prefix='cine-ui-');O=Path(artifacts.name)
# Synthetic RGB565 scene: portable and deterministic, with no user photos.
import struct
(O/'review-input.rgb565').write_bytes(b''.join(struct.pack('<H',((x%32)<<11)|((y%64)<<5)|((x+y)%32))for y in range(168)for x in range(224)))
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'Arduino.h').write_text('#pragma once\n#include <stdint.h>\n#include <stddef.h>\n#include <pgmspace.h>\n');(p/'pgmspace.h').write_text('#pragma once\n#define PROGMEM\n#define pgm_read_byte(a) (*(const unsigned char*)(a))\n')
 (p/'Fonts/GFXFF').mkdir(parents=True);(p/'Fonts/GFXFF/gfxfont.h').write_text('#pragma once\n#include <stdint.h>\nstruct GFXglyph{uint32_t bitmapOffset;uint8_t width,height,xAdvance;int8_t xOffset,yOffset;};struct GFXfont{uint8_t* bitmap;GFXglyph* glyph;uint16_t first,last;uint8_t yAdvance;};\n')
 (p/'render.cpp').write_text('''#include <algorithm>
#include <vector>
#include <cstdio>
#include <cassert>
#include "ui/cine_camera_view.h"
#include "core/cine_controls.h"
#include <Fonts/Instrument/data.inc>
struct Canvas{
 std::vector<uint16_t> pixels=std::vector<uint16_t>(280*240);
 uint16_t at(int x,int y)const{assert(x>=0&&x<280&&y>=0&&y<240);return pixels[y*280+x];}
 uint16_t readPixel(int x,int y)const{return at(x,y);}
 void fillRect(int x,int y,int w,int h,uint16_t ink){for(int yy=std::max(y,0);yy<std::min(y+h,240);++yy)for(int xx=std::max(x,0);xx<std::min(x+w,280);++xx)pixels[yy*280+xx]=ink;}
 void pushImage(int x,int y,int w,int h,uint16_t* src){assert(x>=0&&y>=0&&x+w<=280&&y+h<=240);for(int yy=0;yy<h;++yy)for(int xx=0;xx<w;++xx)pixels[(y+yy)*280+x+xx]=src[yy*w+xx];}
 void drawFastHLine(int x,int y,int w,uint16_t color){fillRect(x,y,w,1,color);}
 void drawBitmap(int x,int y,const uint8_t* bits,int w,int h,uint16_t fg,uint16_t bg){const int stride=(w+7)/8;for(int yy=0;yy<h;++yy)for(int xx=0;xx<w;++xx)fillRect(x+xx,y+yy,1,1,bits[yy*stride+xx/8]&(128>>(xx&7))?fg:bg);}
 void save(const char* path){FILE* f=fopen(path,"wb");assert(f);fprintf(f,"P6\\n280 240\\n255\\n");for(auto p:pixels){unsigned r=(p>>11)&31,g=(p>>5)&63,b=p&31;uint8_t v[]={uint8_t(r*255/31),uint8_t(g*255/63),uint8_t(b*255/31)};fwrite(v,1,3,f);}fclose(f);}
};
using TFT_eSPI=Canvas;
struct Node{const uint8_t* bitmap;uint16_t bitmapWidth,bitmapHeight;struct Box{int x,y,w,h;}rect;uint16_t fgColor=0xffff,bgColor=0;bool bitmapTransparent=false;unsigned bitmapBpp=1;};
template<class...T>void drawAlphaBitmap(T...){assert(false);}
static void renderBitmap(TFT_eSPI& c, const Node& n) {
    if (!n.bitmap || !n.bitmapWidth || !n.bitmapHeight || n.rect.w <= 0 || n.rect.h <= 0) return;
    if (n.bitmapBpp == 4) {
        drawAlphaBitmap(c, n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.bitmap,
                        n.bitmapWidth, n.bitmapHeight, n.fgColor, n.bgColor, n.bitmapTransparent);
        return;
    }
    if (n.rect.w == n.bitmapWidth && n.rect.h == n.bitmapHeight) {
        c.drawBitmap(n.rect.x, n.rect.y, n.bitmap, n.bitmapWidth, n.bitmapHeight, n.fgColor, n.bgColor);
        return;
    }
    c.fillRect(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.bgColor);
    const uint32_t stride = (n.bitmapWidth + 7u) / 8u;
    // Nearest-neighbor scaling in foreground runs; no scratch allocation.
    for (int y = 0; y < n.rect.h; ++y) {
        const uint32_t sourceRow = uint32_t(y) * n.bitmapHeight / n.rect.h * stride;
        int start = -1;
        for (int x = 0; x <= n.rect.w; ++x) {
            const uint32_t sx = uint32_t(x) * n.bitmapWidth / n.rect.w;
            const bool ink = x < n.rect.w && (pgm_read_byte(n.bitmap + sourceRow + sx / 8) & (0x80 >> (sx % 8)));
            if (ink && start < 0) start = x;
            if (!ink && start >= 0) {
                c.drawFastHLine(n.rect.x + start, n.rect.y + y, x - start, n.fgColor);
                start = -1;
            }
        }
    }
}
int main(int argc,char** argv){assert(argc==3);std::vector<uint16_t> source(224*168);FILE* f=fopen(argv[1],"rb");assert(f&&fread(source.data(),2,source.size(),f)==source.size());fclose(f);
 core::cine::WheelControl control;assert(control.color()&&!control.visible()&&!control.shown(0));
 control.show(100);assert(control.shown(100)&&control.shown(2099)&&!control.shown(2100));assert(!control.expire(2099)&&control.visible());assert(control.expire(2100)&&!control.visible());
 control.toggle(3000);assert(!control.color()&&control.shown(3000));control.toggle(3100);assert(control.color());
 control.show(0xffffff00u);assert(control.shown(uint32_t(0xffffff00u+1999)));assert(!control.shown(uint32_t(0xffffff00u+2000)));assert(control.expire(uint32_t(0xffffff00u+2000)));control.reset();assert(control.color()&&!control.visible());
 Canvas gear;ui::cine_view::settingsIcon(gear);int minX=280,maxX=0,minY=240,maxY=0;for(int y=0;y<50;++y)for(int x=0;x<80;++x)if(gear.pixels[y*280+x]){minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);}assert(maxX-minX+1==20&&maxY-minY+1==20);
 static_assert(sizeof(ui::cine_view::settingsMask)==60,"row-aligned20x20 scene bitmap");
 Canvas standard;Node icon{ui::cine_view::settingsMask,20,20,{18,14,20,20}};renderBitmap(standard,icon);assert(standard.pixels==gear.pixels);
 Canvas scaled;icon.rect.w=icon.rect.h=40;renderBitmap(scaled,icon);for(int y=0;y<40;++y)for(int x=0;x<40;++x)assert(scaled.at(18+x,14+y)==gear.at(18+x/2,14+y/2));
 for(int y=0;y<20;++y)assert((ui::cine_view::settingsMask[y*3+2]&15)==0);
 auto gearBit=[](int x,int y){return (ui::cine_view::settingsMask[y*3+x/8]>>(7-(x&7)))&1;};
 for(int y=0;y<20;++y)for(int x=0;x<20;++x)assert(gearBit(x,y)==gearBit(19-y,x));
 assert(gearBit(0,9)&&gearBit(19,9)&&gearBit(9,0)&&gearBit(9,19)&&!gearBit(9,9));
 control.show(100);control.show(1900);assert(control.shown(3899)&&!control.shown(3900));control.toggle(4000);assert(!control.color());assert(control.expire(6000)&&!control.color());control.reset();assert(control.color());control.reset(false);assert(!control.color()&&!control.visible());
 Canvas backing;std::fill(backing.pixels.begin(),backing.pixels.end(),0xffff);ui::cine_view::backdrop(backing,140,43,126,28,128);assert(backing.at(200,56)==0x7bef&&backing.at(140,43)==0xffff&&backing.at(145,43)==0x7bef);
 auto untouched=backing.pixels;ui::cine_view::backdrop(backing,-1,0,20,28,128);ui::cine_view::backdrop(backing,2147483647,0,20,28,128);ui::cine_view::backdrop(backing,0,2147483647,20,28,128);ui::cine_view::backdrop(backing,0,0,1,28,128);ui::cine_view::backdrop(backing,0,0,20,2147483647,128);assert(backing.pixels==untouched);
 for(bool wb:{false,true}){Canvas selected;ui::cine_view::chrome(selected,"Portra","1/1000",0,wb,true);unsigned ss=0,whiteBalance=0;for(int y=43;y<103;++y)for(int x=134;x<268;++x)if(selected.at(x,y)==kUiEdit){if(y<73)++whiteBalance;else ++ss;}assert(wb?(whiteBalance>0&&ss==0):(ss>0&&whiteBalance==0));}

 for(unsigned state=0;state<5;++state){Canvas canvas;ui::cine_view::image(canvas,source.data(),224,168);const auto before=canvas.pixels;ui::cine_view::chrome(canvas,state==2||state==3?"Portra":"Sunny","1/1000",state==4?5000:0,state==2||state==3,state==1||state==2||state==3);if(state==0||state==4)for(int y=43;y<103;++y)for(int x=134;x<268;++x)assert(canvas.pixels[y*280+x]==before[y*280+x]);for(int y=225;y<240;++y)for(int x=60;x<241;++x)assert(canvas.pixels[y*280+x]==before[y*280+x]);char path[1024];snprintf(path,sizeof(path),"%s/subtle-state-%u.ppm",argv[2],state);canvas.save(path);}
 Canvas reference;ui::cine_view::image(reference,source.data(),224,168);camera_photographer::overlays(reference,0);reference.fillRect(52,196,180,20,0);ui::pixel_text::text(reference,"B: PHOTO / HOLD: VIDEO",56,202,1,0xffff);char path[1024];snprintf(path,sizeof(path),"%s/stock-overlay.ppm",argv[2]);reference.save(path);
}
''')
 subprocess.run(['c++','-std=c++17','-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DORIGAMI_INSTRUMENT_FONT','-I'+str(p),'-I'+str(R/'include'),'-I'+str(R/'lib/TB_TFT_eSPI'),str(p/'render.cpp'),'-o',str(p/'render')],check=True)
 subprocess.run([str(p/'render'),str(O/'review-input.rgb565'),str(O)],check=True)
print('PASS actual Cine view and Instrument font: gear row alignment, safe alpha boxes, blue selection, hidden controls, 2s snap timing and rollover; ASan/UBSan')
