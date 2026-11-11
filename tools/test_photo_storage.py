"""Compile real photo storage against faultable SD and upload queue operations."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('services/uploads.h',r'''#pragma once
#include <stddef.h>
#include <stdint.h>
struct Queue {uint32_t generatePath(const char*,const char*,char*,size_t);bool enqueue(const char*,const char*,const char*,bool,uint32_t);};
namespace services {namespace uploads {bool ensureInit();Queue& queue();}}
''')
    write('hal/hal_sd.h',r'''#pragma once
#include <stddef.h>
#include <stdint.h>
namespace hal {namespace sd {bool ensureDir(const char*);bool mounted();bool exists(const char*);bool writeFile(const char*,const uint8_t*,size_t);bool removeFile(const char*);}}
''')
    write('core/board_time.h', '#pragma once\n#include <time.h>\nnamespace core {namespace time {bool isSynced();}}\n')
    write('main.cpp',r'''
#include "services/photo_storage.h"
#include "services/uploads.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
bool synced=true; uint32_t sentCapture=0;
namespace core {namespace time {bool isSynced(){return synced;}}}
bool mounted=true,dirOk=true,initOk=true,writeOk=true,queueOk=true;
int initializes=0,directories=0,writes=0,deletes=0,queued=0;
uint32_t seq=42;std::string generated="/upload/IMG_00042.jpg";
std::map<std::string,std::string> files;Queue q;
namespace services {namespace uploads {bool ensureInit(){++initializes;return initOk;}Queue& queue(){return q;}}}
uint32_t Queue::generatePath(const char* prefix,const char* ext,char*out,size_t cap){assert(!strcmp(prefix,"IMG")&&!strcmp(ext,".jpg"));snprintf(out,cap,"%s",generated.c_str());return seq;}
bool Queue::enqueue(const char* kind,const char* path,const char* mime,bool remove,uint32_t capture){sentCapture=capture;++queued;assert(!strcmp(kind,"image")&&!strcmp(mime,"image/jpeg")&&!remove);assert(files.count(path));return queueOk;}
namespace hal {namespace sd {
bool mounted(){return ::mounted;}
bool ensureDir(const char*p){++directories;assert(!strcmp(p,"/photos"));return dirOk;}
bool exists(const char*p){return files.count(p);}
bool writeFile(const char*p,const uint8_t*data,size_t n){++writes;files[p]=writeOk?std::string((const char*)data,n):"partial";return writeOk;}
bool removeFile(const char*p){++deletes;return files.erase(p);}
}}
int main(){namespace photos=services::photos;
assert(photos::initialize()&&initializes==1&&directories==1);
initOk=false;assert(photos::initialize()&&directories==2);dirOk=false;assert(!photos::initialize());
assert(photos::available());mounted=false;assert(!photos::available());
auto target=photos::prepare();assert(target.valid()&&target.sequence==42&&!strcmp(target.path,"/photos/IMG_00042.jpg"));
assert(target.capturedAtUnix==0);photos::markCaptured(target);assert(target.capturedAtUnix>1700000000);
const auto captured=target.capturedAtUnix;
synced=false; // Losing time later must not overwrite the original capture.
uint8_t jpeg[]={0xff,0xd8,1,0xff,0xd9};
assert(photos::write(target,jpeg,sizeof(jpeg)));assert(files[target.path].size()==sizeof(jpeg));
assert(photos::enqueue(target)&&queued==1&&sentCapture==captured);
queueOk=false;assert(!photos::enqueue(target)&&files.count(target.path)&&sentCapture==captured);
auto unknown=target;photos::markCaptured(unknown);assert(unknown.capturedAtUnix==0);
assert(!photos::prepare().valid());assert(writes==1&&deletes==0); // collision leaves existing photo untouched
files.clear();writeOk=false;assert(!photos::write(target,jpeg,sizeof(jpeg)));assert(files.empty()&&deletes==1);
seq=0;auto invalid=photos::prepare();assert(!invalid.valid());int beforeWrites=writes,beforeDeletes=deletes,beforeQueue=queued;
assert(!photos::write(invalid,jpeg,sizeof(jpeg))&&!photos::enqueue(invalid));assert(writes==beforeWrites&&deletes==beforeDeletes&&queued==beforeQueue);
seq=43;generated="/upload/";assert(!photos::prepare().valid());
generated=std::string(63,'x');assert(!photos::prepare().valid());
generated="IMG_00043.jpg";target=photos::prepare();assert(target.valid()&&!strcmp(target.path,"/photos/IMG_00043.jpg"));
}
''')
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/services/photo_storage.cpp'),str(root/'main.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
print('PASS photo naming/metadata, collisions, path bounds, partial-write cleanup and queue-failure retention under sanitizers')
