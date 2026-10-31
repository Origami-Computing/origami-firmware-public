"""Exercise production Camera startup with optional preview allocation failures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/apps/camera_menu_app.cpp').read_text()
start = source.index('static bool camBeginVideo() {')
end = source.index('\n#ifdef ORIGAMI_DEV_SERIAL_INPUT', start)
body = source[start:end]
code = r'''
#define ORIGAMI_ASYNC_CAMERA_PREVIEW
#include <cassert>
#include <cstdint>
bool s_videoPending=false, s_continuousVga=false, s_reviewActive=false;
bool s_videoStopping=false, previewOk=true, restartOk=true, recordOk=true;
bool failWithPreview=false;
bool camVideoHd(){return false;}
int s_camera=0, s_videoPerf=0, s_cameraStage=0, s_shootMode=2;
uint32_t s_videoStartedMs=0,s_videoPreviewMs=0,s_previewSubmittedUs=0;
struct Preview {
 bool running=false;
 bool begin(){running=previewOk;return running;}
 bool active(){return running;}
 void close(){running=false;}
} s_previewDecode;
struct Renderer {
 bool surfaces=true; int prepares=0;
 bool hasSurfaces(){return surfaces;}
 void closeDecoder(){}
 bool prepareDecoder(bool,bool){++prepares;return true;}
} s_cameraPreview;
struct Video {
 bool running=false; int calls=0;
 bool busy(){return running;}
 bool beginCamera(int,uint16_t w,uint16_t h,bool audio){
  assert(s_videoPending && w==640 && h==480 && audio);
  ++calls;return recordOk && !(failWithPreview && s_previewDecode.active());
 }
} s_video;
struct NavHandler {static NavHandler& instance(){static NavHandler n;return n;}
 void runtimePolicyChanged(){} };
struct Logger {template<class... T> void printf(const char*,T...){}
 void println(const char*){} } Serial;
uint32_t millis(){return 123;}
bool camRestorePreviewAfterCapture(uint32_t ms){assert(ms==700);return restartOk;}
''' + body + r'''
void reset(){
 s_video={};s_cameraPreview={};s_previewDecode={};s_videoPending=false;
 s_continuousVga=false;previewOk=restartOk=recordOk=true;failWithPreview=false;
}
int main(){
 reset();assert(camBeginVideo());assert(s_previewDecode.active()&&s_video.calls==1);
 reset();previewOk=false;assert(camBeginVideo());
 assert(!s_previewDecode.active()&&s_video.calls==1&&s_cameraPreview.prepares==1);
 reset();restartOk=false;assert(!camBeginVideo());
 assert(!s_previewDecode.active()&&s_video.calls==0&&!s_videoPending);
 reset();failWithPreview=true;assert(camBeginVideo());
 assert(!s_previewDecode.active()&&s_video.calls==2&&s_cameraPreview.prepares==1);
 reset();recordOk=false;assert(!camBeginVideo());
 assert(!s_previewDecode.active()&&s_video.calls==2); // Bounded retry.
 reset();s_cameraPreview.surfaces=false;assert(camBeginVideo());
 assert(!s_previewDecode.active()&&s_video.calls==1);
 reset();s_video.running=true;assert(!camBeginVideo());assert(s_video.calls==0);
 reset();s_videoPending=true;assert(!camBeginVideo());assert(s_video.calls==0);
}
'''
with tempfile.TemporaryDirectory() as folder:
    p = Path(folder)
    (p / 'check.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++17', '-fsanitize=address,undefined',
                    str(p / 'check.cpp'), '-o', str(p / 'check')], check=True)
    subprocess.run([str(p / 'check')], check=True)
print('PASS Camera optional preview startup, failed restart cleanup and bounded recording fallback')
