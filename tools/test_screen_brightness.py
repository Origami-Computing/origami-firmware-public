"""Test the production RGB565 curve, S3 SPI writers and persistence owner.

The SPI register fake records the actual bytes emitted by the production writer,
including solid spans, swapped/native streams, odd lengths and chunk boundaries.
No USB. Use --zig PATH on Windows or --cxx with an existing C++17 compiler.
"""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default='c++')
    args = parser.parse_args()
    out = ROOT/'.cache/brightness-host'
    out.mkdir(parents=True, exist_ok=True)
    source = (ROOT/'lib/TB_TFT_eSPI/Processors/TFT_eSPI_ESP32_S3.c').read_text()
    start = source.index('void TFT_eSPI::pushBlock(uint16_t color, uint32_t len){\n#ifdef ORIGAMI_DISPLAY_DIMMING')
    end = source.index('#elif defined (SPI_18BIT_DRIVER)', start)
    production = source[start:end].rsplit('////////////////////////////////////////////////////////////////////////////////////////', 1)[0]
    for signature in [
        'void TFT_eSPI::pushPixelsDMA(uint16_t* image, uint32_t len)',
        'void TFT_eSPI::pushImageDMA(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t const* image)',
        'void TFT_eSPI::pushImageDMA(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* image, uint16_t* buffer)',
    ]:
        a=source.index(signature);b=source.index('{',a)+1;depth=1
        while depth:
            depth+=(source[b]=='{')-(source[b]=='}');b+=1
        production+='\n'+source[a:b]+'\n'
    (out/'driver.inc').write_text(production)
    tft_source=(ROOT/'lib/TB_TFT_eSPI/TFT_eSPI.cpp').read_text()
    a=tft_source.index('void TFT_eSPI::pushImageOutput(')
    b=tft_source.index('\n}',a)+2
    (out/'prepared.inc').write_text(tft_source[a:b])
    # Public scale API and state are taken directly from the production class.
    header = (ROOT/'lib/TB_TFT_eSPI/TB_TFT_eSPI.h').read_text()
    public = header.split('  struct OutputStats {', 1)[1].split('#endif', 1)[0]
    public = '  struct OutputStats {'+public
    private = header.split('  const PixelScale565* _outputScale', 1)[1].split('#endif', 1)[0]
    private = '  const PixelScale565* _outputScale'+private
    code = r'''
#include "PixelScale565.h"
#include "hal/display_brightness_curve.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <set>
#ifdef NDEBUG
#error Brightness checks require assertions enabled
#endif
#define ORIGAMI_DISPLAY_DIMMING 1
#define CONFIG_IDF_TARGET_ESP32S3 1
static uint32_t words[16], bits;
typedef int esp_err_t;
struct spi_transaction_t { void* user; const void* tx_buffer; unsigned length,flags; };
constexpr int portMAX_DELAY=-1,ESP_OK=0;
int spi_device_queue_trans(int,spi_transaction_t*,int){assert(false&&"Dimmed output must not queue caller-owned DMA buffers");return 0;}
static std::vector<uint8_t> wire;
static void emit() {
  for(unsigned i=0;i<(bits+1)/8;++i) wire.push_back((words[i/4]>>(8*(i%4)))&255);
}
constexpr uint32_t SPI_USR=1, SPI_UPDATE=2;
struct Command {
 operator uint32_t() const {return 0;}
 void operator=(uint32_t value) {if(value==SPI_USR)emit();}
} command;
#define _spi_w words
#define _spi_cmd (&command)
#define _spi_mosi_dlen (&bits)
#define SPI_PORT 0
#define SPI_MOSI_DLEN_REG(p) 128
#define SPI_CMD_REG(p) 132
#define SPI_W0_REG(p) 0
''' + '\n'.join(f'#define SPI_W{i}_REG(p) {i*4}' for i in range(1,16)) + r'''
#define WRITE_PERI_REG(addr,v) do{if((addr)==128)bits=(v);else words[(addr)/4]=(v);}while(0)
#define READ_PERI_REG(addr) 0
#define SET_PERI_REG_MASK(addr,v) do{if((v)==SPI_USR)emit();}while(0)
#define DAT8TO32(P) ((uint32_t)P[0]<<8 | P[1] | P[2]<<24 | P[3]<<16)
class TFT_eSPI {
public:
''' + public + private + r'''
 bool _swapBytes=false;
 void pushBlock(uint16_t,uint32_t);
 void pushPixels(const void*,uint32_t);
 void pushSwapBytePixels(const void*,uint32_t);
 bool DMA_Enabled=true;int dmaHAL=0,spiBusyCheck=0,waits=0;
 int _vpX=0,_vpY=0,_vpW=280,_vpH=240,window[4]={},images=0;
 uint16_t* imageSeen=nullptr;bool preparedMode=false;
 void dmaWait(){++waits;spiBusyCheck=0;}
 void setAddrWindow(int x,int y,int w,int h){window[0]=x;window[1]=y;window[2]=w;window[3]=h;}
 void pushImage(int x,int y,int w,int h,uint16_t* image){++images;imageSeen=image;setAddrWindow(x,y,w,h);if(preparedMode){assert(!_outputScale);pushPixels(image,w*h);}}
 void pushImageOutput(int,int,int,int,uint16_t*);
 void pushPixelsDMA(uint16_t*,uint32_t);
 void pushImageDMA(int32_t,int32_t,int32_t,int32_t,uint16_t const*);
 void pushImageDMA(int32_t,int32_t,int32_t,int32_t,uint16_t*,uint16_t*);
};
#include "driver.inc"
#include "prepared.inc"
static double linear(double c){return c<=.04045?c/12.92:std::pow((c+.055)/1.055,2.4);}
static double lightness(uint16_t c){
 const double y=.2126*linear((c>>11)/31.)+.7152*linear(((c>>5)&63)/63.)+.0722*linear((c&31)/31.);
 return y<=216./24389 ? y*(24389./27) : 116*std::cbrt(y)-16;
}
int main(){
 PixelScale565 scale;
 assert(sizeof(scale)<=456);
 for(unsigned c=0;c<65536;++c)assert(scale.map(c)==c);
 unsigned previous[65536]={};
 double maxError=0;
 for(unsigned p=0;p<=100;++p){
  scale.set(p);
  assert(scale.map(0)==0);
  maxError=std::max(maxError,std::abs(lightness(scale.map(65535))-p));
  for(unsigned c=0;c<65536;++c){
   const unsigned m=scale.map(c),old=previous[c];
   assert((m>>11)<=(c>>11)&&((m>>5)&63)<=((c>>5)&63)&&(m&31)<=(c&31));
   assert((m>>11)>=(old>>11)&&((m>>5)&63)>=((old>>5)&63)&&(m&31)>=(old&31));
   if(!p)assert(m==0);
   if(p==100)assert(m==c);
   previous[c]=m;
  }
 }
 assert(maxError<1.7);
 for(unsigned p:{5u,10u}) {
  scale.set(p);std::set<unsigned> rounded,averaged;
  for(unsigned c=0;c<32;++c) {
   const uint16_t rgb=(c<<11)|((c*63/31)<<5)|c;
   rounded.insert(scale.map(rgb));unsigned r=0,g=0,b=0;
   for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
    unsigned m=scale.mapDithered(rgb,x,y);r+=m>>11;g+=(m>>5)&63;b+=m&31;
   }
   averaged.insert((r<<20)|(g<<10)|b);
  }
  assert(averaged.size()>rounded.size()*2);
  printf("PASS %u%% neutral ramp: %zu rounded shades -> %zu spatial-average shades\n",p,rounded.size(),averaged.size());
 }
 // Dithered spatial averages retain fractional tones. Check every input color
 // at low levels, channel bounds and exact endpoints, with the production map.
 for(unsigned p:{0u,5u,10u,25u,100u}) {
  scale.set(p);
  for(unsigned c=0;c<65536;++c) {
   unsigned sums[3]={};
   for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x) {
    const unsigned m=scale.mapDithered(c,x,y);
    assert((m>>11)<=(c>>11)&&((m>>5)&63)<=((c>>5)&63)&&(m&31)<=(c&31));
    if(!p)assert(m==0);
    if(p==100)assert(m==c);
    sums[0]+=m>>11;sums[1]+=(m>>5)&63;sums[2]+=m&31;
   }
   const double t=(p+16.)/116.;const double gain=p<=8?p/903.2963:t*t*t;
   const unsigned codes[]={c>>11,(c>>5)&63,c&31}, maxima[]={31,63,31};
   for(unsigned ch=0;ch<3;++ch) {
    double l=linear(double(codes[ch])/maxima[ch])*gain;
    double encoded=l<=.0031308?12.92*l:1.055*std::pow(l,1/2.4)-.055;
    assert(std::abs(double(sums[ch])/16-encoded*maxima[ch])<.035);
   }
  }
 }
 unsigned lastDrive=0;
 for(unsigned p=0;p<=100;++p){
  const unsigned drive=hal::display::calibratedBrightnessDrive(p);
  assert(drive>=lastDrive&&drive<=100);lastDrive=drive;
 }
 assert(hal::display::calibratedBrightnessDrive(0)==0);
 assert(hal::display::calibratedBrightnessDrive(5)==5);
 assert(hal::display::calibratedBrightnessDrive(25)==27);
 assert(hal::display::calibratedBrightnessDrive(40)==50);
 assert(hal::display::calibratedBrightnessDrive(50)==59);
 assert(hal::display::calibratedBrightnessDrive(75)==79);
 assert(hal::display::calibratedBrightnessDrive(90)==88);
 assert(hal::display::calibratedBrightnessDrive(999)==100);
 TFT_eSPI tft;
 // A sprite's inherited null pointer cannot dim offscreen image data.
 assert(tft.outputColor(0xFFFF)==0xFFFF);
 tft.setOutputScale(&scale);
 unsigned cases=0;
 for(bool calibrated:{false,true})
 for(unsigned percent:{0u,5u,25u,50u,75u,100u}){
  scale.set(calibrated?hal::display::calibratedBrightnessDrive(percent):percent);
  for(unsigned count:{0u,1u,2u,3u,15u,16u,17u,31u,32u,33u,63u,64u,65u,279u,1024u,67200u}){
   for(bool native:{false,true}){
    std::vector<uint8_t> input(count*2+8,0xA5),expected;
    for(unsigned i=0;i<count;++i){
     const uint16_t c=(i*997+0xF81F)&65535,m=scale.map(c);
     input[i*2]=native?c&255:c>>8;input[i*2+1]=native?c>>8:c&255;
     expected.push_back(m>>8);expected.push_back(m&255);
    }
    const auto original=input;
    wire.clear();tft._swapBytes=native;tft.pushPixels(input.data(),count);
    assert(wire==expected&&input==original&&tft._swapBytes==native);
    if(native){wire.clear();tft._swapBytes=false;tft.pushSwapBytePixels(input.data(),count);assert(wire==expected&&!tft._swapBytes);}
    ++cases;
   }
  }
  for(unsigned c:{0u,0xFFFFu,0xF800u,0x07E0u,0x001Fu,0x4208u})
   for(unsigned count:{0u,1u,2u,31u,32u,33u,127u}){
    wire.clear();tft.pushBlock(c,count);
    assert(wire.size()==count*2);
    for(unsigned i=0;i<count;++i)assert(((wire[i*2]<<8)|wire[i*2+1])==scale.map(c));
    ++cases;
   }
 }
 scale.set(50);
 tft.preparedMode=true;
 uint16_t prepared[]={0x1234,0xABCD,0x001F};
 for(bool native:{false,true}) {
  wire.clear();tft._swapBytes=native;
  tft.pushImageOutput(0,0,3,1,prepared);
  std::vector<uint8_t> expected;
  for(uint16_t c:prepared){expected.push_back(native?c>>8:c&255);expected.push_back(native?c&255:c>>8);}
  assert(wire==expected&&tft.outputColor(0xFFFF)==scale.map(0xFFFF)&&tft._swapBytes==native);
 }
 tft.preparedMode=false;
 for(bool native:{false,true})for(unsigned count:{0u,1u,31u,32u,33u,1024u}){
  std::vector<uint16_t> image(count+2,0xA55A);std::vector<uint8_t> expected;
  for(unsigned i=0;i<count;++i){const uint16_t c=(i*997+0xF800)&65535,m=scale.map(c);image[i]=native?c:uint16_t((c<<8)|(c>>8));expected.push_back(m>>8);expected.push_back(m&255);}
  const auto original=image;wire.clear();tft._swapBytes=native;tft.pushPixelsDMA(image.data(),count);
  assert(wire==expected&&image==original&&tft._swapBytes==native);
 }
 std::vector<uint16_t> fixed(37,0x00F8);const auto original=fixed;
 tft._swapBytes=true;wire.clear();tft.pushImageDMA(11,19,5,7,static_cast<const uint16_t*>(fixed.data()));
 assert(tft.window[0]==11&&tft.window[1]==19&&wire.size()==70&&fixed==original&&tft._swapBytes);
 for(unsigned i=0;i<35;++i)assert(((wire[i*2]<<8)|wire[i*2+1])==scale.map(0xF800));
 uint16_t scratch[4]={1,2,3,4};const int oldImages=tft.images;
 tft.pushImageDMA(-2,-3,5,7,fixed.data(),scratch);
 assert(tft.images==oldImages+1&&tft.imageSeen==fixed.data()&&tft.window[0]==-2&&tft.window[1]==-3&&fixed==original&&scratch[0]==1&&scratch[3]==4);
 assert(tft.outputStats().dmaFallback==12);
 printf("PASS all 65536 colors at 101 levels: monotonic channels, exact 100%%, black endpoints; max white L* error %.3f; %u real SPI cases plus all three dimmed DMA fallback adapters; source bytes preserved\n",maxError,cases);
}
'''
    (out/'output.cpp').write_text(code)
    env = os.environ.copy()
    env['ZIG_GLOBAL_CACHE_DIR'] = str(out/'zig-global')
    env['ZIG_LOCAL_CACHE_DIR'] = str(out/'zig-local')
    compiler = [args.zig, 'c++'] if args.zig else [args.cxx]

    def run(name, files, includes):
        binary = out/(name+'.exe' if os.name=='nt' else name)
        subprocess.run(compiler+['-std=c++17','-O2','-UNDEBUG','-Wall','-Wextra']+
                       ['-I'+str(i) for i in includes]+[str(f) for f in files]+['-o',str(binary)],
                       cwd=ROOT, env=env, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True)

    run('output', [out/'output.cpp'], [out, ROOT/'lib/TB_TFT_eSPI',ROOT/'include'])
    fake = out/'fake/hal'
    fake.mkdir(parents=True, exist_ok=True)
    (fake/'hal_display.h').write_text('#pragma once\nnamespace hal {namespace display {void setBrightness(unsigned);}}\n')
    (out/'settings.cpp').write_text(r'''
#include "services/screen_brightness.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
static int stored=100,reads=0,writes=0,output=100;
static bool fail=false;
namespace core{namespace store{
unsigned char getU8(const char*,const char*,unsigned char){++reads;return stored;}
bool putU8(const char*,const char*,unsigned char value){++writes;if(fail)return false;stored=value;return true;}
}}
namespace hal{namespace display{void setBrightness(unsigned value){output=value;}}}
int main(int argc,char**argv){
 using namespace services::screen_brightness;
 stored=argc>1?std::atoi(argv[1]):100;
 assert(percent()==100&&savedPercent()==100&&reads==0); // builders cannot initialize hardware/storage
 initialize();const int initial=stored<5?5:stored>100?100:stored;
 assert(percent()==initial&&savedPercent()==initial&&output==initial&&reads==1);
 initialize();assert(reads==1);
 set(-999);assert(percent()==5&&output==5&&writes==0);
 set(999);assert(percent()==100&&output==100&&writes==0);
 set(35);fail=true;assert(!save()&&savedPercent()==initial&&percent()==35);
 fail=false;assert(save()&&stored==35&&savedPercent()==35);
 const int attempts=writes;assert(save()&&writes==attempts);
 set(80);discard();assert(percent()==35&&output==35&&writes==attempts);
 puts("PASS production persistence: load once, validation, live preview, failed-save retry, unchanged-write suppression, discard");
}
''')
    run('settings', [out/'settings.cpp', ROOT/'src/services/screen_brightness.cpp'], [out/'fake',ROOT/'include'])
    for invalid in (0,4,5,255):
        subprocess.run([str(out/('settings.exe' if os.name=='nt' else 'settings')),str(invalid)],check=True)
    source=(ROOT/'src/apps/settings_menu_app.cpp').read_text(encoding='utf-8')
    start=source.index('static void finishBrightness(')
    end=source.index('{',start)+1
    depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    helper=source[start:end]
    (out/'editor.cpp').write_text(r'''
#include <cassert>
#include <cstdio>
bool s_brightnessSaveFailed=false,s_needRedraw=false,saveOk=false;
int returns=0,attempts=0,discards=0,invalidations=0;
void showMain(){++returns;}
namespace services{namespace screen_brightness{
bool save(){++attempts;return saveOk;}
void discard(){++discards;}
}}
namespace hal{namespace display{void markAllDirty(){++invalidations;}}}
'''+helper+r'''
int main(){
 finishBrightness();assert(s_brightnessSaveFailed&&s_needRedraw&&returns==0&&attempts==1&&invalidations==1);
 finishBrightness();assert(returns==0&&attempts==2); // C retries without claiming success
 finishBrightness(true);assert(discards==1&&returns==1&&attempts==2); // Back cancels failed write
 s_brightnessSaveFailed=false;saveOk=true;finishBrightness(true);
 assert(returns==2&&attempts==3&&discards==1); // ordinary Back still saves
 puts("PASS actual editor helper: save failure stays visible, retry and cancel preserve last saved level");
}
''')
    run('editor',[out/'editor.cpp'],[out])


if __name__ == '__main__': main()
