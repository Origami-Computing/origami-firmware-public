"""Exercise the actual upload body with offline transport and SD fault injection."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/services/device_fs.cpp').read_text()
start = source.index('void doPush(')
end = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
body = source[start:end]
stub = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <algorithm>
struct String : std::string {
 using std::string::string;
 String(const std::string& s):std::string(s){}
 String(unsigned n):std::string(std::to_string(n)){}
 int indexOf(char c) const {auto n=find(c);return n==npos?-1:int(n);}
 String substring(unsigned n)const{return substr(n);}
 int toInt()const{return std::atoi(c_str());}
};
enum Fault {None, Excluded, Active, PauseTimeout, StopBefore, StopDuringPause,
 BadUrl, Missing, Connect, ConnectOnce, CancelConnect, Allocation, Read, Write, CancelWrite, Http500, Http401,
 TextBefore, TextDuringPause, TextDuringWrite, SlowResponse};
Fault fault;
bool held=false, stopping=false, opened=false, locked=false, textPending=false;
unsigned connects=0, releases=0, written=0, reports=0, clears=0;
unsigned clockMs=0, lastFeed=0;
constexpr unsigned kChunk=1024, MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2, MALLOC_CAP_INTERNAL=4;
constexpr int FILE_READ=0;
constexpr const char* DEVICE_FS_UPLOAD_PATH="/fixture";
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(unsigned ms){clockMs+=ms;assert(clockMs-lastFeed<15000);}
unsigned millis(){return clockMs;}
void setStatus(const char*,const char*,unsigned,unsigned,bool=true,bool=false,bool=true){}
bool isExcluded(const String&){return fault==Excluded;}
void reportResult(const char*,bool,unsigned,const char*){++reports;}
bool ok2xx(int n){return n>=200&&n<300;}
String urlEncode(const String& s){return s;}
bool parseBackend(const char*,char* host,size_t,uint16_t* port,bool* tls,char* base,size_t){
 strcpy(host,"fixture.invalid");*port=443;*tls=true;*base=0;return fault!=BadUrl;
}
void* heap_caps_malloc(size_t n,unsigned){return fault==Allocation?nullptr:malloc(n);}
unsigned heap_caps_get_free_size(unsigned){return 12345;}
unsigned heap_caps_get_largest_free_block(unsigned){return 12345;}
namespace core {namespace secrets {
 struct Request {struct {char url[32]="https://fixture.invalid";} backend;char token[16]="synthetic";};
 Request backendRequest(){assert(held);return {};}
 void setDeviceTokenFor(const Request&,const char*){++clears;}
}namespace device {const char* id(){return "fixture";}}namespace watchdog {void feed(){lastFeed=clockMs;}}
 namespace device_agent_text {bool hasPendingRequest(){return textPending;}}}
namespace services {namespace net_poll {bool stopRequested(){return stopping;}}
 namespace agent_coordination {
 bool isSessionActive(){return fault==Active;}
 struct TransportPause {
  bool own=false;
  bool acquire(unsigned){assert(!held);if(fault==PauseTimeout)return false;own=held=true;if(fault==StopDuringPause)stopping=true;if(fault==TextDuringPause)textPending=true;return true;}
  ~TransportPause(){if(own){assert(!opened);held=false;++releases;}}
 };
 }
 namespace tls {template<class T>void useRootBundle(T&) {}}
}
namespace hal {namespace bus {void lockSpi(){assert(!locked);locked=true;}void unlockSpi(){assert(locked);locked=false;}}}
struct File {
 unsigned pos=0;
 operator bool()const{return fault!=Missing;}
 size_t size()const{return 2500;}
 unsigned getLastWrite()const{return 0;}
 size_t read(void*,size_t n){assert(locked);if(fault==Read)return 0;pos+=n;return n;}
 void close(){assert(locked);}
};
struct SDType {File open(const String&,int){assert(locked&&held);return {};}} SD;
struct WiFiClient {
 bool live=false;
 virtual ~WiFiClient(){if(live){assert(held);opened=false;}}
 bool connect(const char*,unsigned,unsigned){assert(held&&!locked);++connects;if(fault==CancelConnect){stopping=true;return false;}if(fault==Connect||(fault==ConnectOnce&&connects==1))return false;opened=live=true;return true;}
 void setNoDelay(bool){}
 template<class... A> void printf(const char*,A...) {assert(held);}
 void print(const char*){assert(held);}
 size_t write(const uint8_t*,size_t n){assert(held&&!locked);if(fault==Write)return 0;if(fault==CancelWrite)stopping=true;if(fault==TextDuringWrite)textPending=true;written+=n;return n;}
 void stop(){assert(held&&!locked);live=opened=false;}
 void flush(){}
 bool connected(){return live;}
 bool available(){return fault!=SlowResponse;}
 String readStringUntil(char){if(fault==SlowResponse)return "";return fault==Http500?"HTTP/1.1 500":fault==Http401?"HTTP/1.1 401":"HTTP/1.1 200";}
};
struct WiFiClientSecure:WiFiClient {void setHandshakeTimeout(unsigned){}int lastError(void*,int){return -1;}};
'''
checks = r'''
int main(){
 for(int f=None;f<=SlowResponse;++f){
  fault=static_cast<Fault>(f);held=opened=locked=false;stopping=fault==StopBefore;
  textPending=fault==TextBefore;
  connects=releases=written=reports=clears=0;clockMs=lastFeed=0;
  doPush("/synthetic.bin","synthetic-op");
  assert(!held&&!opened&&!locked);
  if(fault==Excluded||fault==Active||fault==PauseTimeout||fault==StopBefore||fault==TextBefore)assert(releases==0&&connects==0);
  else assert(releases==1);
  if(fault==StopDuringPause||fault==BadUrl||fault==Missing||fault==TextDuringPause)assert(connects==0);
  if(fault==None||fault==Http500||fault==Http401)assert(written==2500);
  if(fault==ConnectOnce)assert(connects==2&&written==2500);
  if(fault==Connect)assert(connects==2&&!written);
  if(fault==CancelConnect)assert(connects==1&&!written);
  if(fault==CancelWrite||fault==TextDuringWrite)assert(written==1024);
  if(fault>=TextBefore)assert(reports==0);
  if(fault==SlowResponse)assert(clockMs>=15000&&clockMs<15030&&written==2500);
  assert(clears==(fault==Http401?1u:0u));
  if(fault==Connect||fault==Allocation||fault==Read||fault==Write||fault==CancelWrite||fault==Http500)assert(reports==0);
 }
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    cpp = path / 'test.cpp'
    cpp.write_text(stub + body + checks)
    executable = path / 'test'
    subprocess.run(['c++', '-std=c++17', '-fsanitize=address,undefined',
                    str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=10)
print('PASS actual file upload: foreground admission, pause failure, cancellation, SD/TLS/allocation errors, cleanup order, bounded slow-server watchdog wait and HTTP results')
