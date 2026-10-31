"""Run the production reduced preview adapter with the pinned real JPEGDEC codec.

Requires the normal target's installed JPEGDEC dependency, c++, FFmpeg and cjpeg.
Synthetic quadrant JPEGs check colors, orientation, HD tail rows and letterboxing.
Host CPU codec execution is not ESP32 SIMD/performance acceptance.
"""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
LIB=ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/JPEGDEC/src'
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)
 (p/'Arduino.h').write_text('#pragma once\nstruct SerialStub{template<class...T>void printf(const char*,T...) {}};extern SerialStub Serial;\n')
 (p/'esp_heap_caps.h').write_text('''#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_INTERNAL=1,MALLOC_CAP_SPIRAM=2,MALLOC_CAP_8BIT=4;
inline void* heap_caps_malloc(size_t n,int){return malloc(n);}
inline void* heap_caps_aligned_calloc(size_t,size_t n,size_t s,int){return calloc(n,s);}
inline void heap_caps_free(void*p){free(p);}
''')
 (p/'esp_jpeg_dec.h').write_text('''#pragma once
#include <cstdint>
constexpr int JPEG_PIXEL_FORMAT_RGB565_BE=0,JPEG_ROTATE_0D=0,JPEG_ROTATE_270D=270,JPEG_ERR_OK=0;
struct Resolution{int width,height;};
struct jpeg_dec_config_t{int output_type;Resolution scale,clipper;int rotate;bool block_enable;};
struct jpeg_dec_io_t{uint8_t*inbuf;int inbuf_len;uint8_t*outbuf;int out_size;};
struct jpeg_dec_header_info_t{int width,height;};
inline int jpeg_dec_open(jpeg_dec_config_t*,void**){return -1;}
inline int jpeg_dec_close(void*){return 0;}
inline int jpeg_dec_parse_header(void*,jpeg_dec_io_t*,jpeg_dec_header_info_t*){return -1;}
inline int jpeg_dec_get_outbuf_len(void*,int*){return -1;}
inline int jpeg_dec_get_process_count(void*,int*){return -1;}
inline int jpeg_dec_process(void*,jpeg_dec_io_t*){return -1;}
''')
 (p/'main.cpp').write_text('''#include "core/preview_decoder.h"
#include <Arduino.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <cassert>
#include <JPEGDEC.h>
#include <cstring>
SerialStub Serial;
struct Full {int w,h;uint16_t* pixels;};
int drawFull(JPEGDRAW* b){auto& f=*static_cast<Full*>(b->pUser);
 assert(b->x>=0&&b->y>=0&&b->x+b->iWidthUsed<=f.w&&b->y+b->iHeight<=f.h);
 for(int y=0;y<b->iHeight;++y)memcpy(f.pixels+(b->y+y)*f.w+b->x,b->pPixels+y*b->iWidth,b->iWidthUsed*2);
 return 1;
}
int main(int argc,char**argv){assert(argc==3||argc==4);
 std::ifstream in(argv[1],std::ios::binary);std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(in)),{});
 const auto original=jpeg;
 if(argc==4){JPEGDEC full;assert(full.openRAM(jpeg.data(),jpeg.size(),drawFull));
  Full f{full.getWidth(),full.getHeight(),nullptr};std::vector<uint16_t> raw(f.w*f.h+16,0xabcd);f.pixels=raw.data()+8;
  full.setUserPointer(&f);full.setPixelType(RGB565_BIG_ENDIAN);assert(full.decode(0,0,0));full.close();
  assert(jpeg==original);for(int i=0;i<8;++i)assert(raw[i]==0xabcd&&raw[8+f.w*f.h+i]==0xabcd);
  std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<char*>(f.pixels),f.w*f.h*2);return 0;
 }
 using D=core::images::PreviewDecoder;D decoder;
 assert(decoder.prepareNative());std::vector<uint16_t> pixels(D::kPixels+16,0xabcd);
 assert(decoder.decode(jpeg.data(),jpeg.size(),pixels.data()+8,nullptr,nullptr,true)==D::Result::Decoded);
 assert(jpeg==original);for(int i=0;i<8;++i)assert(pixels[i]==0xabcd&&pixels[8+D::kPixels+i]==0xabcd);
 std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<char*>(pixels.data()+8),D::kPixels*2);
}''')
 subprocess.run(['c++','-std=c++17','-fsanitize=address','-DORIGAMI_REDUCED_VIDEO_DECODE_TRIAL','-I'+str(p),'-I'+str(ROOT/'include'),'-I'+str(LIB),str(ROOT/'src/core/preview_decoder.cpp'),str(LIB/'JPEGDEC.cpp'),str(p/'main.cpp'),'-o',str(p/'decode')],check=True)
 colors=[(255,0,0),(0,255,0),(0,0,255),(255,255,255)]
 for w,h,sampling in [(w,h,s) for w,h in [(320,240),(640,480),(1280,720)] for s in ('yuvj420p','yuvj422p')]:
  data=b''.join(bytes(max(0,c-(x*y%17)) for c in colors[(y>=h//2)*2+(x>=w//2)]) for y in range(h) for x in range(w))
  source=p/'source.ppm';source.write_bytes(f'P6\n{w} {h}\n255\n'.encode()+data)
  jpeg=p/'source.jpg';output=p/'pixels.bin'
  if sampling=='yuvj422p':
   # FFmpeg emits non-unit chroma factors in its 422 JPEGs. The production
   # preflight intentionally rejects those; use a true horizontal 2x1 MCU.
   subprocess.run(['cjpeg','-quality','95','-sample','2x1,1x1,1x1','-outfile',str(jpeg),str(source)],check=True)
  else:
   subprocess.run(['ffmpeg','-nostdin','-v','error','-y','-i',str(source),'-pix_fmt',sampling,'-q:v','2','-threads','1',str(jpeg)],check=True)
  subprocess.run([str(p/'decode'),str(jpeg),str(output)],check=True)
  raw=output.read_bytes();assert len(raw)==280*240*2
  subprocess.run([str(p/'decode'),str(jpeg),str(p/'full.bin'),'full'],check=True)
  full=(p/'full.bin').read_bytes();assert len(full)==w*h*2
  for i,(x,y) in enumerate([(w//4,h//4),(3*w//4,h//4),(w//4,3*h//4),(3*w//4,3*h//4)]):
   v=int.from_bytes(full[(y*w+x)*2:(y*w+x)*2+2],'big')
   rgb=((v>>11)*255//31,((v>>5)&63)*255//63,(v&31)*255//31)
   assert max(abs(a-b) for a,b in zip(rgb,colors[i]))<35
  height=280*h//w;top=(240-height)//2
  def pixel(x,y):
   at=((279-x)*240+y)*2;v=int.from_bytes(raw[at:at+2],'big')
   return ((v>>11)*255//31,((v>>5)&63)*255//63,(v&31)*255//31)
  for x in range(280):
   for y in list(range(top))+list(range(top+height,240)):assert pixel(x,y)==(0,0,0)
  for i,(x,y) in enumerate([(70,top+height//4),(210,top+height//4),(70,top+3*height//4),(210,top+3*height//4)]):
   actual=pixel(x,y);assert max(abs(a-b) for a,b in zip(actual,colors[i]))<35,(w,i,actual)
  # Last displayed row must contain the bottom quadrants, not a stale MCU band.
  assert pixel(70,top+height-1)[2]>220 and min(pixel(210,top+height-1))>220
print('PASS pinned JPEGDEC QVGA/VGA/HD 420/422 reduced preview and full RGB565, colors, rotation, letterbox, immutable input and guarded output (host CPU)')
