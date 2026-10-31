"""Compare real UI composition to the pre-extraction renderer, with a fake panel/decoder."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
# Frozen from 18d9d5ea; expected output keeps the pre-extraction composition.
REFERENCE = r'''
struct CameraCanvas {
 uint16_t* pixels;
 uint16_t at(int x,int y)const {uint16_t p=pixels[(279-x)*240+y];return (p>>8)|(p<<8);}
 uint16_t readPixel(int x,int y)const{return x>=0&&x<280&&y>=0&&y<240?at(x,y):0;}
 void fillRect(int x,int y,int w,int h,uint16_t ink){
  uint16_t p=(ink>>8)|(ink<<8);
  for(int xx=x;xx<x+w;xx++)for(int yy=y;yy<y+h;yy++)
   if(xx>=0&&xx<280&&yy>=0&&yy<240)pixels[(279-xx)*240+yy]=p;
 }
};
static void legacyPresent() {
 if(!s_cameraFrameReady||!s_cameraComposite)return;
 memcpy(s_cameraComposite,s_previewFit,280u*240u*2);
 CameraCanvas canvas{s_cameraComposite};
 const uint32_t now=millis();
 const unsigned age=std::min(999u,(unsigned)(now-s_cameraStageAt));
 camera_photographer::overlays(canvas,s_cameraStage*1000+age);
 if(s_cameraPulseAt && now-s_cameraPulseAt<140u){
  unsigned t=now-s_cameraPulseAt;
  int alpha=t<35?15:(140-t)*15/105;
  for(int i=0;i<280*240;i++){
   uint16_t p=s_cameraComposite[i];p=(p>>8)|(p<<8);
   p=music_kami::blend(0xFFFF,p,alpha);
   s_cameraComposite[i]=(p>>8)|(p<<8);
  }
 }
}
'''
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('core/preview_decoder.h',r'''#pragma once
#include <stdint.h>
#include <stddef.h>
namespace core {namespace images {
class PreviewDecoder {bool ready_=false;public:
enum class Result {Decoded,Interrupted,Invalid};using ShouldAbort=bool(*)(void*);
bool prepare();bool prepareNative(bool=false);void close();bool ready()const{return ready_;}
Result decode(const uint8_t*,size_t,uint16_t*,ShouldAbort,void*,bool=false);
};}}
''')
    write('hal/hal_display.h',r'''#pragma once
#include <stdint.h>
struct Panel {
bool swap=true;uint8_t rotation=3;
bool getSwapBytes(){return swap;}uint8_t getRotation(){return rotation;}
void setSwapBytes(bool s){swap=s;}void setRotation(uint8_t r){rotation=r;}
int width(){return 280;}int height(){return 240;}
void setViewport(int,int,int,int,bool);void resetViewport();
void writecommand(uint8_t);void pushImage(int,int,int,int,uint16_t*);
void pushImageOutput(int,int,int,int,uint16_t*);
};
namespace hal {namespace display {Panel& directTft();void beginDirectDraw();void endDirectDraw();bool waitForVSync();void markAllDirty();uint16_t* prepareNativeOutput(uint16_t*);}}
''')
    write('JPEGDecoder.h',r'''#pragma once
#include <stdint.h>
#include <stddef.h>
struct Decoder {unsigned MCUWidth=16,MCUHeight=16,MCUx=0,MCUy=0;uint16_t* pImage=nullptr;bool decodeArray(uint8_t*,size_t);bool readSwappedBytes();};
extern Decoder JpegDec;
''')
    write('esp_heap_caps.h','#include <stddef.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\nvoid* heap_caps_malloc(size_t,int);void heap_caps_free(void*);void* heap_caps_aligned_calloc(size_t,size_t,size_t,int);')
    write('pgmspace.h', '#pragma once\n#define PROGMEM\n#define pgm_read_byte(address) (*(const unsigned char*)(address))\n')
    write('esp_timer.h','#include <stdint.h>\nint64_t esp_timer_get_time();')
    write('main.cpp',r'''
#include "ui/camera_preview.h"
#include "ui/camera_photographer_view.h"
#include "ui/music_kami_view.h"
#include "hal/hal_display.h"
#include "JPEGDecoder.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <set>
#ifdef ORIGAMI_INSTRUMENT_FONT
#include <Fonts/Instrument/data.inc>
#endif
bool outputPrepared=false,syncSeen=false;
bool fallback=false,viewport=false,decodeOk=true;int dirty=0,block=0;
std::vector<int> positions;
uint16_t mcu[256]={1,2,3};Decoder JpegDec;
bool Decoder::decodeArray(uint8_t*,size_t){block=0;pImage=mcu;return decodeOk;}
bool Decoder::readSwappedBytes(){const unsigned xs[]={0,17,18,0,0};const unsigned ys[]={0,0,0,14,15};if(block==5)return false;MCUx=xs[block];MCUy=ys[block++];return true;}
Panel panel;bool nativePrepared=false;bool drawing=false,syncOk=true,prepareOk=true,nativeOk=true;int legacyPrepares=0;int pushes=0,call=0,failAt=0;
uint32_t clockMs=1000;uint32_t millis(){return clockMs;}
int64_t clockUs=0;int64_t esp_timer_get_time(){clockUs+=17;return clockUs;}
std::set<void*> owned;
void* heap_caps_malloc(size_t n,int caps){assert(caps==3&&n==280*240*2);if(++call==failAt)return nullptr;auto*p=malloc(n);owned.insert(p);return p;}
void* heap_caps_aligned_calloc(size_t align,size_t count,size_t n,int caps){assert(align==16&&count==1);return heap_caps_malloc(n,caps);}
void heap_caps_free(void*p){if(p){assert(owned.erase(p)==1);free(p);}}
std::vector<uint16_t> source(280*240),captured;
namespace core {namespace images {
bool PreviewDecoder::prepare(){++legacyPrepares;ready_=prepareOk;return ready_;}
bool nativeCameraCrop=false;
bool PreviewDecoder::prepareNative(bool cameraCrop){nativeCameraCrop=cameraCrop;ready_=prepareOk&&nativeOk;return ready_;}
void PreviewDecoder::close(){ready_=false;}
PreviewDecoder::Result PreviewDecoder::decode(const uint8_t*,size_t,uint16_t*out,ShouldAbort abort,void*user,bool fit){if(!decodeOk){out[0]=0xdead;return Result::Invalid;}if(abort&&abort(user)){out[0]=0xbeef;return Result::Interrupted;}std::copy(source.begin(),source.end(),out);if(fit)for(int x=0;x<280;++x){std::fill(out+x*240,out+x*240+15,0);std::fill(out+x*240+225,out+x*240+240,0);}return Result::Decoded;}
}}
void Panel::writecommand(uint8_t cmd){assert(drawing&&cmd==0x21);}
void Panel::setViewport(int x,int y,int w,int h,bool datum){assert(fallback&&!viewport&&!datum&&x==0&&y==0&&w==280&&h==212);viewport=true;}
void Panel::resetViewport(){assert(viewport);viewport=false;}
void Panel::pushImage(int x,int y,int w,int h,uint16_t*p){if(fallback){assert(viewport&&!drawing&&w==16&&h==16&&p==mcu);positions.push_back(x);positions.push_back(y);return;}assert(drawing&&!swap&&rotation==0&&x==0&&y==0&&w==240&&h==280);captured.assign(p,p+w*h);++pushes;}
void Panel::pushImageOutput(int x,int y,int w,int h,uint16_t*p){assert(outputPrepared&&syncSeen);pushImage(x,y,w,h,p);}
namespace hal {namespace display {
uint16_t* prepareNativeOutput(uint16_t* pixels){assert(!drawing&&!nativePrepared);nativePrepared=true;syncSeen=false;outputPrepared=syncOk;return outputPrepared?pixels:nullptr;}
void markAllDirty(){assert(!viewport);++dirty;}
Panel& directTft(){return panel;}
void beginDirectDraw(){assert(!drawing&&nativePrepared);drawing=true;syncSeen=false;}
void endDirectDraw(){assert(drawing&&nativePrepared);drawing=false;nativePrepared=false;}
bool waitForVSync(){assert(drawing);syncSeen=true;return syncOk;}
}}
std::vector<uint16_t> expected(280*240);
uint16_t* s_previewFit=source.data();uint16_t* s_cameraComposite=expected.data();
bool s_cameraFrameReady=true;unsigned s_cameraStage=0;uint32_t s_cameraStageAt=0,s_cameraPulseAt=0;
'''+REFERENCE+r'''
int main(){
 for(size_t i=0;i<source.size();++i)source[i]=(i*137+59)&0xffff;
 for(int fail=1;fail<=2;++fail){call=0;failAt=fail;ui::CameraPreview p;assert(!p.prepareSurfaces()&&!p.hasSurfaces());assert(owned.empty());}
 failAt=0;
 {ui::CameraPreview p;assert(!p.hasFrame());assert(!p.copyDecoded(source.data()));p.present(0);assert(pushes==0);
 assert(p.prepareDecoder()&&!p.ready());assert(p.decode(nullptr,0)==ui::CameraPreview::Result::Invalid);
 assert(p.prepareSurfaces()&&!p.ready()&&!p.hasFrame());
 prepareOk=false;assert(!p.prepareDecoder());prepareOk=true;assert(p.prepareDecoder());
 int beforeLegacy=legacyPrepares;
 assert(p.prepareDecoder()&&legacyPrepares==beforeLegacy);
 nativeOk=false;assert(p.prepareDecoder()&&legacyPrepares==beforeLegacy+1);
 assert(!p.prepareDecoder(true)&&legacyPrepares==beforeLegacy+1);
 nativeOk=true;assert(p.prepareDecoder(true)&&legacyPrepares==beforeLegacy+1);
 assert(p.prepareDecoder(true,false,true)&&legacyPrepares==beforeLegacy+2);
 assert(p.prepareDecoder(true));
 assert(!core::images::nativeCameraCrop);
 assert(p.prepareDecoder(false,true)&&core::images::nativeCameraCrop);
 assert(p.prepareDecoder()&&!core::images::nativeCameraCrop);
 assert(p.prepareDecoder(true,true)&&!core::images::nativeCameraCrop);
 assert(p.decode(nullptr,0)==ui::CameraPreview::Result::Decoded&&p.hasFrame());
 assert(!p.copyDecoded(nullptr));auto decodedCopy=source;assert(p.copyDecoded(decodedCopy.data()));
 std::fill(decodedCopy.begin(),decodedCopy.end(),0); // Released worker storage may be reused immediately.
 for(unsigned stage=0;stage<=6;++stage)for(unsigned age:{0u,34u,35u,139u,140u,999u})for(int pulse:{-1,0,34,35,139,140}){
   s_cameraStage=stage;s_cameraStageAt=clockMs-age;s_cameraPulseAt=pulse<0?0:clockMs-pulse;
   legacyPresent();syncOk=!syncOk;auto r=p.present(stage*1000+age,pulse);
   assert(captured==expected);assert(r.syncMissed==!syncOk&&r.syncWaitUs==17);
   assert(panel.rotation==3&&panel.swap&&!drawing);
 }
 // Compare actual alpha text against its actual background, including a long
 // status clipped to the existing rail. The white-background fallback fails this.
 for(const char* status:{"B: PHOTO  C: SETTINGS","720p + MIC  B: REC","VGA SILENT  B: REC","REC 00:15  B: STOP","REC + MIC  30s MAX  B: STOP","This status is deliberately too long for its rail"}){
  s_cameraStage=0;s_cameraStageAt=clockMs;s_cameraPulseAt=0;legacyPresent();
  CameraCanvas canvas{expected.data()};canvas.fillRect(52,196,180,20,0);
  char caption[29];unsigned columns=28;
  do {ui::pixel_text::shorten(status,caption,columns);
#ifdef ORIGAMI_INSTRUMENT_FONT
   int width=ui::instrument_text::width(caption,ui::instrument_text::cell(1));
#else
   int width=int(strlen(caption))*6;
#endif
   if(width<=172||columns==3)break;--columns;
  }while(true);
  ui::pixel_text::text(canvas,caption,56,202,1,0xF800);
  p.present(0,-1,status,true);assert(captured==expected);
 }
 // Fitted live-camera input still uses the camera compositor and its overlays.
 // Build the expected bars independently of the new fit forwarding argument.
 assert(p.decode(nullptr,0,nullptr,nullptr,true)==ui::CameraPreview::Result::Decoded);
 const auto originalSource=source;
 for(int x=0;x<280;++x){std::fill(source.begin()+x*240,source.begin()+x*240+15,0);std::fill(source.begin()+x*240+225,source.begin()+x*240+240,0);}
 s_cameraStage=0;s_cameraStageAt=clockMs;s_cameraPulseAt=0;legacyPresent();
 p.present(0);assert(captured==expected);source=originalSource;
 // A codec may overwrite part of the frame before failing or acknowledging
 // cancellation. A later animation/status redraw must not publish those pixels.
 for(bool interrupted:{false,true}){
  assert(p.decode(nullptr,0)==ui::CameraPreview::Result::Decoded&&p.hasFrame());
  p.present(0);const auto lastGood=captured;const int previousPushes=pushes;
  decodeOk=interrupted;bool cancel=true;
  auto result=p.decode(nullptr,0,interrupted?+[](void*u){return *(bool*)u;}:nullptr,&cancel);
  assert(result==(interrupted?ui::CameraPreview::Result::Interrupted:ui::CameraPreview::Result::Invalid));
  assert(!p.hasFrame());p.present(1000,-1,"REC + MIC",true);
  assert(pushes==previousPushes&&captured==lastGood);
  decodeOk=true;assert(p.decode(nullptr,0)==ui::CameraPreview::Result::Decoded&&p.hasFrame());
  p.present(0);assert(pushes==previousPushes+1);
 }
 assert(p.decodeVideo(nullptr,0)==ui::CameraPreview::Result::Decoded);
 // Reference starts from a fresh image for each overlay. Production retains
 // the same image while labels shorten, progress rewinds and controls change.
 for(const char* title:{"Video gjpqy 0123456789","V",""})
 for(const char* controls:{"B:PAUSE 00:03/00:08","B:PLAY",""})
 for(unsigned progress:{1000u,500u,0u,1500u}){
  expected=source;CameraCanvas videoCanvas{expected.data()};
  videoCanvas.fillRect(0,0,280,15,0);videoCanvas.fillRect(0,225,280,15,0);
  ui::pixel_text::text(videoCanvas,title,20,4,1,0xFFFF);
  ui::pixel_text::text(videoCanvas,controls,20,228,1,0xFFFF);
  videoCanvas.fillRect(20,222,240,2,0x4208);
  videoCanvas.fillRect(20,222,int(240*std::min(progress,1000u)/1000),2,0xFFFF);
  p.presentVideo(title,controls,progress);assert(captured==expected);
  auto videoCopy=source;assert(p.copyDecoded(videoCopy.data(),true));
  std::fill(videoCopy.begin(),videoCopy.end(),0);
  p.presentVideo(title,controls,progress);assert(captured==expected);
 }
 decodeOk=false;assert(p.decodeVideo(nullptr,0)==ui::CameraPreview::Result::Invalid&&!p.hasFrame());
 int beforePush=pushes;p.presentVideo("Bad","",0);assert(pushes==beforePush);
 decodeOk=true;assert(p.decodeVideo(nullptr,0)==ui::CameraPreview::Result::Decoded&&p.hasFrame());
 assert(p.decode(nullptr,0)==ui::CameraPreview::Result::Decoded);

 // Real tutorial composition: the instruction must survive every capture stage
 // and every shutter pulse. Check the exact native transfer surface, not a mock UI.
 p.present(0,-1,nullptr,false,true);auto baseline=captured;
 for(unsigned stage=0;stage<=6;++stage)for(unsigned age:{0u,35u,140u,999u})for(int pulse:{-1,0,35,139}){
  p.present(stage*1000+age,pulse,nullptr,false,true);
  for(int x=24;x<256;++x)for(int y=194;y<224;++y)assert(captured[(279-x)*240+y]==baseline[(279-x)*240+y]);
 }
 for(size_t i=0;i<source.size();++i)assert(source[i]==((i*137+59)&0xffff));
 assert(p.decode(nullptr,size_t(2147483648ULL))==ui::CameraPreview::Result::Invalid);
 bool abort=true;assert(p.decode(nullptr,0,[](void*u){return *(bool*)u;},&abort)==ui::CameraPreview::Result::Interrupted);
 p.closeDecoder();assert(!p.ready()&&p.hasSurfaces());assert(p.prepareDecoder());
 p.close();p.close();assert(!p.hasSurfaces()&&!p.hasFrame()&&owned.empty());
 assert(p.prepareSurfaces());assert(p.prepareDecoder());
 }assert(owned.empty());
 {ui::CameraPreview p;assert(p.prepareSurfaces());
 std::vector<uint8_t> raw(320*240*2+2,0x5a);
 for(int y=0;y<240;++y)for(int x=0;x<320;++x){
  unsigned value=(x*137+y*59)&65535;raw[1+(y*320+x)*2]=value>>8;raw[2+(y*320+x)*2]=value;
 }
 const auto originalRaw=raw;
 assert(p.copyRgb565(raw.data()+1,320*240*2)==ui::CameraPreview::Result::Decoded);
 expected.resize(280*240);
 for(int x=0;x<280;++x)for(int y=0;y<240;++y){unsigned v=((x+20)*137+y*59)&65535;expected[(279-x)*240+y]=uint16_t((v>>8)|(v<<8));}
 CameraCanvas reference{expected.data()};camera_photographer::overlays(reference,0);
 p.present(0);assert(captured==expected&&raw==originalRaw);
 for(size_t length:{size_t(0),size_t(320*240*2-1),size_t(320*240*2+1)}){
  int before=pushes;assert(p.copyRgb565(raw.data()+1,length)==ui::CameraPreview::Result::Invalid);
  p.present(0);assert(pushes==before&&!p.hasFrame());
 }
 int calls=0,before=pushes;
 assert(p.copyRgb565(raw.data()+1,320*240*2,[](void*u){return ++*static_cast<int*>(u)==3;},&calls)==ui::CameraPreview::Result::Interrupted);
 p.present(0);assert(pushes==before&&!p.hasFrame());
 assert(p.copyRgb565(raw.data()+1,320*240*2)==ui::CameraPreview::Result::Decoded);
 p.present(0);assert(captured==expected&&raw==originalRaw);p.close();
 assert(p.copyRgb565(raw.data()+1,320*240*2)==ui::CameraPreview::Result::Invalid);
 }assert(owned.empty());
 {ui::CameraPreview p;fallback=true;decodeOk=false;
 assert(!p.presentFallback(nullptr,0)&&!viewport&&dirty==0&&positions.empty());
 decodeOk=true;assert(p.presentFallback(nullptr,0));
 assert((positions==std::vector<int>{0,0,272,0,0,224}));
 assert(dirty==1&&!viewport&&owned.empty()&&panel.rotation==3&&panel.swap);
 }
}
''')
    for instrument in [False, True]:
        flags=['-DORIGAMI_INSTRUMENT_FONT'] if instrument else []
        subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',*flags,
            '-I'+str(root),'-I'+str(ROOT/'tools/mp3-preview/host'),'-I'+str(ROOT/'include'),
            '-I'+str(ROOT/'lib/TB_TFT_eSPI'),str(ROOT/'src/ui/camera_preview.cpp'),
            str(ROOT/'src/ui/native_frame.cpp'),str(root/'main.cpp'),'-o',str(root/'test')],check=True)
        subprocess.run([str(root/'test')],check=True)
print('PASS pixel/Instrument camera and video composition, actual-background alpha, bounded status, panel state, allocation rollback and cleanup under sanitizers')
