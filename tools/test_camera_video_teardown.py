"""Exercise actual Camera teardown ordering and exactly-once terminal reporting."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/apps/camera_menu_app.cpp').read_text()
start=source.index('static void camStopViewfinder() {')
end=source.index('\nstatic bool camRestorePreviewAfterCapture',start)
body=source[start:end]
code=r'''#include <cassert>
#include <string>
std::string trace;
bool s_videoPending=false,s_videoStopping=false,s_shutterSkipTap=true;
void camFinishQuickVideo(){trace+='F';}
struct Preview{bool active=false;void close(){active=false;trace+='A';}}s_previewDecode;
struct Video{bool active=true;void close(){assert(!s_previewDecode.active);trace+='V';active=false;}}s_video;
void camReportVideoResult(){assert(!s_video.active&&s_videoPending);trace+='Q';}
struct NavHandler{static NavHandler& instance(){static NavHandler n;return n;}
void invalidateChrome(){trace+='I';}
void runtimePolicyChanged(){assert(!s_video.active&&!s_videoPending&&!s_videoStopping);trace+='N';}};
namespace hal{namespace display{void exitOptimizedRender(){assert(!s_video.active);trace+='D';}}}
struct Camera{void stop(){assert(!s_video.active);trace+='C';}}s_camera;
void camReleasePreviewBuffers(){trace+='R';}
void delay(unsigned ms){assert(ms==100);trace+='L';}
struct Logger{void println(const char*){trace+='P';}}Serial;
'''+body+r'''
int main(){
 const std::string prefix=
#ifdef ORIGAMI_ASYNC_CAMERA_PREVIEW
 "A";
#else
 "";
#endif
 for(bool pending:{false,true})for(bool stopping:{false,true}){
  s_video.active=true;s_videoPending=pending;s_videoStopping=stopping;trace.clear();
  s_previewDecode.active=!prefix.empty();
  camStopViewfinder();assert(trace==prefix+(pending?"VFQNDICRLP":"VFNDICRLP"));
  trace.clear();camStopViewfinder();assert(trace==prefix+"VFNDICRLP");
 }
}
'''
with tempfile.TemporaryDirectory() as folder:
    p=Path(folder);(p/'check.cpp').write_text(code)
    for flags in [[], ['-DORIGAMI_ASYNC_CAMERA_PREVIEW']]:
        subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',*flags,str(p/'check.cpp'),'-o',str(p/'check')],check=True)
        subprocess.run([str(p/'check')],check=True)
print('PASS Camera close acknowledgment precedes one terminal report, policy and hardware release')
