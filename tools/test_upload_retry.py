"""Fault-test actual queue result application and scheduling with deterministic I/O/time.

Compiles production processOnePending/helpers; persistence and network are fakes.
Checks no-attempt prerequisite/cancellation, turn fairness, auth, response loss,
file ownership and stable identity during concurrent enqueue/cancel callbacks.
"""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/upload_queue.cpp').read_text(); header=(ROOT/'include/upload_queue.h').read_text()
def block(text, marker):
    a=text.index(marker); b=text.index('{',a)+1; depth=1
    while depth: depth+=(text[b]=='{')-(text[b]=='}'); b+=1
    return text[a:b]
methods='\n'.join(block(source, marker) for marker in [
    'static void clearRetryState(', 'bool UploadQueue::processOnePending(',
    'int UploadQueue::findRunnableIndexUnlocked(', 'int UploadQueue::forcedIndexUnlocked(',
    'bool UploadQueue::applyResetAfterFlightUnlocked(', 'bool UploadQueue::isRetryable(',
    'void UploadQueue::removeItemAtUnlocked(', 'void UploadQueue::rotateItemToBackUnlocked(',
    'void UploadQueue::deleteFileIfOwnedUnlocked('])
code=r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "upload_queue_policy.h"
enum QueueItemStatus:uint8_t {QSTATUS_PENDING,QSTATUS_UPLOADING,QSTATUS_COMPLETED,QSTATUS_FAILED};
uint32_t now=100;uint32_t millis(){return now;}
struct MutexGuard {bool taken=true;MutexGuard(int){}};
struct {template<class... T>void printf(const char*,T...){}}Serial;
struct {std::vector<std::string> deleted;void remove(const char* p){deleted.emplace_back(p);}}SD;
'''+block(header,'struct QueueItem {')+';\n'+r'''
class UploadQueue {public:
'''+block(header,'    enum class UploadResultKind')+';\n'+block(header,'    struct UploadResult {')+';\n'+r'''
 QueueItem m_items[20]{};int m_count=0,m_mutex=0;
 bool m_uploadPaused=false,m_resetAfterFlight=false;
 uint32_t m_nextNetworkRetryAt=0;
 char m_forcedItemId[24]{},m_inFlightId[24]{},m_inFlightPath[96]{},m_lastStatus[64]{};
 int requests=0,saves=0;std::function<void()> duringUpload;
 UploadResult result{UploadResultKind::HttpResponse,200,true,0};
 UploadResult doUpload(QueueItem& item){++requests;assert(!strcmp(m_inFlightId,item.id));if(duringUpload)duringUpload();assert(!strcmp(m_inFlightId,item.id));strcpy(item.lastError,"injected");return result;}
 bool saveManifestUnlocked(){++saves;return true;}
 uint32_t jitteredDelayMs(uint32_t n){return n;}
 bool processOnePending();int findRunnableIndexUnlocked(uint32_t)const;int forcedIndexUnlocked()const;
 bool applyResetAfterFlightUnlocked(QueueItem&,bool);bool isRetryable(int);
 void removeItemAtUnlocked(int);void rotateItemToBackUnlocked(int);void deleteFileIfOwnedUnlocked(const QueueItem&);
 void add(const char* id,bool owned=false){auto& item=m_items[m_count++];strcpy(item.id,id);strcpy(item.filePath,id);item.deleteAfterUpload=owned;}
};
'''+methods+r'''
int main(){
 {UploadQueue q;q.add("video",true);strcpy(q.m_items[0].type,"video");q.deleteFileIfOwnedUnlocked(q.m_items[0]);assert(SD.deleted.empty());}
 {UploadQueue q;q.add("video");q.duringUpload=[&]{q.removeItemAtUnlocked(0);assert(!strcmp(q.m_inFlightId,"video"));};assert(q.processOnePending()&&!q.m_inFlightId[0]);}
using Kind=UploadQueue::UploadResultKind;
 for(auto kind:{Kind::GlobalDeferral,Kind::Aborted}){
  UploadQueue q;q.add("a",true);q.result={kind,-1,false,5000};
  assert(!q.processOnePending());assert(q.m_count==1&&q.m_items[0].status==QSTATUS_PENDING);
  assert(q.m_items[0].attempts==0&&SD.deleted.empty());
  if(kind==Kind::GlobalDeferral){assert(q.m_nextNetworkRetryAt==now+5000);assert(!q.processOnePending()&&q.requests==1);}
 }
 for(int code:{200,201,204})for(bool owned:{false,true}){
  SD.deleted.clear();UploadQueue q;q.add("a",owned);q.result={Kind::HttpResponse,code,true,0};
  assert(q.processOnePending()&&q.m_count==0);assert(SD.deleted.size()==(owned?1:0));
 }
 SD.deleted.clear();
 // Chunk/session progress cannot delete media or consume the failure budget.
 for(uint32_t defer:{0u,1000u,0xffffffffu}) {
  UploadQueue q;q.add("video",true);q.add("photo");
  q.m_items[0].attempts=19;q.m_items[0].attemptsThisTurn=2;
  q.result={Kind::Progress,204,true,defer};
  assert(!q.processOnePending()&&q.m_count==2&&SD.deleted.empty());
  const auto& video=q.m_items[1];
  assert(!strcmp(video.id,"video")&&video.status==QSTATUS_PENDING);
  assert(video.attempts==19&&video.attemptsThisTurn==0&&!video.lastError[0]);
  assert(video.nextEligibleAt==now+(defer<250?250:defer>300000?300000:defer));
  assert(q.m_nextNetworkRetryAt==0&&q.saves>0);
  q.result={Kind::HttpResponse,200,true,0};assert(q.processOnePending());
  assert(q.m_count==1&&!strcmp(q.m_items[0].id,"video")&&SD.deleted.empty());
  assert(!q.processOnePending());now=q.m_items[0].nextEligibleAt;
  assert(q.processOnePending()&&q.m_count==0&&SD.deleted.size()==1);SD.deleted.clear();
 }
 {UploadQueue q;q.add("video",true);q.m_resetAfterFlight=true;q.result={Kind::Progress,200,true,1000};
  assert(q.processOnePending()&&q.m_count==1&&q.m_items[0].attempts==0&&SD.deleted.empty());}
 {UploadQueue q;q.add("video",true);q.add("photo");q.result={Kind::Progress,204,true,1000};
  q.duringUpload=[&]{q.removeItemAtUnlocked(0);};assert(q.processOnePending());
  assert(q.m_count==1&&!strcmp(q.m_items[0].id,"photo")&&SD.deleted.empty());}
 // Successful HTTP without a verified receipt is a transfer failure, not
 // permission to remove the item or its queue-owned source.
 {UploadQueue q;q.add("unconfirmed",true);q.result={Kind::RetryableFailure,200,true,0};
  assert(q.processOnePending());assert(q.m_count==1&&q.m_items[0].attempts==1&&SD.deleted.empty());}
 for(int code:{400,404,413,415}){
  UploadQueue q;q.add("a",true);q.result={Kind::HttpResponse,code,true,0};
  assert(q.processOnePending()&&q.m_count==0&&SD.deleted.empty());
 }
 for(int code:{408,429,500,503}){
  UploadQueue q;q.add("a");q.add("b");q.result={Kind::HttpResponse,code,true,0};
  for(int n=1;n<=4;++n){
   assert(q.processOnePending());
   if(n<4){assert(q.m_items[0].attempts==n);assert(!q.processOnePending());now=q.m_items[0].nextEligibleAt;}
  }
  assert(q.m_items[0].id[0]=='b'&&q.m_items[1].id[0]=='a');
  assert(q.m_items[1].attempts==4&&q.m_items[1].attemptsThisTurn==0);
  assert(q.m_items[1].nextEligibleAt==now+300000);
  q.result={Kind::HttpResponse,200,true,0};assert(q.processOnePending());
  assert(q.m_count==1&&q.m_items[0].id[0]=='a');assert(!q.processOnePending());
  now=q.m_items[0].nextEligibleAt;assert(q.processOnePending()&&q.m_count==0);
 }
 for(int code:{401,403}){
  UploadQueue q;q.add("a");q.result={Kind::HttpResponse,code,true,0};
  assert(!q.processOnePending()&&q.m_items[0].attempts==1&&q.m_count==1);
  assert(q.m_nextNetworkRetryAt==now+60000);
  assert(!q.processOnePending()&&q.requests==1);
 }
 {UploadQueue q;q.add("a",true);q.m_items[0].attempts=19;q.result={Kind::RetryableFailure,-1,true,0};
  assert(q.processOnePending()&&q.m_count==0&&SD.deleted.empty());}
 {UploadQueue q;q.add("a");q.duringUpload=[&]{q.add("b");};assert(q.processOnePending());
  assert(q.m_count==1&&!strcmp(q.m_items[0].id,"b"));}
 {UploadQueue q;q.add("a");q.add("b");q.duringUpload=[&]{q.removeItemAtUnlocked(0);};
  assert(q.processOnePending()&&q.m_count==1&&!strcmp(q.m_items[0].id,"b"));}
 for(bool success:{false,true}){
  UploadQueue q;q.add("a");q.m_resetAfterFlight=true;q.result={Kind::HttpResponse,success?200:503,true,0};
  assert(q.processOnePending());assert(q.m_count==(success?0:1));
  if(!success)assert(q.m_items[0].attempts==0&&q.m_items[0].status==QSTATUS_PENDING);
 }
 {UploadQueue q;q.add("a");q.m_uploadPaused=true;assert(!q.processOnePending()&&q.requests==0);}
 {UploadQueue q;q.add("a");now=0xfffffff0u;q.m_items[0].nextEligibleAt=0x10u;
  assert(!q.processOnePending());now=0x10u;assert(q.processOnePending());}
 puts("PASS actual retry application: all result classes, fairness, auth backoff, lifetime cap, ownership, enqueue/cancel/reset during upload and millis wrap");
}
'''
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);(root/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I',str(ROOT/'include'),str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
