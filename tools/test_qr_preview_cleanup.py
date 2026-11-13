"""Exercise production QR renderer cleanup and chrome invalidation on the host."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    def write(name, text):
        path = temp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    write('Arduino.h', '''#pragma once
#include <algorithm>
#include <cstdint>
using std::min; using std::max;
inline uint32_t millis(){return 0;}
struct SerialMock {template<class... A> void printf(const char*,A...) {}};
static SerialMock Serial;
''')
    write('esp_heap_caps.h', '''#pragma once
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
extern bool failAllocation;
inline void* heap_caps_malloc(size_t n,int){return failAllocation?nullptr:malloc(n);}
inline void heap_caps_free(void* p){free(p);}
''')
    write('ui/qr_scanner_view.h', '#pragma once\nnamespace qr_scanner_view {template<class T> void overlay(T&) {} template<class T> void appOverlay(T&) {}}\n')
    write('hal/hal_display.h', '''#pragma once
#include <cstdint>
struct TFT_eSprite {
 uint16_t pixels[280*240]{};
 bool getSwapBytes(){return false;} void setSwapBytes(bool){}
 void pushImage(int,int,int,int,uint16_t*){}
 uint16_t readPixel(int x,int y){return pixels[y*280+x];}
 void fillRect(int,int,int,int,uint16_t){}
 void* getPointer(){return pixels;}
};
namespace hal {namespace display {TFT_eSprite& canvas();}}
''')
    write('main.cpp', '''
#include "ui/qr_preview.h"
#include "ui/native_frame.h"
#include "services/qr_decode.h"
#include "hal/hal_display.h"
#include <cassert>
bool failAllocation=false, decodeOk=true;
int invalidations=0;
void invalidateScreenChrome(){++invalidations;}
namespace hal {namespace display {TFT_eSprite& canvas(){static TFT_eSprite c;return c;}}}
namespace ui {NativeFramePresentation presentNativeFrame(uint16_t*){return {};}}
namespace services {namespace qr {
bool scanBusy(){return false;}
bool decodeScannerJpeg(const uint8_t*,size_t,PreviewBlitFn,void*,bool){return decodeOk;}
}}
int main(){
 {ui::QrPreview p;
  p.close();assert(invalidations==0);
  failAllocation=true;assert(!p.render(nullptr,0));p.close();assert(invalidations==0);
  failAllocation=false;assert(p.render(nullptr,0));p.close();assert(invalidations==1);
  p.close();assert(invalidations==1);
  // A failed decode can already have blitted partial blocks into the canvas.
  decodeOk=false;assert(!p.render(nullptr,0));p.close();assert(invalidations==2);
  decodeOk=true;assert(p.render(nullptr,0));
 }assert(invalidations==3);
}
''')
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(temp),'-I'+str(ROOT/'include'),str(ROOT/'src/ui/qr_preview.cpp'),str(temp/'main.cpp'),'-o',str(temp/'check')],check=True)
    subprocess.run([str(temp/'check')],check=True)
print('PASS QR cleanup: success, decode/allocation failures, repeated close, re-entry and destruction')
