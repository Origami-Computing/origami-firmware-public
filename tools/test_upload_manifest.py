"""Compile actual manifest methods against ArduinoJson 7.4.3 and a faulting SD.

Set ARDUINOJSON_SRC to a matching source directory, or use the project's installed
PlatformIO dependency. No device/network access. Tests cuts at every mutation,
short writes, operation failures, corrupt primary recovery and durable counters.
"""
from pathlib import Path
import os, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
json_src=Path(os.environ.get('ARDUINOJSON_SRC',ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src'))
if not (json_src/'ArduinoJson.h').exists():
    raise SystemExit('Install the project ArduinoJson dependency, or set ARDUINOJSON_SRC (7.4.3)')
source=(ROOT/'src/upload_queue.cpp').read_text();header=(ROOT/'include/upload_queue.h').read_text()
def block(text, marker):
    a=text.index(marker);b=text.index('{',a)+1;depth=1
    while depth:depth+=(text[b]=='{')-(text[b]=='}');b+=1
    return text[a:b]
code=r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#define UPLOAD_QUEUE_MAX_ITEMS 20
#define UPLOAD_QUEUE_DIR "/upload_queue"
#define UPLOAD_QUEUE_MANIFEST "/upload_queue/manifest.json"
#define FILE_READ 0
#define FILE_WRITE 1
using Disk=std::map<std::string,std::string>;
Disk disk;std::vector<Disk> cuts;int operations=0,failAt=-1;bool snapshot=false,shortWrite=false;
bool fail(){return ++operations==failAt;}
void changed(){if(snapshot)cuts.push_back(disk);}
struct File {
 std::string path;size_t pos=0;bool valid=false;
 explicit operator bool()const{return valid;}
 int read(){auto& s=disk[path];return pos<s.size()?(unsigned char)s[pos++]:-1;}
 size_t readBytes(char* p,size_t n){size_t k=0;for(;k<n;++k){int c=read();if(c<0)break;p[k]=c;}return k;}
 size_t write(uint8_t c){return write(&c,1);}
 size_t write(const uint8_t* p,size_t n){if(fail())return 0;if(shortWrite)n=0;disk[path].append((const char*)p,n);changed();return n;}
 void flush(){changed();}void close(){changed();}size_t size()const{return disk[path].size();}
};
struct {
 bool exists(const char* p){return disk.count(p);}
 bool remove(const char* p){if(fail())return false;bool ok=disk.erase(p);changed();return ok;}
 bool rename(const char* a,const char* b){if(fail()||!disk.count(a)||disk.count(b))return false;disk[b]=disk[a];disk.erase(a);changed();return true;}
 File open(const char* p,int mode){if(fail())return {};if(mode==FILE_READ&&!disk.count(p))return {};if(mode==FILE_WRITE){disk[p];changed();}return {p,0,true};}
}SD;
struct {template<class... T>void printf(const char*,T...){}void println(const char*){}}Serial;
enum QueueItemStatus:uint8_t {QSTATUS_PENDING,QSTATUS_UPLOADING,QSTATUS_COMPLETED,QSTATUS_FAILED};
'''+block(header,'struct QueueItem {')+';\n'+r'''
struct MutexGuard {explicit MutexGuard(int){}};
class UploadQueue {public:
 int m_mutex=0;
 uint32_t generatePath(const char*,const char*,char*,size_t);
 QueueItem m_items[20]{};int m_count=0;uint32_t m_nextImgId=1,m_nextAudId=1;
 bool loadManifestUnlocked();bool loadManifestFileUnlocked(const char*);bool saveManifestUnlocked();
};
'''+'\n'.join(block(source,m) for m in ['bool UploadQueue::loadManifestUnlocked(', 'bool UploadQueue::loadManifestFileUnlocked(', 'bool UploadQueue::saveManifestUnlocked(', 'uint32_t UploadQueue::generatePath('])+r'''
void resetFaults(){failAt=-1;operations=0;snapshot=false;shortWrite=false;}
void checkRecovery(const Disk& state){disk=state;resetFaults();UploadQueue boot;assert(boot.loadManifestUnlocked());
 assert(boot.m_count==1);assert(boot.m_nextImgId==164||boot.m_nextImgId==165);assert(boot.m_nextAudId==12);
 assert(boot.m_items[0].status==QSTATUS_PENDING);assert(!strcmp(boot.m_items[0].id,"test-pending.jpg"));
 assert(boot.m_items[0].attempts<=1);assert(boot.m_items[0].attemptsThisTurn==0);assert(boot.m_items[0].nextEligibleAt==0);
 // A recovered copy must survive the following save too.
 assert(boot.saveManifestUnlocked());UploadQueue second;assert(second.loadManifestUnlocked());assert(second.m_count==1&&second.m_nextImgId==boot.m_nextImgId);
}
int main(){
 UploadQueue q;q.m_count=1;q.m_nextImgId=164;q.m_nextAudId=12;auto& item=q.m_items[0];
 strcpy(item.id,"test-pending.jpg");strcpy(item.filePath,"/upload_queue/test-pending.jpg");strcpy(item.type,"image");strcpy(item.mimeType,"image/jpeg");strcpy(item.fileName,item.id);
 item.status=QSTATUS_UPLOADING;disk[item.filePath]="media";assert(q.saveManifestUnlocked());
 // Include a prior backup and stale scratch so their cleanup is fault-tested.
 disk[UPLOAD_QUEUE_MANIFEST ".bak"]=R"({"nextImgId":163,"nextAudId":12,"items":[]})";disk[UPLOAD_QUEUE_MANIFEST ".tmp"]="partial";
 Disk initial=disk;q.m_nextImgId=165;item.attempts=1;snapshot=true;operations=0;assert(q.saveManifestUnlocked());
 int saveOps=operations;auto snapshots=cuts;for(auto& state:snapshots)checkRecovery(state);
 for(int failure=1;failure<=saveOps;++failure){disk=initial;resetFaults();failAt=failure;q.saveManifestUnlocked();Disk failed=disk;checkRecovery(failed);}
 disk=initial;resetFaults();shortWrite=true;assert(!q.saveManifestUnlocked());Disk shortState=disk;checkRecovery(shortState);
 disk=initial;disk[UPLOAD_QUEUE_MANIFEST ".bak"]=disk[UPLOAD_QUEUE_MANIFEST];disk[UPLOAD_QUEUE_MANIFEST]="{}";disk.erase(UPLOAD_QUEUE_MANIFEST ".tmp");checkRecovery(disk);
 disk=initial;disk[UPLOAD_QUEUE_MANIFEST ".bak"]=disk[UPLOAD_QUEUE_MANIFEST];disk.erase(UPLOAD_QUEUE_MANIFEST);disk[UPLOAD_QUEUE_MANIFEST ".tmp"]="{partial";checkRecovery(disk);
 disk.clear();resetFaults();UploadQueue fresh;assert(fresh.loadManifestUnlocked()&&fresh.m_count==0);
 disk[UPLOAD_QUEUE_MANIFEST]="{}";UploadQueue corrupt;assert(!corrupt.loadManifestUnlocked());assert(disk[UPLOAD_QUEUE_MANIFEST]=="{}");
 disk=initial;resetFaults();UploadQueue reserve;assert(reserve.loadManifestUnlocked());
 char path[96]{};shortWrite=true;assert(reserve.generatePath("IMG",".jpg",path,sizeof(path))==0&&path[0]=='\0');
 shortWrite=false;assert(reserve.generatePath("IMG",".jpg",path,sizeof(path))==165);assert(strstr(path,"IMG_00165.jpg"));
 UploadQueue reserved;assert(reserved.loadManifestUnlocked()&&reserved.m_nextImgId==166);
 resetFaults();disk.clear();strcpy(item.type,"video");item.deleteAfterUpload=true;disk[item.filePath]="media";
 assert(q.saveManifestUnlocked());UploadQueue videoBoot;assert(videoBoot.loadManifestUnlocked());
 assert(videoBoot.m_count==1&&!videoBoot.m_items[0].deleteAfterUpload);
 printf("PASS actual manifest: %zu cut boundaries, %d I/O failures, short writes, recovery and counters\n",snapshots.size(),saveOps);
}
'''
with tempfile.TemporaryDirectory(prefix='upload-manifest-') as tmp:
    p=Path(tmp);(p/'test.cpp').write_text(code)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-fsanitize=address,undefined','-g','-I',str(json_src),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
    # Frozen regression control: unlinking the only current copy before writing
    # scratch must fail the same recovery assertions (an older backup is stale).
    unsafe=code.replace('// FILE_WRITE appends', 'SD.remove(UPLOAD_QUEUE_MANIFEST); SD.remove(UPLOAD_QUEUE_MANIFEST ".bak");\n    // FILE_WRITE appends')
    assert unsafe != code
    (p/'unsafe.cpp').write_text(unsafe)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-I',str(json_src),str(p/'unsafe.cpp'),'-o',str(p/'unsafe')],check=True)
    result=subprocess.run([str(p/'unsafe')],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    assert result.returncode != 0, 'Unsafe unlink-before-write unexpectedly passed'
    print('PASS manifest negative control rejects unlink-before-write')
