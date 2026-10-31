"""Check real preview decoder ownership/crop/transpose against synthetic JPEG MCUs."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('Arduino.h','#pragma once\nstruct SerialStub{template<class... T>void printf(const char*,T...) {}};extern SerialStub Serial;\ninline unsigned micros(){static unsigned tick=0;return ++tick;}\n')
    write('esp_heap_caps.h',r'''#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_SPIRAM=2,MALLOC_CAP_8BIT=4;
extern bool failInternal,failPsram;extern int allocations,frees,lastCaps;
inline void* heap_caps_malloc(size_t n,int caps){lastCaps=caps;if((caps&1)?failInternal:failPsram)return nullptr;++allocations;return malloc(n);}
inline void* heap_caps_aligned_calloc(size_t,size_t,size_t n,int caps){return heap_caps_malloc(n,caps);}
inline void heap_caps_free(void*p){if(p){++frees;free(p);}}
''')
    write('JPEGDEC.h',r'''#pragma once
#include <cstdint>
#include <vector>
#include <algorithm>
constexpr int RGB565_BIG_ENDIAN=42,JPEG_SCALE_HALF=7,JPEG_SCALE_QUARTER=4;
struct JPEGDRAW{int x,y,iWidthUsed,iHeight,iWidth;uint16_t*pPixels;void*pUser;};
extern int sourceWidth,sourceHeight,blockWidth,blocks,lastScale,lastPixelType,closes,constructs,destructs;
extern bool openOk,decodeOk,badBlock;
inline uint16_t pixel(int x,int y){return uint16_t(((y*320+x)*37)^0x5a5a);}
class JPEGDEC {
int(*writer)(JPEGDRAW*)=nullptr;void*user=nullptr;
public:
JPEGDEC(){++constructs;}~JPEGDEC(){++destructs;}
int openRAM(uint8_t*,int,int(*fn)(JPEGDRAW*)){writer=fn;return openOk;}
int getWidth(){return sourceWidth;}int getHeight(){return sourceHeight;}
void setUserPointer(void*p){user=p;}void setPixelType(int t){lastPixelType=t;}
void close(){++closes;}
int decode(int,int,int scale){lastScale=scale;if(!decodeOk)return 0;
const int height=sourceWidth==1280?180:240;
for(int y=0;y<height;y+=8)for(int x=0;x<320;x+=blockWidth){
 int used=std::min(blockWidth,320-x),stride=blockWidth+3;
 int rows=std::min(8,height-y);
 std::vector<uint16_t> values(stride*8,0xdead);
 for(int yy=0;yy<8;++yy)for(int xx=0;xx<used;++xx)values[yy*stride+xx]=pixel(x+xx,y+yy);
 JPEGDRAW block{badBlock?-1:x,y,used,rows,stride,values.data(),user};++blocks;
 if(!writer(&block))return 0;
}return 1;}
};
''')
    write('esp_jpeg_dec.h',r'''#pragma once
#include <cstdint>
#include <cstdlib>
constexpr int JPEG_PIXEL_FORMAT_RGB565_BE=0,JPEG_ROTATE_0D=0,JPEG_ROTATE_270D=270,JPEG_ERR_OK=0;
struct Resolution{int width,height;};
struct jpeg_dec_config_t{int output_type;Resolution scale;Resolution clipper;int rotate;bool block_enable;};
struct jpeg_dec_io_t{uint8_t*inbuf;int inbuf_len;uint8_t*outbuf;int out_size;};
struct jpeg_dec_header_info_t{int width,height;};
extern int sourceWidth,sourceHeight;extern bool openOk,decodeOk;
inline int& parseCalls(){static int calls=0;return calls;}
inline bool& parseOk(){static bool ok=true;return ok;}
inline bool& headerSizeOk(){static bool ok=true;return ok;}
inline bool& outputSizeOk(){static bool ok=true;return ok;}
inline bool& returnedSizeOk(){static bool ok=true;return ok;}
inline int jpeg_dec_open(jpeg_dec_config_t*c,void**p){if(!openOk)return -1;*p=malloc(sizeof(*c));if(*p)*static_cast<jpeg_dec_config_t*>(*p)=*c;return *p?0:-1;}
inline int jpeg_dec_close(void*p){free(p);return 0;}
inline int& blockRows(){static int rows=16;return rows;}
inline int& currentRow(){static int row=0;return row;}
inline int jpeg_dec_parse_header(void*handle,jpeg_dec_io_t*,jpeg_dec_header_info_t*h){++parseCalls();currentRow()=0;bool block=static_cast<jpeg_dec_config_t*>(handle)->block_enable;h->width=block?sourceWidth:240;h->height=block?sourceHeight:280;if(!headerSizeOk())++h->width;return parseOk()?0:-1;}
inline int jpeg_dec_get_outbuf_len(void*h,int*n){*n=(static_cast<jpeg_dec_config_t*>(h)->block_enable?sourceWidth*blockRows()*2:280*240*2)+(outputSizeOk()?0:16);return 0;}
inline int jpeg_dec_get_process_count(void*h,int*n){*n=static_cast<jpeg_dec_config_t*>(h)->block_enable?sourceHeight/blockRows():1;return 0;}
inline int jpeg_dec_process(void*h,jpeg_dec_io_t*io){if(!decodeOk)return -1;auto*p=reinterpret_cast<uint16_t*>(io->outbuf);
auto& config=*static_cast<jpeg_dec_config_t*>(h);
if(!config.block_enable){
 if(config.scale.width!=320||config.scale.height!=240||config.clipper.width!=280||config.clipper.height!=240||config.rotate!=JPEG_ROTATE_270D)return -1;
 for(int x=0;x<280;++x)for(int y=0;y<240;++y)p[(279-x)*240+y]=pixel((x+20)*sourceWidth/320,y*sourceHeight/240);
 io->out_size=280*240*2+(returnedSizeOk()?0:2);return 0;
}
for(int y=0;y<blockRows();++y)for(int x=0;x<sourceWidth;++x)p[y*sourceWidth+x]=pixel(x,y+currentRow());currentRow()+=blockRows();io->out_size=sourceWidth*blockRows()*2+(returnedSizeOk()?0:2);return 0;}
''')
    write('main.cpp',r'''
#include "core/preview_decoder.h"
#include <Arduino.h>
#include <JPEGDEC.h>
#include <esp_jpeg_dec.h>
#include <esp_heap_caps.h>
#include <cassert>
#include <climits>
SerialStub Serial;
bool failInternal=false,failPsram=false,openOk=true,decodeOk=true,badBlock=false;
int allocations=0,frees=0,lastCaps=0,sourceWidth=640,sourceHeight=480,blockWidth=16,
 blocks=0,lastScale=0,lastPixelType=0,closes=0,constructs=0,destructs=0;
bool abortAfterTwo(void*p){return ++*static_cast<int*>(p)==2;}
bool abortAfterThree(void*p){return ++*static_cast<int*>(p)==3;}
int main(){using Decoder=core::images::PreviewDecoder;using Result=Decoder::Result;
uint8_t jpeg=0;std::vector<uint16_t> output(Decoder::kPixels,0);
{Decoder decoder;assert(!decoder.ready());assert(decoder.decode(&jpeg,1,output.data())==Result::Invalid);
assert(decoder.prepare()&&decoder.ready()&&(lastCaps&MALLOC_CAP_INTERNAL));
for(int width:{320,640})for(int mcu:{8,16,17,32}){
 sourceWidth=width;sourceHeight=width==640?480:240;blockWidth=mcu;
 assert(decoder.decode(&jpeg,1,output.data())==Result::Decoded);
 assert(lastScale==(width==640?JPEG_SCALE_HALF:0)&&lastPixelType==RGB565_BIG_ENDIAN);
 for(int x=0;x<280;++x)for(int y=0;y<240;++y)assert(output[(279-x)*240+y]==pixel(x+20,y));
}
for(int width:{320,640,1280})for(int mcu:{8,16,17,32}){
 sourceWidth=width;sourceHeight=width==1280?720:width==640?480:240;blockWidth=mcu;
 assert(decoder.decode(&jpeg,1,output.data(),nullptr,nullptr,true)==Result::Decoded);
 assert(lastScale==(width==1280?JPEG_SCALE_QUARTER:width==640?JPEG_SCALE_HALF:0));
 const int top=width==1280?41:15,sh=width==1280?180:240,dh=width==1280?157:210;
 for(int x=0;x<280;++x)for(int y=0;y<240;++y)
  assert(output[(279-x)*240+y]==(y>=top&&y<top+dh?pixel(x*320/280,(y-top)*sh/dh):0));
}
assert(decoder.decode(&jpeg,1,output.data())==Result::Invalid); // HD crop is not supported.
sourceWidth=640;sourceHeight=480;
int calls=0,before=blocks;assert(decoder.decode(&jpeg,1,output.data(),abortAfterThree,&calls)==Result::Interrupted);
assert(calls==3&&blocks-before==3);
sourceWidth=800;assert(decoder.decode(&jpeg,1,output.data())==Result::Invalid);sourceWidth=640;
openOk=false;before=closes;assert(decoder.decode(&jpeg,1,output.data())==Result::Invalid&&closes==before);openOk=true;
decodeOk=false;assert(decoder.decode(&jpeg,1,output.data())==Result::Invalid);decodeOk=true;
badBlock=true;assert(decoder.decode(&jpeg,1,output.data())==Result::Invalid);badBlock=false;
assert(decoder.decode(nullptr,1,output.data())==Result::Invalid);
assert(decoder.decode(&jpeg,size_t(INT32_MAX)+1,output.data())==Result::Invalid);
assert(decoder.decode(&jpeg,1,nullptr)==Result::Invalid);
failInternal=true;assert(decoder.prepare()&&(lastCaps&MALLOC_CAP_SPIRAM));
failPsram=true;assert(!decoder.prepare()&&!decoder.ready());decoder.close();decoder.close();
failPsram=false;assert(decoder.prepare());
}
{Decoder decoder;sourceWidth=640;sourceHeight=480;failInternal=false;
assert(decoder.prepareNative()&&decoder.ready());
// Real marker structure; codec pixels remain mocked to test the adapter.
std::vector<uint8_t> nativeJpeg={0xff,0xd8,0xff,0xc0,0,17,8,1,224,2,128,3,1,0x21,0,2,0x11,1,3,0x11,1,
 0xff,0xda,0,12,3,1,0,2,0x11,3,0x11,0,63,0};
const auto validJpeg=nativeJpeg;
const auto unchanged=output;
const int parsedBefore=parseCalls();
for(size_t length=0;length<nativeJpeg.size();++length)
 assert(decoder.decode(nativeJpeg.data(),length,output.data(),nullptr,nullptr,true)==Result::Invalid&&output==unchanged);
for(auto bad:std::vector<std::pair<int,int>>{{0,0},{7,0},{9,0},{3,0xc2},{4,255},{5,1},{6,12},{11,4},{13,0x12},{16,0x12},{19,0x12},{22,0xd9},{24,255}}){
 nativeJpeg=validJpeg;nativeJpeg[bad.first]=bad.second;
 assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Invalid&&output==unchanged);
}
assert(parseCalls()==parsedBefore);
nativeJpeg=validJpeg;
// APP payload may contain marker-looking bytes; skip by segment length.
nativeJpeg.insert(nativeJpeg.begin()+2,{0xff,0xe1,0,6,0xff,0xc2,0xff,0xd9});
assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Decoded);
nativeJpeg=validJpeg;
for(int sampling:{0x11,0x21,0x22}){nativeJpeg[13]=sampling;assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Decoded);}
nativeJpeg={0xff,0xd8,0xff,0xc0,0,11,8,1,224,2,128,1,1,0x11,0,0xff,0xda,0,8,1,1,0,0,63,0};
assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Decoded);
nativeJpeg=validJpeg;nativeJpeg.insert(nativeJpeg.begin()+2,0xff);
assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Decoded);
nativeJpeg=validJpeg;
// Duplicate frames must not let a later unsupported SOF bypass validation.
nativeJpeg.insert(nativeJpeg.begin()+21,validJpeg.begin()+2,validJpeg.begin()+21);
assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Invalid);
nativeJpeg=validJpeg;
// HD scratch expansion failure must preserve both existing scratch and pixels.
sourceWidth=1280;sourceHeight=720;
nativeJpeg[7]=sourceHeight>>8;nativeJpeg[8]=sourceHeight;nativeJpeg[9]=sourceWidth>>8;nativeJpeg[10]=sourceWidth;
auto beforeHd=output;failPsram=true;
assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Invalid && output==beforeHd);
failPsram=false;
for(int rows:{8,16})for(int width:{320,640,1280}){blockRows()=rows;sourceWidth=width;sourceHeight=width==1280?720:width==640?480:240;nativeJpeg[7]=sourceHeight>>8;nativeJpeg[8]=sourceHeight;nativeJpeg[9]=sourceWidth>>8;nativeJpeg[10]=sourceWidth;
std::fill(output.begin(),output.end(),0xdead); // Every pixel, including bars, must be replaced.
assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Decoded);
const int height=280*sourceHeight/sourceWidth,top=(240-height)/2;
for(int x=0;x<280;++x)for(int y=0;y<240;++y)assert(output[(279-x)*240+y]==(y>=top&&y<top+height?pixel(x*sourceWidth/280,(y-top)*sourceHeight/height):0));}
for(int rows:{8,16})for(int width:{320,640}){blockRows()=rows;sourceWidth=width;sourceHeight=width==640?480:240;nativeJpeg[7]=sourceHeight>>8;nativeJpeg[8]=sourceHeight;nativeJpeg[9]=sourceWidth>>8;nativeJpeg[10]=sourceWidth;
std::fill(output.begin(),output.end(),0xdead);
assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data())==Result::Decoded);
for(int x=0;x<280;++x)for(int y=0;y<240;++y)
 assert(output[(279-x)*240+y]==pixel((x+20)*sourceWidth/320,y*sourceHeight/240));
int cancelledCrop=0;
assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),abortAfterTwo,&cancelledCrop)==Result::Interrupted&&cancelledCrop==2);
}

auto saved=output;
parseOk()=false;assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Invalid&&output==saved);parseOk()=true;
outputSizeOk()=false;assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Invalid&&output==saved);outputSizeOk()=true;
returnedSizeOk()=false;assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Invalid);returnedSizeOk()=true;
int cancelled=0;assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),abortAfterTwo,&cancelled,true)==Result::Interrupted&&cancelled==2);
sourceWidth=800;assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Invalid);sourceWidth=320;
decodeOk=false;assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),output.data(),nullptr,nullptr,true)==Result::Invalid);decodeOk=true;
// Whole-frame camera mode uses the borrowed aligned destination, no scratch.
failPsram=true;assert(decoder.prepareNative(true));failPsram=false;
alignas(16) uint16_t crop[Decoder::kPixels+16];
for(int width:{320,640}){
 sourceWidth=width;sourceHeight=width==640?480:240;nativeJpeg[7]=sourceHeight>>8;nativeJpeg[8]=sourceHeight;nativeJpeg[9]=sourceWidth>>8;nativeJpeg[10]=sourceWidth;
 std::fill(crop,crop+Decoder::kPixels+16,0xabcd);
 assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),crop+8)==Result::Decoded);
 for(int x=0;x<280;++x)for(int y=0;y<240;++y)assert(crop[8+(279-x)*240+y]==pixel((x+20)*sourceWidth/320,y*sourceHeight/240));
 for(int i=0;i<8;++i)assert(crop[i]==0xabcd&&crop[8+Decoder::kPixels+i]==0xabcd);
 assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),crop+1)==Result::Invalid);
 assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),crop+8,nullptr,nullptr,true)==Result::Invalid);
 int cancel=0;assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),crop+8,abortAfterThree,&cancel)==Result::Interrupted&&cancel==3);
}
headerSizeOk()=false;assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),crop+8)==Result::Invalid);headerSizeOk()=true;
outputSizeOk()=false;assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),crop+8)==Result::Invalid);outputSizeOk()=true;
returnedSizeOk()=false;assert(decoder.decode(nativeJpeg.data(),nativeJpeg.size(),crop+8)==Result::Invalid);returnedSizeOk()=true;
openOk=false;assert(!decoder.prepareNative()&&!decoder.ready());openOk=true;
failPsram=true;assert(!decoder.prepareNative());failPsram=false;
assert(decoder.prepareNative());assert(decoder.prepare());assert(decoder.prepareNative());decoder.close();decoder.close();
}assert(allocations==frees&&constructs==destructs);
}
''')
    for flags in ([],['-DORIGAMI_DEV_SERIAL_INPUT']):
        subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',*flags,'-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/core/preview_decoder.cpp'),str(root/'main.cpp'),'-o',str(root/'test')],check=True)
        subprocess.run([str(root/'test')],check=True)
    trial=(root/'main.cpp').read_text().split('int main(){')[0]+r'''
int main(){using Decoder=core::images::PreviewDecoder;using Result=Decoder::Result;
std::vector<uint16_t> pixels(Decoder::kPixels+16,0xabcd);
std::vector<uint8_t> jpeg={0xff,0xd8,0xff,0xc0,0,17,8,1,224,2,128,3,1,0x21,0,2,0x11,1,3,0x11,1,
 0xff,0xda,0,12,3,1,0,2,0x11,3,0x11,0,63,0};
{Decoder decoder;assert(decoder.prepareNative());
for(int width:{640,1280}){
 sourceWidth=width;sourceHeight=width==640?480:720;
 jpeg[7]=sourceHeight>>8;jpeg[8]=sourceHeight;jpeg[9]=width>>8;jpeg[10]=width;
 assert(decoder.decode(jpeg.data(),jpeg.size(),pixels.data()+8,nullptr,nullptr,true)==Result::Decoded);
 const int top=width==1280?41:15,sh=width==1280?180:240,dh=width==1280?157:210;
 for(int x=0;x<280;++x)for(int y=0;y<240;++y)
  assert(pixels[8+(279-x)*240+y]==(y>=top&&y<top+dh?pixel(x*320/280,(y-top)*sh/dh):0));
 for(int i=0;i<8;++i)assert(pixels[i]==0xabcd&&pixels[8+Decoder::kPixels+i]==0xabcd);
 int calls=0;assert(decoder.decode(jpeg.data(),jpeg.size(),pixels.data()+8,abortAfterThree,&calls,true)==Result::Interrupted);
 const auto before=pixels;jpeg[13]=0x12;
 assert(decoder.decode(jpeg.data(),jpeg.size(),pixels.data()+8,nullptr,nullptr,true)==Result::Invalid&&pixels==before);
 jpeg[13]=0x21;
}
decoder.close();assert(!decoder.ready());failInternal=failPsram=true;
assert(!decoder.prepareNative());failInternal=failPsram=false;
assert(decoder.prepareNative(true));sourceWidth=640;sourceHeight=480;
jpeg[7]=1;jpeg[8]=224;jpeg[9]=2;jpeg[10]=128;
assert(decoder.decode(jpeg.data(),jpeg.size(),pixels.data()+8)==Result::Decoded);
for(int x=0;x<280;++x)for(int y=0;y<240;++y)assert(pixels[8+(279-x)*240+y]==pixel(x+20,y));
}assert(allocations==frees&&constructs==destructs);
}
'''
    write('trial.cpp',trial)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-DORIGAMI_REDUCED_VIDEO_DECODE_TRIAL','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/core/preview_decoder.cpp'),str(root/'trial.cpp'),'-o',str(root/'trial')],check=True)
    subprocess.run([str(root/'trial')],check=True)
print('PASS camera MCU crop/transpose and native video row fitting, output boundaries, cancellation, mode switching and cleanup under sanitizers (mocked codecs)')
