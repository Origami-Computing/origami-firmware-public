"""Test real gallery service/reservation against faultable catalog, network and queue."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('services/uploads.h',r'''#pragma once
#include <stdint.h>
struct Queue {void setUploadPaused(bool);struct Counts {int pending,failed;};Counts counts();bool removeByFilePath(const char*);};
namespace services {namespace uploads {bool ensureInit();Queue& queue();}}
''')
    write('services/media_delete.h','extern bool videoDeleteOk;namespace services {namespace media_delete {struct Result {int queueRemoved=0;};inline bool deleteFile(const char*,Result*r){r->queueRemoved=1;return videoDeleteOk;}}}')
    write('services/background_network.h', '#include <stdint.h>\nnamespace services {namespace background_network {void requestPeripheralIo();bool peripheralIoReady();bool suspendForPeripheralIo(uint32_t);void resumeAfterPeripheralIo();}}')
    write('hal/hal_wifi.h','namespace hal {namespace wifi {void stop();void scheduleRestore();bool connected();}}')
    write('main.cpp',r'''
#include "services/photo_library.h"
#include "services/uploads.h"
#include <cassert>
#include <cstring>
#include <string>
std::string trace;bool uploadOk=true,agentOk=true,removeOk=true,connected=true;
bool videoFile=false,videoDeleteOk=false;
int removedQueue=0,loads=0,pending=3,failed=1;Queue queue;
namespace services {namespace uploads {
bool ensureInit(){trace+='I';return true;}Queue& queue(){return ::queue;}
}}
namespace services {namespace background_network {
bool suspendForPeripheralIo(uint32_t t){assert(t==8000);trace+='P';return uploadOk;}
void requestPeripheralIo(){trace+='R';}bool peripheralIoReady(){return uploadOk;}
void resumeAfterPeripheralIo(){trace+='p';}
}}
namespace services {namespace agent_coordination {
bool TransportPause::pollAcquire(){if(!held_){trace+='A';held_=true;}return agentOk;}
bool TransportPause::acquire(uint32_t t){if(held_)return true;assert(t==8000);trace+='A';return held_=agentOk;}
void TransportPause::release(){if(!held_)return;held_=false;trace+='a';}
}}
namespace hal {namespace wifi {
void stop(){trace+='W';}void scheduleRestore(){trace+='w';}bool connected(){return ::connected;}
}}
void Queue::setUploadPaused(bool paused){assert(!paused);trace+='U';}
Queue::Counts Queue::counts(){return {pending,failed};}
bool Queue::removeByFilePath(const char*p){assert(!strcmp(p,"/photos/IMG_00001.jpg"));trace+='q';if(removedQueue==2)return false;++removedQueue;return true;}
namespace hal {namespace sd {
PhotoCatalogResult PhotoCatalog::load(bool){++loads;clear();files_=new PhotoFile[1];strcpy(files_[0].path,"/photos/IMG_00001.jpg");files_[0].modified=123;files_[0].video=videoFile;count_=1;return PhotoCatalogResult::Ok;}
bool PhotoCatalog::removeAt(size_t i,bool removed){assert(removed==videoFile);assert(i<count_);trace+='D';if(!removeOk)return false;clear();return true;}
void PhotoCatalog::clear(){delete[] files_;files_=nullptr;count_=0;}
}}
int main(){using L=services::photos::Library;using B=L::BrowseResult;
 {L lib;assert(lib.load()==L::LoadResult::ReadError&&loads==0);lib.close();assert(trace.empty());
 uploadOk=false;assert(lib.beginBrowsing()==B::UploadsBusy&&!lib.browsing());assert(trace=="P");lib.close();assert(trace=="Pw");
 uploadOk=true;agentOk=false;trace.clear();assert(lib.beginBrowsing()==B::NetworkBusy&&!lib.browsing());assert(trace=="PAp");lib.close();assert(trace=="PApw");
 agentOk=true;trace.clear();assert(lib.beginBrowsing()==B::Ready&&lib.browsing());assert(trace=="PAWa");
 assert(lib.load()==L::LoadResult::Ok&&lib.count()==1);assert(lib[0].modified==123);
 trace.clear();assert(lib.beginBrowsing()==B::Ready&&trace=="AWa"); // retains existing worker reservation
 trace.clear();int removed=-1;removeOk=false;assert(!lib.removeAt(0,&removed)&&removed==0&&lib.count()==1&&trace=="D");
 removeOk=true;trace.clear();assert(lib.removeAt(0,&removed)&&removed==2&&lib.count()==0&&trace=="Dqqq");
 trace.clear();assert(!lib.removeAt(0,&removed)&&removed==0&&trace.empty());
 videoFile=true;lib.load();trace.clear();videoDeleteOk=false;
 assert(!lib.removeAt(0,&removed)&&lib.count()==1&&trace.empty());
 videoDeleteOk=true;assert(lib.removeAt(0,&removed)&&lib.count()==0&&removed==1&&trace=="D");
 videoFile=false;
 lib.load();trace.clear();assert(lib.beginSync()==3&&!lib.browsing()&&lib.count()==1);assert(trace=="IUwp");
 auto status=lib.syncStatus();assert(status.pending==3&&status.failed==1&&status.connected);assert(trace=="IUwp");
 // UI polling observes service progress without any worker-control API.
 pending=2;assert(lib.syncStatus().pending==2&&trace=="IUwp");
 pending=0;assert(lib.syncStatus().pending==0&&trace=="IUwp");
 connected=false;assert(!lib.syncStatus().connected&&trace=="IUwp");
 assert(!lib.removeAt(0,&removed));
 trace.clear();assert(lib.beginBrowsing()==B::Ready&&trace=="PAWa"&&lib.count()==1);
 agentOk=false;trace.clear();assert(lib.beginBrowsing()==B::NetworkBusy&&trace=="Ap"&&!lib.browsing());
 agentOk=true;lib.beginBrowsing();trace.clear();lib.close();assert(trace=="wp"&&lib.count()==0);lib.close();assert(trace=="wp");
 }
 trace.clear();{L lib;lib.beginBrowsing();lib.load();}assert(trace=="PAWawp");
 trace.clear();uploadOk=false;agentOk=false;
 {L lib;assert(lib.beginBrowsing(true)==B::Pending && !lib.browsing());
  assert(lib.load()==L::LoadResult::ReadError && trace=="R");
  assert(lib.beginBrowsing(true)==B::Pending && trace=="R");
  uploadOk=true;assert(lib.beginBrowsing(true)==B::Pending && !lib.browsing());
  assert(lib.load()==L::LoadResult::ReadError && trace=="RA");
  agentOk=true;assert(lib.beginBrowsing(true)==B::Ready && lib.browsing() && trace=="RAWa");
  lib.close();assert(trace=="RAWawp");
 }
 trace.clear();agentOk=false;
 {L lib;assert(lib.beginBrowsing(true)==B::Pending);lib.close();assert(trace=="RAawp");}
 trace.clear();uploadOk=false;
 {L lib;assert(lib.beginBrowsing(true)==B::Pending);lib.close();assert(trace=="Rwp");}
}
''')
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/services/photo_library.cpp'),str(ROOT/'src/services/peripheral_reservation.cpp'),str(root/'main.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
print('PASS Photos reservation rollback, catalog lifetime, SD-before-queue deletion, sync handoff and destructor cleanup under sanitizers')
