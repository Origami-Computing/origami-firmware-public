"""Exercise Photos' real enqueue handler with controlled queue and gallery owners."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'src/apps/photos_app.cpp').read_text()
def function(signature):
    start=source.index(signature);brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
handler=function('static void uploadFailed(')+'\n'+function('static void uploadSelected() {')
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(r'''
#include <cassert>
#include <cstring>
#include "apps/photos_flow.h"
namespace flow=apps::photos::flow;
using flow::Mode;
flow::Model s_m;
unsigned s_noticeUntil=0;constexpr unsigned kNoticeMs=1800;
const char* s_notice=nullptr;bool s_overlayFull=false;
unsigned millis(){return 100;}
enum {QSTATUS_FAILED=3};
struct UploadQueue {enum class ForceResult { Scheduled, Busy }; enum class VideoEnqueue {Queued,AlreadyQueued,Busy,Full,Invalid,AccountRequired,AccountChanged,StorageUnavailable};};
using E=UploadQueue::VideoEnqueue;
struct File {bool video=true;const char* path="/videos/fixture.avi";};
struct Library {bool ready=true;File file;bool browsing(){return ready;}File& operator[](int){return file;}} s_library;
int enqueued=0,notices=0;bool initialized=true,s_needRender=false;const char* s_uploadError=nullptr;E result=E::Queued;
int photos=0,forced=0;bool existing=false,failed=false,forceOk=true,enqueueOk=true;
struct Queue {
 struct View {int count;struct {const char* filePath;int status;const char* id;} item;};
 View view(const char*){return {existing?1:0,{"/videos/fixture.avi",failed?QSTATUS_FAILED:0,"fixture.avi"}};}
 UploadQueue::ForceResult forceSend(const char*){++forced;return forceOk?UploadQueue::ForceResult::Scheduled:UploadQueue::ForceResult::Busy;}
 bool enqueue(const char*,const char*,const char*,bool temporary){assert(!temporary);++photos;return enqueueOk;}
 E enqueueVideo(const char* path){assert(!strcmp(path,"/videos/fixture.avi"));++enqueued;return result;}} queue;
namespace services {namespace uploads {bool ensureInit(){return initialized;}Queue& queue(){return ::queue;}}}
const File& current(){return s_library[s_m.index];}
void redrawImage(){s_needRender=true;}
''' + handler + r'''
void exercise(){
 s_m.mode=Mode::Sheet;s_needRender=false;s_uploadError=nullptr;s_notice=nullptr;s_noticeUntil=0;
 uploadSelected();
 if(s_uploadError){assert(s_m.mode==Mode::Sheet&&s_needRender&&s_overlayFull);}
 else if(s_notice){assert(s_m.mode==Mode::Browse&&s_needRender&&s_noticeUntil==1900);++notices;}
}
int main(){
 flow::loaded(s_m,1);
 for(E value:{E::Queued,E::AlreadyQueued}){result=value;exercise();assert(notices==enqueued);}
 int before=enqueued;
 for(E value:{E::Busy,E::Full,E::Invalid,E::AccountRequired,E::AccountChanged,E::StorageUnavailable}){
  result=value;s_uploadError=nullptr;s_needRender=false;exercise();assert(s_uploadError&&s_needRender);assert(notices==2);
 }
 assert(enqueued==before+6);
 initialized=false;s_uploadError=nullptr;exercise();assert(enqueued==before+6&&s_uploadError);
 initialized=true;s_library.file.video=false;exercise();assert(photos==1&&notices==3&&enqueued==before+6);
 existing=true;failed=true;exercise();assert(forced==1&&notices==4);
 forceOk=false;s_uploadError=nullptr;exercise();assert(forced==2&&notices==4&&s_uploadError);
 failed=false;exercise();assert(forced==2&&notices==5);
 existing=false;enqueueOk=false;s_uploadError=nullptr;exercise();assert(notices==5&&s_uploadError);
 s_library.file.video=true;s_library.ready=false;exercise();assert(enqueued==before+6);
 s_library.ready=true;s_m.index=-1;exercise();s_m.index=1;exercise();assert(enqueued==before+6);
}
'''.replace('int main(){','#include <initializer_list>\nint main(){'))
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(root/'include'),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('PASS real Photos upload handler: queued/duplicate browsing feedback, all rejection paths, initialization failure and invalid selections')
