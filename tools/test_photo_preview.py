from pathlib import Path
import tempfile,subprocess,sys
F=Path(__file__).resolve().parents[1];lib=F/'.pio/libdeps/esp32-s3-menu-n16r8-production/JPEGDEC/src'
with tempfile.TemporaryDirectory(prefix='photo-preview-') as td:
 p=Path(td)
 def put(name,s):
  f=p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(s)
 put('SD.h','#pragma once\n')
 put('Arduino.h','#pragma once\n#include <stdint.h>\nunsigned micros();\n')
 put('esp_heap_caps.h','#pragma once\n#include <stddef.h>\nconstexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;\nvoid* heap_caps_malloc(size_t,int);void heap_caps_free(void*);\n')
 put('core/watchdog.h','#pragma once\nnamespace core{namespace watchdog{inline void feed(){}}}\n')
 put('hal/hal_sd.h','#pragma once\n#include <stddef.h>\n#include <stdint.h>\nnamespace hal{namespace sd{struct StreamReader{void* handle=nullptr;size_t size=0,position=0;};bool openStream(const char*,StreamReader*);size_t readStream(StreamReader*,uint8_t*,size_t);void closeStream(StreamReader*);}}\n')
 put('hal/hal_display.h',r'''#pragma once
#include <stddef.h>
#include <stdint.h>
struct Surface{bool swap=false;bool getSwapBytes(){return swap;}void setSwapBytes(bool v){swap=v;}void pushImage(int,int,int,int,uint16_t*);};
namespace hal{namespace display{Surface& tft();bool drawJpegFileScaled(const char*,int,int,int,int);bool drawJpegBufferScaled(const uint8_t*,size_t,int,int,int,int);}}
''')
 put('main.cpp',r'''
#include "hal/hal_photo_preview.h"
#include "hal/hal_display.h"
#include "hal/hal_sd.h"
#include <cassert>
#include <map>
#include <vector>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <algorithm>
std::map<void*,size_t> owned;int budget=-1,opened=0,legacy=0,buffered=0,draws=0;bool readFail=false;size_t fakeSize=0;Surface output;uint16_t canvas[280*240]={};
unsigned micros(){static unsigned t=0;return ++t;}
void* heap_caps_malloc(size_t n,int caps){assert(caps==3);if(budget==0)return nullptr;if(budget>0)--budget;void* p=malloc(n);if(p)owned[p]=n;return p;}
void heap_caps_free(void* p){if(p)assert(owned.erase(p)==1);free(p);}
void Surface::pushImage(int x,int y,int w,int h,uint16_t* src){++draws;assert(x>=0&&y>=0&&x+w<=280&&y+h<=240);for(int j=0;j<h;++j)for(int i=0;i<w;++i){uint16_t v=src[j*w+i];canvas[(y+j)*280+x+i]=swap?v:uint16_t((v>>8)|(v<<8));}}
namespace hal{namespace display{Surface& tft(){return output;}bool drawJpegFileScaled(const char*,int,int,int,int){++legacy;return true;}bool drawJpegBufferScaled(const uint8_t*,size_t,int,int,int,int){++buffered;return true;}}
namespace sd{
bool openStream(const char* path,StreamReader* r){auto* v=new std::vector<uint8_t>;std::ifstream file(path,std::ios::binary);if(!file){delete v;return false;}v->assign(std::istreambuf_iterator<char>(file),{});r->handle=v;r->size=fakeSize?fakeSize:v->size();++opened;return true;}
size_t readStream(StreamReader* r,uint8_t* dst,size_t n){if(readFail)return 0;auto& v=*static_cast<std::vector<uint8_t>*>(r->handle);n=std::min({n,v.size()-r->position,size_t(997)});memcpy(dst,v.data()+r->position,n);r->position+=n;return n;}
void closeStream(StreamReader* r){if(r->handle){delete static_cast<std::vector<uint8_t>*>(r->handle);--opened;*r=StreamReader{};}}
}}
int main(int argc,char** argv){assert(argc==3);using P=hal::display::PhotoPreview;P preview;
 assert(!preview.draw(nullptr,0,38,280,180));assert(!preview.draw(argv[1],0,38,0,180));assert(!preview.draw(argv[1],0,38,281,180));
 for(int n=0;n<3;++n){budget=n;assert(preview.draw(argv[1],0,38,280,180));assert(preview.timing().fallback&&owned.empty()&&opened==0&&draws==0);}budget=-1;
 readFail=true;assert(preview.draw(argv[1],0,38,280,180)&&preview.timing().fallback&&owned.empty()&&opened==0);readFail=false;
 fakeSize=3*1024*1024;assert(preview.draw(argv[1],0,38,280,180)&&preview.timing().fallback&&owned.empty());fakeSize=0;
 assert(preview.draw(argv[1],0,38,280,180)&&!preview.timing().fallback);assert(opened==0&&draws==1&&owned.size()==3&&!output.swap);int previous=legacy;
 for(int i=0;i<10;++i){assert(preview.draw(argv[1],0,38,280,180)&&!preview.timing().fallback&&opened==0&&owned.size()==3);}assert(legacy==previous);
 std::ofstream ppm(argv[2],std::ios::binary);ppm<<"P6\n280 240\n255\n";for(auto v:canvas){uint8_t rgb[]={uint8_t((v>>11)*255/31),uint8_t(((v>>5)&63)*255/63),uint8_t((v&31)*255/31)};ppm.write(reinterpret_cast<char*>(rgb),3);}ppm.close();
 auto timing=preview.timing();assert(timing.width&&timing.height);assert(timing.scale==1||timing.scale==2||timing.scale==4||timing.scale==8);
 assert(preview.draw(argv[1],0,38,280,180,P::Mode::Buffered)&&buffered==1&&opened==0);assert(preview.draw(argv[1],0,38,280,180,P::Mode::Legacy)&&owned.empty());preview.close();preview.close();assert(owned.empty()&&opened==0);
}
''')
 subprocess.run(['c++','-std=c++11','-O1','-g','-fsanitize=address','-D__LINUX__','-I'+str(lib),'-c',str(lib/'JPEGDEC.cpp'),'-o',str(p/'codec.o')],check=True)
 subprocess.run(['c++','-std=c++11','-O1','-g','-fsanitize=address,undefined','-D__LINUX__','-I'+str(p),'-I'+str(F/'include'),'-I'+str(lib),str(p/'main.cpp'),str(F/'src/hal/hal_photo_preview.cpp'),str(p/'codec.o'),'-o',str(p/'test')],check=True)
 # Include real sensor files plus non-MCU-aligned portrait, landscape, and tiny
 # synthetic JPEGs to exercise rounding, edge callbacks and no-upscale fitting.
 images=[]
 generator=r'''from PIL import Image
import sys
for w,h in [(2048,1536),(640,480),(777,555),(481,641),(32,24),(1,1),(1280,720)]:
 g=Image.linear_gradient('L');im=Image.merge('RGB',(g.rotate(90).resize((w,h)),g.resize((w,h)),Image.new('L',(w,h),80)));im.save(sys.argv[1]+'/'+str(w)+'x'+str(h)+'.jpg',quality=92)
'''
 subprocess.run([sys.executable,'-c',generator,str(p)],check=True);images+=list(p.glob('*.jpg'))
 for i,source in enumerate(images):
  dest=p/(str(i)+'.ppm');subprocess.run([str(p/'test'),str(source),str(dest)],check=True)
  compare=r'''from PIL import Image,ImageChops,ImageStat
import sys
im=Image.open(sys.argv[1]).convert('RGB');w,h=im.size
if w>280 or h>180:
 if w*180>h*280:h=h*280//w;w=280
 else:w=w*180//h;h=180
ref=im.resize((w,h),Image.Resampling.BOX);out=Image.open(sys.argv[2]).crop(((280-w)//2,38+(180-h)//2,(280-w)//2+w,38+(180-h)//2+h));mean=ImageStat.Stat(ImageChops.difference(ref,out)).mean
print(im.size,'mean absolute RGB error',mean);assert sum(mean)/3<18
'''
  subprocess.run([sys.executable,'-c',compare,str(source),str(dest)],check=True)
print('PASS actual gallery renderer + pinned JPEGDEC with ASan/UBSan adapter and ASan codec: SD short reads/failure, OOM/fallback, bounded reuse/cleanup, 3MP/VGA/HD/odd/portrait/tiny geometry, channel order and image agreement')

# Directly fault the actual block mapper, independent of the vendor decoder.
text=(F/'src/hal/hal_photo_preview.cpp').read_text()
start=text.index('struct Sink {');end=text.index('void PhotoPreview::close()',start)
block=text[start:end];block=block[:block.rfind('}')]  # Exclude the anonymous namespace, irrespective of formatting.
with tempfile.TemporaryDirectory(prefix='photo-block-bounds-') as td:
 p=Path(td);source=p/'bounds.cpp'
 source.write_text(r'''#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <cassert>
#include <vector>
#define MAX_BUFFERED_PIXELS 2048
namespace core{namespace watchdog{void feed(){}}}
struct JPEGDRAW{void* pUser;int x,y,iWidthUsed,iHeight,iWidth;uint16_t* pPixels;};
'''+block+r'''
int main(){std::vector<uint16_t> output(240*180+2,0xbeef),input(272*4,0x1234);
 Sink s{output.data()+1,256,192,240,180,0,true};assert(!paint(nullptr));
 JPEGDRAW b{&s,0,0,256,4,272,input.data()};assert(paint(&b)&&s.valid&&s.written>0); // padded stride
 for(int which=0;which<7;++which){s.valid=true;JPEGDRAW bad=b;
  if(which==0)bad.x=INT_MAX;if(which==1)bad.y=-1;if(which==2)bad.iWidthUsed=INT_MAX;
  if(which==3)bad.iHeight=INT_MAX;if(which==4)bad.iWidth=INT_MAX;
  if(which==5)bad.pPixels=nullptr;if(which==6){bad.iWidth=1<<30;bad.iHeight=4;}
  assert(!paint(&bad)&&!s.valid);
 }
 assert(output.front()==0xbeef&&output.back()==0xbeef);
}
''')
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',str(source),'-o',str(p/'bounds')],check=True)
 subprocess.run([str(p/'bounds')],check=True)
print('PASS actual JPEG block mapper: padded strides, nulls, signed extremes, 32-bit overflow bounds and output guards under ASan/UBSan')
