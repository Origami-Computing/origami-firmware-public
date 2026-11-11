"""Exercise the production mailbox's ordering, ownership and bounded failure paths."""
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory() as directory:
        temp=Path(directory)
        def write(name,text):
            p=temp/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
        write('freertos/FreeRTOS.h','#pragma once\n')
        write('freertos/portmacro.h','''#pragma once
#include <mutex>
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
''')
        write('esp_heap_caps.h','''#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void* heap_caps_malloc(size_t,int);
void heap_caps_free(void*);
''')
        write('main.cpp',r'''
#include "services/agent_reply_mailbox.h"
#include <cassert>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
using Box=services::agent_transport::ReplyMailbox;
std::atomic<int> live{0};
bool failAllocation=false;
Box* resetDuringAllocation=nullptr;
void* heap_caps_malloc(size_t n,int) {
 if(failAllocation)return nullptr;
 if(resetDuringAllocation){resetDuringAllocation->reset();resetDuringAllocation=nullptr;}
 void* p=malloc(n);if(p)++live;return p;
}
void heap_caps_free(void* p){if(p){--live;free(p);}}
int main(){
 {
 Box box(256);Box::Event event;Box::Completion end;
 auto epoch=box.epoch();
 assert(box.push(epoch,Box::Type::Message,"hello")==Box::Push::Queued);
 assert(box.push(epoch,Box::Type::Metadata,"emotion")==Box::Push::Queued);
 assert(box.push(epoch,Box::Type::Token," world")==Box::Push::Queued);
 Box::Completion done;done.outcome=services::agent_transport::Outcome::Ok;
 done.sentSamples=123;done.cancelTag=2;
 assert(box.finish(epoch,done));assert(!box.finish(epoch,done));
 assert(!box.takeCompletion(end));
 assert(box.pop(event)&&event.type()==Box::Type::Message&&!strcmp(event.text(),"hello"));
 assert(!box.takeCompletion(end));
 assert(box.pop(event)&&event.type()==Box::Type::Metadata&&!strcmp(event.text(),"emotion"));
 assert(!box.takeCompletion(end));
 assert(box.pop(event)&&!strcmp(event.text()," world"));
 assert(box.takeCompletion(end)&&end.sentSamples==123&&end.cancelTag==2);
 assert(!box.takeCompletion(end));
 assert(box.push(epoch,Box::Type::Token,"late")==Box::Push::Stale);
 const auto fresh=box.reset();
 assert(box.push(epoch,Box::Type::Token,"old")==Box::Push::Stale);
 assert(!strcmp(event.text()," world")); // Consumer owns popped storage across reset.
 assert(!box.finish(epoch,done));
 failAllocation=true;assert(box.push(fresh,Box::Type::Message,"x")==Box::Push::Full);failAllocation=false;
 std::string huge(256,'x');assert(box.push(fresh,Box::Type::Message,huge.c_str())==Box::Push::Full);
 int accepted=0;
 while(box.push(fresh,Box::Type::Token,"0123456789")==Box::Push::Queued)++accepted;
 assert(accepted>0 && box.stats().bytes<=256 && box.stats().events==(size_t)accepted);
 assert(box.finish(fresh,done)); // Terminal result needs no heap/queue slot.
 while(box.pop(event))--accepted;
 assert(accepted==0 && box.takeCompletion(end));
 box.reset();const auto prior=box.epoch();resetDuringAllocation=&box;
 assert(box.push(prior,Box::Type::Message,"raced reset")==Box::Push::Stale);
 assert(box.stats().bytes==0);
 }
 assert(live==0);
 {
 Box box(512);const auto epoch=box.epoch();
 std::thread worker([&]{
  for(int i=0;i<1000;++i){
   auto text=std::to_string(i);
   while(box.push(epoch,Box::Type::Token,text.c_str())==Box::Push::Full)std::this_thread::yield();
  }
  Box::Completion done;done.sentSamples=1000;assert(box.finish(epoch,done));
 });
 Box::Event event;Box::Completion end;int received=0;
 for(;;){
  if(box.pop(event)){assert(std::stoi(event.text())==received++);}
  else if(box.takeCompletion(end))break;
  else std::this_thread::yield();
 }
 worker.join();assert(received==1000 && end.sentSamples==1000 && box.stats().peakBytes<=512);
 }
 assert(live==0);
}
''')
        subprocess.run([shutil.which('c++') or 'c++','-std=c++11','-pthread','-I'+str(temp),'-I'+str(ROOT/'include'),str(ROOT/'src/services/agent_reply_mailbox.cpp'),str(temp/'main.cpp'),'-o',str(temp/'check')],check=True)
        subprocess.run([str(temp/'check')],check=True)
    print('PASS ordered text/completion, stale/reset races, allocation/budget failures, concurrent drain and storage reclamation')


if __name__=='__main__':main()
