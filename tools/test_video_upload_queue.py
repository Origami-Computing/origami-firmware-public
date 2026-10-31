"""Compile the actual queue adapter: only verified completion maps to success."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/upload_queue.cpp').read_text();header=(ROOT/'include/upload_queue.h').read_text()
def block(text,marker):
 a=text.index(marker);b=text.index('{',a)+1;depth=1
 while depth:depth+=(text[b]=='{')-(text[b]=='}');b+=1
 return text[a:b]
code=r'''
#include "services/video_upload.h"
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <functional>
enum QueueItemStatus:uint8_t {QSTATUS_PENDING,QSTATUS_UPLOADING,QSTATUS_COMPLETED,QSTATUS_FAILED};
uint32_t now=100;uint32_t millis(){return now;}
bool locked=false;int feeds=0,yields=0;
struct MutexGuard {bool taken=true;MutexGuard(int){assert(!locked);locked=true;}~MutexGuard(){locked=false;}};
void vTaskDelay(int n){assert(!locked&&n==1);++yields;now+=n;}
namespace core {namespace watchdog {void feed(){++feeds;}}}
'''+block(header,'struct QueueItem {')+';\nclass UploadQueue {public:\n'+block(header,'    enum class UploadResultKind')+';\n'+block(header,'    struct UploadResult {')+r''';
 QueueItem m_items[2]{};int m_count=1,m_mutex=0;bool m_workerStop=false,m_uploadPaused=false;
 UploadResult doVideoUpload(QueueItem&);
};
'''+block(source,'UploadQueue::UploadResult UploadQueue::doVideoUpload(')+r'''
using namespace services::video_upload;
Result next{Step::Progress,204,250,"fixture"};std::function<void()> during;
namespace services {namespace video_upload {
Result advance(const char* id,const char* path,bool (*check)(void*),void* context){
 assert(!strcmp(id,"VID_test.avi")&&!strcmp(path,"/videos/VID_test.avi"));
 if(!check(context))return {Step::Cancelled,0,0,"paused"};
 if(during)during();now+=11;
 if(!check(context))return {Step::Cancelled,0,0,"paused"};
 return next;
}
}}
int main(){
 UploadQueue q;auto& live=q.m_items[0];strcpy(live.id,"VID_test.avi");strcpy(live.filePath,"/videos/VID_test.avi");live.status=QSTATUS_UPLOADING;
 using Kind=UploadQueue::UploadResultKind;
 const Step steps[]={Step::Complete,Step::Progress,Step::Retry,Step::Deferred,Step::Cancelled,Step::Rejected};
 const Kind kinds[]={Kind::HttpResponse,Kind::Progress,Kind::RetryableFailure,Kind::GlobalDeferral,Kind::Aborted,Kind::PermanentItemFailure};
 for(unsigned i=0;i<6;++i){
  QueueItem snapshot=live;next={steps[i],204,5000,"fixture"};const auto r=q.doVideoUpload(snapshot);
  assert(r.kind==kinds[i]&&!strcmp(snapshot.lastError,"fixture"));
  assert(r.attemptStarted==(steps[i]==Step::Complete||steps[i]==Step::Retry));
  if(steps[i]==Step::Complete)assert(r.httpCode==200);
 }
 auto aborted=[&](){QueueItem snapshot=live;assert(q.doVideoUpload(snapshot).kind==Kind::Aborted);};
 q.m_workerStop=true;aborted();q.m_workerStop=false;
 q.m_uploadPaused=true;aborted();q.m_uploadPaused=false;
 during=[&](){q.m_count=0;};aborted();q.m_count=1;
 during=[&](){live.status=QSTATUS_PENDING;};aborted();live.status=QSTATUS_UPLOADING;
 during=[&](){q.m_workerStop=true;};aborted();q.m_workerStop=false;
 during={};assert(feeds&&yields&&!locked);
 puts("PASS actual queue adapter: verified completion only, progress/retry distinction, pause/stop/removal and unlocked yielding");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'main.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
