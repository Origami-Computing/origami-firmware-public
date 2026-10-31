"""Exercise production USB upload mutation ownership through finish/abort/failure."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1];s=(ROOT/'src/tooling/dev_serial_input.cpp').read_text()
def block(marker):
 a=s.index(marker);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
code=r'''
#include <memory>
#include <new>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
bool held=false,locked=false,admit=true,initOk=true,dirOk=true,openOk=true;
int acquires=0,releases=0,opens=0,removes=0,closes=0;
class UploadQueue{public:class FileMutation{bool acquired;public:FileMutation(UploadQueue&,const char*,bool remove):acquired(admit){assert(!remove&&!held);++acquires;if(acquired)held=true;}~FileMutation(){if(acquired){assert(!locked&&held);held=false;++releases;}}bool ready()const{return acquired;}};};
namespace services{namespace uploads{UploadQueue q;bool ensureInit(){return initOk;}UploadQueue& queue(){return q;}}}
namespace hal{namespace sd{bool mounted(){return true;}}namespace bus{void lockSpi(){assert(held&&!locked);locked=true;}void unlockSpi(){assert(locked);locked=false;}}}
struct File{bool alive=false;explicit operator bool()const{return alive;}void close(){assert(held&&locked);alive=false;++closes;}};
constexpr int FILE_WRITE=1;constexpr size_t kSdPathCap=96;
struct {bool remove(const char*){assert(held&&locked);++removes;return true;}File open(const char*,int){assert(held&&locked);++opens;File f;f.alive=openOk;return f;}}SD;
struct {void println(const char*){}template<class...T>void printf(const char*,T...){}}Serial;
bool validSdPath(const char* p){return p&&p[0]=='/';}
bool ensureParentDirs(const char*){assert(held);return dirOk;}
void copyLine(char* out,size_t cap,const char* text){snprintf(out,cap,"%s",text);}
'''+block('struct SdUploadState {')+';\nstatic SdUploadState s_sdUpload;\n'+'\n'.join(block(m) for m in ['static void abortSdUpload(', 'static void finishSdUpload(', 'static bool startSdUpload('])+r'''
void reset(){assert(!held&&!locked&&!s_sdUpload.active&&!s_sdUpload.mutation);admit=initOk=dirOk=openOk=true;acquires=releases=opens=removes=closes=0;}
int main(){
 for(int fault=0;fault<4;++fault){reset();if(fault==0)admit=false;if(fault==1)initOk=false;if(fault==2)dirOk=false;if(fault==3)openOk=false;
  assert(startSdUpload("/videos/test.avi",1,0));assert(!s_sdUpload.active&&!held&&!s_sdUpload.mutation);
  if(fault<3)assert(!opens&&!removes);
 }
 reset();assert(startSdUpload("/videos/test.avi",1,0));assert(held&&s_sdUpload.active&&s_sdUpload.mutation&&!releases);
 s_sdUpload.received=1;finishSdUpload();assert(!held&&!s_sdUpload.active&&releases==1&&closes==1);
 reset();startSdUpload("/videos/test.avi",1,123);finishSdUpload();assert(!held&&releases==1&&removes==2);
 reset();startSdUpload("/videos/test.avi",1,0);abortSdUpload("timeout");assert(!held&&releases==1&&closes==1&&removes==2);
 reset();puts("PASS actual USB upload mutation lifetime: admission, allocation owner, open/mkdir failure, completion, CRC failure and abort");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'main.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
