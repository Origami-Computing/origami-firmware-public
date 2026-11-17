from pathlib import Path
import tempfile,subprocess
R=Path(__file__).resolve().parents[1];s=(R/'src/services/cine_camera_resize.cpp').read_text();callbacks=s[s.index('struct Decode {'):s.index('} // namespace\nvoid Session::releaseIspPreview()')];methods=s[s.index('void Session::releaseIspPreview()'):s.index('} // namespace cine_camera')]
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.cpp').write_text(r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <cassert>
#include <cstdlib>
#include <new>
#include <set>
#include "core/cine_frame.h"
#define MAX_BUFFERED_PIXELS 2048
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define RGB565_LITTLE_ENDIAN 0
#define JPEG_SCALE_EIGHTH 8
#define JPEG_SCALE_HALF 2
#define JPEG_SCALE_QUARTER 4
namespace core{namespace watchdog{void feed(){}}}
namespace hal{namespace camera{struct CameraFrame{const uint8_t* buf;size_t len;unsigned width,height;};}}
std::set<void*> owned;int budget=-1,behavior=0;unsigned opens=0;
void* heap_caps_malloc(size_t n,int caps){assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));if(budget==0)return nullptr;if(budget>0)--budget;auto* p=malloc(n);if(p)assert(owned.insert(p).second);return p;}
void* heap_caps_aligned_alloc(size_t alignment,size_t n,int caps){assert(alignment==16);return heap_caps_malloc(n,caps);}
struct jpeg_dec_config_t {int output_type;struct {int width,height;} scale,clipper;};
struct jpeg_dec_io_t {uint8_t* inbuf;int inbuf_len;uint8_t* outbuf;int out_size;};
struct jpeg_dec_header_info_t {int width,height;};
constexpr int JPEG_ERR_OK=0,JPEG_PIXEL_FORMAT_RGB565_LE=1;
int nativeCloses=0;
int jpeg_dec_open(jpeg_dec_config_t* c,void** d){assert(c->clipper.width==256);*d=c;return 0;}
void jpeg_dec_close(void*){++nativeCloses;}
int jpeg_dec_parse_header(void*,jpeg_dec_io_t*,jpeg_dec_header_info_t* h){h->width=256;h->height=192;return 0;}
int jpeg_dec_get_outbuf_len(void*,int* len){*len=256*192*2;return 0;}
int jpeg_dec_process(void*,jpeg_dec_io_t* io){io->out_size=256*192*2;memset(io->outbuf,0x45,io->out_size);return 0;}
void heap_caps_free(void* p){if(p)assert(owned.erase(p)==1);free(p);}
struct JPEGDRAW{void* pUser;int x,y,iWidthUsed,iHeight,iWidth;uint16_t* pPixels;};
class JPEGDEC{void* user=nullptr;int(*callback)(JPEGDRAW*)=nullptr;public:
 int openRAM(uint8_t*,int,int(*cb)(JPEGDRAW*)){++opens;callback=cb;return behavior!=1;}
 int getWidth(){return behavior==2?640:2048;}int getHeight(){return 1536;}
 void setUserPointer(void* p){user=p;}void setPixelType(int){}
 int decode(int,int,int){uint16_t pixels[2048];for(auto& v:pixels)v=0x1234;for(int y=0;y<192;y+=8){JPEGDRAW b{user,0,y,256,8,256,pixels};if(!callback(&b))return 1;if(behavior==3)return 0;}return 1;}
 void close(){}
};
class Session{public:static constexpr unsigned previewWidth=256,previewHeight=192;uint16_t *preview_=nullptr,*previewWork_=nullptr;JPEGDEC* ispDecoder_=nullptr;void* videoPreviewDecoder_=nullptr;bool triedVideoDecoder_=false;struct {bool previewInterrupted=false;} status_;void releaseIspPreview();bool renderIspPreview(const hal::camera::CameraFrame&,bool(*)(void*)=nullptr,void* =nullptr);};
'''+callbacks+methods+r'''
int main(){uint8_t jpeg[]={255,216,255,217};hal::camera::CameraFrame f{jpeg,4,2048,1536};Session s;s.preview_=static_cast<uint16_t*>(heap_caps_malloc(256*192*2,3));for(unsigned i=0;i<256*192;++i)s.preview_[i]=0xbeef;
 budget=0;assert(!s.renderIspPreview(f)&&owned.size()==1);budget=1;assert(!s.renderIspPreview(f)&&owned.size()==2);budget=-1;
 for(int b:{1,2,3}){behavior=b;auto* front=s.preview_;assert(!s.renderIspPreview(f)&&s.preview_==front);for(unsigned i=0;i<256*192;++i)assert(front[i]==0xbeef);assert(owned.size()==3);}
 behavior=0;{auto* front=s.preview_;unsigned calls=0;auto abort=[](void* p){return ++*static_cast<unsigned*>(p)>=2;};assert(!s.renderIspPreview(f,abort,&calls)&&s.status_.previewInterrupted&&s.preview_==front);for(unsigned i=0;i<256*192;++i)assert(front[i]==0xbeef);assert(owned.size()==3);}
 behavior=0;assert(s.renderIspPreview(f)&&owned.size()==3);for(unsigned i=0;i<256*192;++i)assert(s.preview_[i]==0x1234);
 for(int i=0;i<20;++i){assert(s.renderIspPreview(f));assert(owned.size()==3);}
 auto count=opens;f.buf=nullptr;assert(!s.renderIspPreview(f)&&opens==count);f.buf=jpeg;f.len=0;assert(!s.renderIspPreview(f));assert(!core::cine::validJpeg(jpeg,4,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX));
 f={jpeg,4,320,240};assert(s.renderIspPreview(f));assert(s.preview_[0]==0x4545);
 s.releaseIspPreview();s.releaseIspPreview();assert(nativeCloses==1);assert(owned.size()==1);heap_caps_free(s.preview_);assert(owned.empty());
}
''');subprocess.run(['c++','-std=c++11','-O1','-fsanitize=address,undefined','-I'+str(R/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
print('PASS actual preview lifecycle under ASan/UBSan: PSRAM-only persistent decoder, OOM, open/dimension/partial-decode failures, atomic frame swap, malformed frames, repeated reuse and idempotent cleanup; codec mocked.')
