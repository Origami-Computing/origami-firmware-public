"""Exercise actual Camera teardown ordering and exactly-once terminal reporting."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/apps/camera_menu_app.cpp').read_text()
start=source.index('static void camStopViewfinder() {')
end=source.index('\nstatic bool camRestorePreviewAfterCapture',start)
body=source[start:end]
helper=source[source.index('static bool camQueueSavedVideo() {'):source.index('// Report terminal recordings')]
code=r'''#include <cassert>
#include <string>
std::string trace;
bool s_videoPending=false,s_videoStopping=false,s_shutterSkipTap=true;
void camFinishQuickVideo(){trace+='F';}
struct Preview{bool active=false;void close(){active=false;trace+='A';}}s_previewDecode;
namespace services {namespace camera {struct VideoRecording{enum class State{Saved,Failed};};}}
using State=services::camera::VideoRecording::State;
struct Video{bool active=true;State result=State::Saved;
 State state(){assert(!active);return result;}const char* path(){return "/videos/test-exit.avi";}
 void close(){assert(!s_previewDecode.active);trace+='V';active=false;}}s_video;
struct UploadQueue{enum class VideoEnqueue{Queued,AlreadyQueued,StorageUnavailable};};
namespace services{namespace uploads{bool ready=true;unsigned calls=0;
 UploadQueue::VideoEnqueue outcome=UploadQueue::VideoEnqueue::Queued;
 bool ensureInit(){return ready;}struct Queue{UploadQueue::VideoEnqueue enqueueVideo(const char*){++calls;trace+='E';return outcome;}};
 Queue& queue(){static Queue q;return q;}}}
void camReportVideoResult(){assert(!s_video.active&&s_videoPending);trace+='Q';}
struct NavHandler{static NavHandler& instance(){static NavHandler n;return n;}
void invalidateChrome(){trace+='I';}
void runtimePolicyChanged(){assert(!s_video.active&&!s_videoPending&&!s_videoStopping);trace+='N';}};
namespace hal{namespace display{void exitOptimizedRender(){assert(!s_video.active);trace+='D';}}}
struct Camera{void stop(){assert(!s_video.active);trace+='C';}}s_camera;
void camReleasePreviewBuffers(){trace+='R';}
void delay(unsigned ms){assert(ms==100);trace+='L';}
struct Logger{void println(const char*){trace+='P';}template<class... T>void printf(const char*,T...){}}Serial;
'''+helper+'\n'+body+r'''
int main(){
 const std::string prefix=
#ifdef ORIGAMI_ASYNC_CAMERA_PREVIEW
 "A";
#else
 "";
#endif
 for(bool pending:{false,true})for(bool stopping:{false,true})for(auto state:{State::Saved,State::Failed}){
  s_video.result=state;const auto before=services::uploads::calls;
  s_video.active=true;s_videoPending=pending;s_videoStopping=stopping;trace.clear();
  s_previewDecode.active=!prefix.empty();
  camStopViewfinder();assert(trace==prefix+(pending?(state==State::Saved?"VFEQNDICRLP":"VFQNDICRLP"):"VFNDICRLP"));
  assert(services::uploads::calls==before+unsigned(pending&&state==State::Saved));
  trace.clear();camStopViewfinder();assert(trace==prefix+"VFNDICRLP");
  assert(services::uploads::calls==before+unsigned(pending&&state==State::Saved));
 }
}
'''
with tempfile.TemporaryDirectory() as folder:
    p=Path(folder);(p/'check.cpp').write_text(code)
    for flags in [[], ['-DORIGAMI_ASYNC_CAMERA_PREVIEW']]:
        subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',*flags,str(p/'check.cpp'),'-o',str(p/'check')],check=True)
        subprocess.run([str(p/'check')],check=True)
print('PASS Camera close acknowledgment precedes one terminal report, policy and hardware release')
