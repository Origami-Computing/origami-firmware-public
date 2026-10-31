"""Compile actual video enqueue and legacy dispatch with fake persistence faults."""
from pathlib import Path
import os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/upload_queue.cpp').read_text();header=(ROOT/'include/upload_queue.h').read_text()
JSON=Path(os.environ.get('ARDUINOJSON_SRC',ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src'))
def block(text,marker):
 a=text.index(marker);b=text.index('{',a)+1;depth=1
 while depth:depth+=(text[b]=='{')-(text[b]=='}');b+=1
 return text[a:b]
code=r'''
#include "services/video_upload_state.h"
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <string>
#include <vector>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,UPLOAD_QUEUE_MAX_ITEMS=20;
bool mounted=true,eraseOk=true,saveOk=true,manifestOk=true,noMemory=false,haveState=false;
int allocations=0,erases=0,saves=0,manifests=0,notifies=0;size_t fileBytes=123;
std::vector<std::string> events;
services::video_upload::Resume persisted;
core::secrets::BackendConfig backend;
void* heap_caps_malloc(size_t n,int caps){assert(caps==3);if(noMemory)return nullptr;++allocations;return malloc(n);}
void heap_caps_free(void* p){if(p){--allocations;free(p);}}
uint32_t millis(){return 100;}
struct MutexGuard {bool taken=true;MutexGuard(int){}};
struct {void println(const char*){}template<class...T>void printf(const char*,T...){}}Serial;
struct {bool exists(const char*){return mounted;}}SD;
namespace hal {namespace sd {bool fileInfo(const char*,size_t& bytes,uint32_t& modified){bytes=fileBytes;modified=1;return mounted;}}}
namespace core {namespace secrets {BackendConfig backendConfig(){return backend;}}}
namespace services {namespace video_upload {
bool binding(const core::secrets::BackendConfig& b,char (&out)[65]){if(!b.apiKey[0])return false;memset(out,!strcmp(b.apiKey,"account")?'b':'c',64);out[64]=0;return true;}
Load load(const char*,Resume& out){if(!haveState)return Load::Missing;out=persisted;return Load::Ready;}
bool erase(const char*){++erases;events.push_back("erase");if(!eraseOk)return false;haveState=false;return true;}
Save save(const char*,const Resume& r){++saves;events.push_back("state");if(!saveOk)return Save::Unavailable;persisted=r;haveState=true;return Save::Saved;}
}}
enum QueueItemStatus:uint8_t{QSTATUS_PENDING,QSTATUS_UPLOADING,QSTATUS_COMPLETED,QSTATUS_FAILED};
'''+block(header,'struct QueueItem {')+';\nclass UploadQueue {public:\n'+block(header,'    enum class VideoEnqueue')+r''';
 QueueItem items[20]{};QueueItem* m_items=items;int m_count=0,m_mutex=0;bool m_initialized=true;
 char m_inFlightId[24]{},m_mutationPath[96]{},m_lastStatus[64]{};
 VideoEnqueue enqueueVideo(const char*);
 bool enqueue(const char*,const char*,const char*,bool=true);
 bool mutationBlocksUnlocked(const char*)const;
 bool makeRoomForOneUnlocked(){assert(false);return false;}
 bool saveManifestUnlocked(){++manifests;events.push_back("manifest");assert(m_count>0);return manifestOk;}
 void notifyWorker(){++notifies;events.push_back("notify");}
};
'''+block(source,'bool withinUploadPath(')+'\n'+block(source,'bool UploadQueue::mutationBlocksUnlocked(')+'\n'+block(source,'UploadQueue::VideoEnqueue UploadQueue::enqueueVideo(')+'\n'+block(source,'bool UploadQueue::enqueue(')+r'''
void reset(){assert(!allocations);mounted=eraseOk=saveOk=manifestOk=true;noMemory=haveState=false;allocations=erases=saves=manifests=notifies=0;fileBytes=123;backend={};strcpy(backend.apiKey,"account");strcpy(backend.url,"https://fixture.invalid");persisted={};events.clear();}
int main(){
 using E=UploadQueue::VideoEnqueue;const char* path="/videos/VID_test.avi";
 reset();{UploadQueue q;assert(q.enqueueVideo(path)==E::Queued&&q.m_count==1);
 assert((events==std::vector<std::string>{"erase","state","manifest","notify"}));
 assert(!q.items[0].deleteAfterUpload&&!strcmp(q.items[0].type,"video")&&!strcmp(q.items[0].filePath,path));
 assert(!persisted.media.bytes&&!persisted.media.sha256[0]&&!persisted.session[0]);
 assert(q.enqueueVideo(path)==E::AlreadyQueued&&saves==1&&manifests==1);
 strcpy(backend.apiKey,"other");assert(q.enqueueVideo(path)==E::AccountChanged&&saves==1);
 }
 reset();{UploadQueue q;strcpy(q.m_inFlightId,"VID_test.avi");assert(q.enqueueVideo(path)==E::Busy&&events.empty());
 assert(!q.enqueue("image",path,"image/jpeg",false)&&events.empty());}
 reset();{UploadQueue q;strcpy(q.m_mutationPath,"/VIDEOS");assert(q.enqueueVideo(path)==E::Busy&&events.empty());assert(!q.enqueue("image",path,"image/jpeg",false));}
 reset();{UploadQueue q;q.m_count=20;assert(q.enqueueVideo(path)==E::Full&&events.empty());}
 for(const char* bad:{"/other/VID_test.avi","/videos/../bad.avi","/videos/.hidden.avi","/videos/overly_long_recording_name.avi","/videos/VID_test.wav"}){
 reset();UploadQueue q;assert(q.enqueueVideo(bad)==E::Invalid&&events.empty());}
 reset();{UploadQueue q;assert(!q.enqueue("video",path,"video/x-msvideo",true)&&events.empty());assert(q.enqueue("video",path,"video/x-msvideo",false)&&q.m_count==1);}
 reset();{UploadQueue q;backend.apiKey[0]=0;assert(q.enqueueVideo(path)==E::AccountRequired&&events.empty());}
 reset();{UploadQueue q;mounted=false;assert(q.enqueueVideo(path)==E::StorageUnavailable&&!q.m_count&&events.empty());}
 reset();{UploadQueue q;fileBytes=0;assert(q.enqueueVideo(path)==E::Invalid&&events.empty());}
 reset();{UploadQueue q;fileBytes=core::video_upload::kMaximumBytes+1u;assert(q.enqueueVideo(path)==E::Invalid&&events.empty());}
 reset();{UploadQueue q;eraseOk=false;assert(q.enqueueVideo(path)==E::StorageUnavailable&&!q.m_count&&!saves&&!notifies);}
 reset();{UploadQueue q;saveOk=false;assert(q.enqueueVideo(path)==E::StorageUnavailable&&!q.m_count&&!manifests&&!notifies);}
 reset();{UploadQueue q;manifestOk=false;assert(q.enqueueVideo(path)==E::StorageUnavailable&&!q.m_count&&haveState&&!notifies);
 manifestOk=true;assert(q.enqueueVideo(path)==E::Queued&&q.m_count==1);}
 reset();{UploadQueue q;noMemory=true;assert(q.enqueueVideo(path)==E::StorageUnavailable&&events.empty());}
 reset();{UploadQueue q;haveState=true;persisted.media.bytes=123;strcpy(persisted.session,"old");assert(q.enqueueVideo(path)==E::Queued&&!persisted.media.bytes&&!persisted.session[0]);}
 reset();puts("PASS actual video enqueue: bound publication ordering, no original ownership, ID fencing, rehash on requeue, full queue and persistence faults");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'main.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),'-I'+str(JSON),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
