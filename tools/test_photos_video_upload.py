"""Exercise Photos' real enqueue handler with controlled queue and gallery owners."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'src/apps/photos_app.cpp').read_text()
handler = source[source.index('static void uploadVideo() {'):source.index('\nstatic void drawActions(', source.index('static void uploadVideo() {'))]
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(r'''
#include <cassert>
#include <cstring>
struct UploadQueue {enum class VideoEnqueue {Queued,AlreadyQueued,Busy,Full,Invalid,AccountRequired,AccountChanged,StorageUnavailable};};
using E=UploadQueue::VideoEnqueue;
struct File {bool video=true;const char* path="/videos/fixture.avi";};
struct Library {bool ready=true;File file;bool browsing(){return ready;}File& operator[](int){return file;}} s_library;
int s_idx=0,s_count=1,enqueued=0,closed=0,synced=0;bool initialized=true,s_needRender=false;const char* s_uploadError=nullptr;E result=E::Queued;
struct Queue {E enqueueVideo(const char* path){assert(!strcmp(path,"/videos/fixture.avi"));++enqueued;return result;}} queue;
namespace services {namespace uploads {bool ensureInit(){return initialized;}Queue& queue(){return ::queue;}}}
void closeActions(){++closed;}void startSync(){++synced;}
''' + handler + r'''
int main(){
 for(E value:{E::Queued,E::AlreadyQueued}){result=value;uploadVideo();assert(closed==synced&&synced==enqueued);}
 int before=enqueued;
 for(E value:{E::Busy,E::Full,E::Invalid,E::AccountRequired,E::AccountChanged,E::StorageUnavailable}){
  result=value;s_uploadError=nullptr;s_needRender=false;uploadVideo();assert(s_uploadError&&s_needRender);assert(synced==2&&closed==2);
 }
 assert(enqueued==before+6);
 initialized=false;s_uploadError=nullptr;uploadVideo();assert(enqueued==before+6&&s_uploadError);
 initialized=true;s_library.file.video=false;uploadVideo();assert(enqueued==before+6);
 s_library.file.video=true;s_library.ready=false;uploadVideo();assert(enqueued==before+6);
 s_library.ready=true;s_idx=-1;uploadVideo();s_idx=1;uploadVideo();assert(enqueued==before+6);
}
'''.replace('int main(){','#include <initializer_list>\nint main(){'))
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('PASS real Photos upload handler: queued/duplicate handoff, all rejection paths, initialization failure and invalid selections')
