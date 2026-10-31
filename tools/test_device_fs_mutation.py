"""Exercise actual cloud pull/delete entry points with queue mutation admission faults."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1];s=(ROOT/'src/services/device_fs.cpp').read_text()
def block(marker):
 a=s.index(marker);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
code=r'''
#include <cassert>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <initializer_list>
struct String:std::string{using std::string::string;String(const std::string& s):std::string(s){}bool startsWith(const char* s)const{return find(s)==0;}int lastIndexOf(char c)const{auto n=rfind(c);return n==npos?-1:int(n);}String substring(size_t a)const{return substr(a);}String substring(size_t a,size_t b)const{return substr(a,b-a);}};
bool initOk=true,admit=true,held=false,locked=false,openOk=true,renameOk=true,allocOk=true,stopping=false;
int acquires=0,releases=0,network=0,files=0,report=0,opened=0,httpStatus=200;bool reportedOk=false,removePending=false;
class UploadQueue{public:class FileMutation{bool acquired;public:FileMutation(UploadQueue&,const char*,bool remove):acquired(admit){++acquires;removePending=remove;if(acquired){assert(!held);held=true;}}~FileMutation(){if(acquired){assert(!locked);held=false;++releases;}}bool ready()const{return acquired;}};};
namespace services {namespace uploads {UploadQueue q;bool ensureInit(){return initOk;}UploadQueue& queue(){return q;}}namespace net_poll {bool stopRequested(){return stopping;}}}
namespace hal {namespace bus{void lockSpi(){assert(held&&!locked);locked=true;}void unlockSpi(){assert(locked);locked=false;}}}
bool isExcluded(const String& s){return s=="/excluded";}
void reportResult(const char*,bool ok,uint32_t,const char* =nullptr){++report;reportedOk=ok;}
template<class... T>void setStatus(T...){}
bool ok2xx(int code){return code>=200&&code<300;}
String urlEncode(const String& s){return s;}
struct Field{const char* operator|(const char* s)const{return s;}};
struct JsonDocument{explicit JsonDocument(void*){}Field operator[](const char*){return {};}};
int s_jsonMemory;int httpGetJson(const char*,JsonDocument&){assert(held);++network;return 200;}
constexpr const char* DEVICE_FS_DOWNLOAD_PATH="/download";
struct WiFiClient{size_t pos=0;size_t available(){return 3-pos;}int readBytes(uint8_t* b,size_t n){n=std::min(n,3-pos);memset(b,42,n);pos+=n;return n;}};
struct WiFiClientSecure:WiFiClient{void setHandshakeTimeout(unsigned){}};
namespace services{namespace tls{void useRootBundle(WiFiClientSecure&){assert(held);}}}
struct HTTPClient{WiFiClient stream;void setConnectTimeout(int){}void setTimeout(int){}bool begin(WiFiClient&,const String&){assert(held);++network;return true;}void setFollowRedirects(int){}void useHTTP10(bool){}int GET(){return httpStatus;}void end(){}WiFiClient* getStreamPtr(){return &stream;}int getSize(){return 3;}bool connected(){return stream.pos<3;}};
constexpr int HTTPC_STRICT_FOLLOW_REDIRECTS=1,FILE_WRITE=1,FILE_READ=0,MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;constexpr size_t kChunk=1024;
struct File{bool alive=false;explicit operator bool()const{return alive;}size_t write(const uint8_t*,size_t n){assert(held&&locked);return n;}void flush(){assert(held&&locked);}void close(){assert(held&&locked);if(alive){alive=false;--opened;}}uint32_t getLastWrite(){return 1;}};
struct Disk{
 bool remove(const String&){assert(held&&locked);++files;return true;}
 bool exists(const String&){assert(held&&locked);++files;return true;}
 bool rename(const String&,const String&){assert(held&&locked);++files;return renameOk;}
 File open(const String&,int){assert(held&&locked);++files;File f;f.alive=openOk;if(openOk)++opened;return f;}
}SD;
void ensureParentDirs(const String&){assert(held);}
void* heap_caps_malloc(size_t n,int){return allocOk?malloc(n):nullptr;}
uint32_t millis(){static uint32_t n=0;return ++n;}
void vTaskDelay(int){}int pdMS_TO_TICKS(int n){return n;}
namespace core{namespace watchdog{void feed(){}}}
bool removeRecursive(const String&){assert(held&&!locked);++files;return true;}
#define LOGW(...)
#define LOGI(...)
'''+block('void doPull(')+'\n'+block('void doDelete(')+r'''
void reset(){assert(!held&&!locked&&!opened);initOk=admit=openOk=renameOk=allocOk=true;stopping=false;acquires=releases=network=files=report=0;reportedOk=false;httpStatus=200;}
int main(){
 for(bool pull:{false,true})for(int fault=0;fault<3;++fault){
  reset();if(fault==0)initOk=false;if(fault==1)admit=false;
  const char* path=fault==2?"/excluded":"/videos/test.avi";
  if(pull)doPull(path,"https://fixture.invalid/file","id");else doDelete(path,"id");
  assert(!held&&!locked&&!opened&&!network&&!files&&report==1&&!reportedOk);
 }
 reset();doDelete("/videos/test.avi","id");assert(removePending&&reportedOk&&releases==1&&!held);
 for(int fault=0;fault<5;++fault){
  reset();if(fault==1)openOk=false;if(fault==2)allocOk=false;if(fault==3)stopping=true;if(fault==4)httpStatus=503;
  doPull("/videos/test.avi","https://fixture.invalid/file","id");
  assert(!removePending&&acquires==1&&releases==1&&!held&&!locked&&!opened);
  if(!fault)assert(reportedOk);else assert(!reportedOk);
 }
 reset();doPull("/videos/test.avi","","id");assert(releases==1&&!held&&!files&&!reportedOk);
 puts("PASS actual cloud file mutation: refusal before I/O, retained lease through download/delete and cleanup on network/storage/allocation/cancellation faults");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'main.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
