"""Compile the real deletion service; preserve queue-first failure semantics."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
 p=Path(d)
 def write(name,text):
  f=p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(text)
 write('Arduino.h','#pragma once\n#include <cstdio>\n#include <string>\n#include <vector>\nextern std::vector<std::string> events;\nstruct Logger {template<class... T> void printf(const char* fmt,T... args){char s[256];snprintf(s,sizeof(s),fmt,args...);events.push_back(s);}};extern Logger Serial;\n')
 write('services/uploads.h',r'''#pragma once
class UploadQueue {public:
 bool removeByFilePath(const char*);
 class FileMutation {bool ready_;int removed_;public:FileMutation(UploadQueue&,const char*,bool);~FileMutation();bool ready()const{return ready_;}int removed()const{return removed_;}};
};
namespace services {namespace uploads {bool ensureInit();UploadQueue& queue();}}
''')
 write('hal/hal_sd.h','#pragma once\nnamespace hal {namespace sd {bool exists(const char*);bool removeFile(const char*);}}\n')
 write('test.cpp',r'''
#include "services/media_delete.h"
#include "services/uploads.h"
#include "Arduino.h"
#include <cassert>
#include <cstring>
std::vector<std::string> events;
Logger Serial;
bool queued,present,removeOk,mutationReady=true,queueInit=true;
namespace services {namespace uploads {
 bool ensureInit(){return queueInit;}
 UploadQueue& queue(){static UploadQueue q;return q;}
}}
 bool UploadQueue::removeByFilePath(const char* p){assert(!strcmp(p,"/test.wav"));events.push_back("queue");bool old=queued;queued=false;return old;}
UploadQueue::FileMutation::FileMutation(UploadQueue& q,const char* path,bool remove):ready_(mutationReady),removed_(0){assert(remove);if(ready_)removed_=q.removeByFilePath(path);}
UploadQueue::FileMutation::~FileMutation(){if(ready_)events.push_back("release");}
namespace hal {namespace sd {
 bool exists(const char* p){assert(!strcmp(p,"/test.wav"));events.push_back("exists");return present;}
 bool removeFile(const char* p){assert(!strcmp(p,"/test.wav"));events.push_back("remove");if(removeOk)present=false;return removeOk;}
}}
int main(){
 using services::media_delete::Result;using services::media_delete::deleteFile;
 unsigned count=0;
 for(unsigned mask=0;mask<16;++mask){
  const bool wasQueued=mask&1,wasPresent=mask&2,hasResult=mask&8;
  queued=wasQueued;present=wasPresent;removeOk=mask&4;events.clear();
  Result r;r.requested=5;r.removed=6;r.missing=7;r.queueRemoved=8;r.failed=9;
  bool ok=deleteFile("/test.wav",hasResult?&r:nullptr);
  assert(ok==(!wasPresent||removeOk));assert(!queued);
  assert(present==(wasPresent&&!removeOk)); // Failed file removal does not restore queue.
  std::vector<std::string> expected={"queue","exists"};
  char log[256];
  if(!wasPresent)snprintf(log,sizeof(log),"[media-delete] Missing, queue-cleaned=%d: /test.wav\n",wasQueued);
  else {
   expected.push_back("remove");
   if(removeOk)snprintf(log,sizeof(log),"[media-delete] Deleted /test.wav queue-cleaned=%d\n",wasQueued);
   else strcpy(log,"[media-delete] Delete failed: /test.wav\n");
  }
  expected.push_back(log);expected.push_back("release");assert(events==expected);
  assert(r.requested==5+hasResult);
  assert(r.removed==6+(hasResult&&wasPresent&&removeOk));
  assert(r.missing==7+(hasResult&&!wasPresent));
  assert(r.queueRemoved==8+(hasResult&&wasQueued));
  assert(r.failed==9+(hasResult&&wasPresent&&!removeOk));++count;
 }
 for(const char* path:{static_cast<const char*>(nullptr),""})for(bool result:{false,true}){
  events.clear();Result r;assert(!deleteFile(path,result?&r:nullptr));assert(events.empty());
  assert(r.requested==int(result)&&r.failed==int(result));++count;
 }
 assert(count==20);
 for(bool initialized:{false,true}){
  queueInit=initialized;mutationReady=false;queued=present=true;events.clear();Result r;
  assert(!deleteFile("/test.wav",&r)&&queued&&present&&events.empty());assert(r.failed==1&&!r.removed&&!r.queueRemoved);
 }

}
''')
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),str(p/'test.cpp'),str(ROOT/'src/services/media_delete.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS 20 deletion cases: queue-first ordering, exact logs, missing files, failed removal, nullable/accumulated results and invalid paths')
