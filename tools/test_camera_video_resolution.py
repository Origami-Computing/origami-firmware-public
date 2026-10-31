"""Exercise production video-start ordering with failed sensor reconfiguration."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/apps/camera_menu_app.cpp').read_text()
start=source.index('static bool camBeginVideo() {')
end=source.index('\n#ifdef ORIGAMI_DEV_SERIAL_INPUT',start)
entry=source[start:end]
def function(signature):
 a=source.index(signature);b=source.index('{',a)+1;depth=1
 while depth:
  depth+=(source[b]=='{')-(source[b]=='}');b+=1
 return source[a:b]
selector=function('static int sPreviewFrameSize()')
default='\n'.join(source[source.index('static constexpr int '+name):].split(';',1)[0]+';' for name in ['kDefaultShootMode','kVideoJpegQuality','kHdVideoJpegQuality'])

start=source.index('static void camPreparePreviewBuffers() {')
end=source.index('\n\n\n// Allocate the synchronous',start)
prepare=source[start:end]
start=source.index('      s_videoNoticeMs = millis();',source.index('static void camTick()'))
end=source.index('\n    uint32_t videoLimit',start)
# Compile the terminal handoff statements from the actual tick branch.
finish='void finishVideo() {\n'+source[start:end].rsplit('}',1)[0]+'\n}'
code=r'''#include <cassert>
#include <cstdint>
#include <string>
std::string trace;
bool s_continuousVga=false,s_videoPending=false,s_videoStopping=false,s_reviewActive=false;
bool restartOk=true,beginOk=true,busy=false;
unsigned s_cameraStage=0,s_videoStartedMs=0,s_videoPreviewMs=0;
int s_shootMode=2,s_videoPerf=0,s_camera=0;
unsigned s_videoNoticeMs=0;bool finishing=false;
struct {int capture=0;}s_settings;
const int CR_VGA=0;
static bool camVideoHd();
bool surfaces=true;
struct {bool prepareSurfaces(){return surfaces;}}s_cameraPreview;
struct {void println(const char*){}}Serial;
void camReleasePreviewBuffers(){s_continuousVga=false;}
unsigned millis(){return 123;}
void camDrawReviewStatus(const char*){}
void camFinishQuickVideo(){}
bool camRestorePreviewAfterCapture(unsigned ms){
 assert(ms==700&&s_continuousVga==!finishing&&!s_videoPending&&!busy);
 trace+="restart;";if(!restartOk)s_continuousVga=false;return restartOk;
}
struct NavHandler{static NavHandler& instance(){static NavHandler n;return n;}
 void runtimePolicyChanged(){assert(s_videoPending==!finishing);trace+="policy;";}};
struct Video{bool busy(){return ::busy;}
 void close(){trace+="close;";::busy=false;}
 bool beginCamera(int&,unsigned w,unsigned h,bool audio,bool preview=true){
 assert(w==(s_settings.capture?1280u:640u)&&h==(s_settings.capture?720u:480u)&&s_videoPending);
#ifdef ORIGAMI_VIDEO_CAPTURE_ONLY_TRIAL
 assert(!preview);
#else
 assert(preview);
#endif
 trace+=audio?"audio;":"silent;";return beginOk;}
}s_video;
'''+function('static bool camVideoHd() {')+default+'\nconst int FRAMESIZE_HD=2,FRAMESIZE_VGA=1,FRAMESIZE_QVGA=0;\n'+selector+function('static int camVideoQuality()')+prepare+entry+finish+r'''
int main(){
 assert(kDefaultShootMode==0);
 for(int shoot:{0,1,2})for(int capture:{0,1,2})for(bool memory:{false,true}){
  s_shootMode=shoot;s_settings.capture=capture;surfaces=memory;
  camPreparePreviewBuffers();
  assert(s_continuousVga==(memory&&shoot==0&&capture==CR_VGA));
 }

 for(int capture:{0,1,2})for(int shoot:{1,2})for(bool ready:{false,true})for(bool started:{false,true}){
  s_settings.capture=capture;s_shootMode=shoot;s_continuousVga=ready;s_videoPending=false;trace.clear();beginOk=started;
  assert(camBeginVideo()==started);
  assert(trace==std::string(ready?"":"restart;")+"policy;"+(shoot==2?"audio;":"silent;"));
  assert(s_videoPending&&s_continuousVga&&s_videoStartedMs==123);
  assert(sPreviewFrameSize()==(capture?FRAMESIZE_HD:FRAMESIZE_VGA));
  assert(camVideoQuality()==(capture?16:12));
 }
 s_videoPending=false;s_continuousVga=false;restartOk=false;trace.clear();
 assert(!camBeginVideo()&&!s_videoPending&&!s_continuousVga&&trace=="restart;");
 restartOk=true;beginOk=true;trace.clear();assert(camBeginVideo());
 trace.clear();assert(!camBeginVideo()&&trace.empty());
 s_videoPending=false;busy=true;assert(!camBeginVideo()&&trace.empty());
 for(bool restored:{false,true}){
  finishing=true;restartOk=restored;busy=false;s_videoPending=true;s_continuousVga=true;
  trace.clear();finishVideo();
  assert(trace=="close;policy;restart;"&&!s_videoPending&&!s_continuousVga);
 }
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 for flags in ([],['-DORIGAMI_VIDEO_HD_TRIAL'],['-DORIGAMI_VIDEO_HD_TRIAL','-DORIGAMI_VIDEO_HD_PREVIEW_TRIAL'],['-DORIGAMI_VIDEO_CAPTURE_ONLY_TRIAL']):
  subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',*flags,str(p/'test.cpp'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
print('PASS production video framing/start/finish: selected VGA/720p before audio/writer, Photo default, restart failure, retry, busy admission, acknowledged close before QVGA return')
