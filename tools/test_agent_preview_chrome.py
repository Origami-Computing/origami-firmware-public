"""Exercise the real Agent preview renderer's chrome restoration and cleanup."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    def write(name, text):
        path = root/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    write('core/store.h', '''#pragma once
namespace core {namespace store {
inline int getU8(const char*,const char*,int){return 0;}
inline bool putU8(const char*,const char*,int){return true;}
}}
''')
    write('config/ui_layout.h','#pragma once\nconstexpr int kSystemStatusBarH=38;\n')
    write('config/ui_theme.h','#pragma once\nconstexpr int kUiBg=0xffff;\n')
    write('esp_heap_caps.h', '''#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
extern bool failAllocation;
inline void* heap_caps_malloc(size_t n,int){return failAllocation?nullptr:malloc(n);}
''')
    write('hal/hal_display.h', '''#pragma once
#include <stdint.h>
#include <vector>
inline uint32_t micros(){return 0;}
struct TFT_eSprite {
 std::vector<uint16_t> pixels=std::vector<uint16_t>(280*240,0xffff);
 int width(){return 280;}int height(){return 240;}
 void* getPointer(){return pixels.data();}
 void fillRect(int x,int y,int w,int h,uint16_t c){
  for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)pixels[yy*280+xx]=c;
 }
};
namespace hal {namespace display {
extern TFT_eSprite surface;
inline TFT_eSprite& canvas(){return surface;}
inline void markAllDirty(){}
}}
''')
    write('test.cpp', r'''
#include "ui/listening_style.h"
#include "hal/hal_display.h"
#include <algorithm>
#include <cassert>
bool failAllocation=false;
namespace hal{namespace display{TFT_eSprite surface;}}
int main(){
 using namespace ui::listening_style;
 auto& canvas=hal::display::surface;
 for(bool fail:{false,true})for(int style=0;style<kCount;++style){
  exitPreview();failAllocation=fail;
  canvas.fillRect(0,0,280,240,0xffff);
  drawPreview(canvas,style);
  const auto clean=canvas.pixels;
  assert(!ready()&&selected()==0); // preview never owns live capture/selection
  for(int frame=0;frame<3;++frame){
   canvas.fillRect(0,0,280,38,0); // obsolete clock/wifi pixels
   canvas.pixels[100*280+100]=0x1234; // preserve newly rendered menu content
   restorePreviewChrome(canvas,style);
   assert(std::equal(clean.begin(),clean.begin()+280*38,canvas.pixels.begin()));
   assert(canvas.pixels[100*280+100]==0x1234);
  }
  exitPreview();exitPreview();
 }
 // New style replaces the old underlay; re-entry can allocate after a failure.
 failAllocation=false;canvas.fillRect(0,0,280,240,0xffff);drawPreview(canvas,0);
 canvas.fillRect(0,0,280,240,0xffff);drawPreview(canvas,9);
 const auto last=canvas.pixels;
 canvas.fillRect(0,0,280,38,0);restorePreviewChrome(canvas,9);
 assert(canvas.pixels==last);
 exitPreview();releaseCache();
 // Overlay mode must use this frame's eyes/content, never the old snapshot.
 for(int style=0;style<kCount;++style){
  assert(select(style));
  canvas.fillRect(0,0,280,240,0x0000);
  assert(begin(nullptr));
  canvas.fillRect(0,0,280,240,0xffff);
  canvas.pixels[120*280+140]=0x1234;
  paint(1,nullptr,1,true,true);
  assert(canvas.pixels[120*280+20]!=0xffff);
  assert(canvas.pixels[120*280+140]==0x1234);
  const auto loud=canvas.pixels;
  canvas.fillRect(0,0,280,240,0xffff);
  canvas.pixels[120*280+140]=0x1234;
  paint(1,nullptr,1,true,true);
  assert(canvas.pixels==loud); // no accumulation on a fresh underlay
  canvas.fillRect(0,0,280,240,0xffff);
  paint(0,nullptr,0,true,true);
  assert(std::all_of(canvas.pixels.begin(),canvas.pixels.end(),[](uint16_t p){return p==0xffff;}));
  releaseCache();
 }
}
''')
    executable = root/'check'
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',
                    '-I'+str(root),'-I'+str(ROOT/'include'),
                    str(ROOT/'src/ui/listening_style.cpp'),str(root/'test.cpp'),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
print('PASS ten preview chrome underlays, stale-glyph removal, allocation fallback, cleanup and live overlay preservation/fade')
