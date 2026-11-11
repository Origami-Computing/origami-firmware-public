"""Compile the production HTTP sink/download function with a faultable transport."""
from pathlib import Path
import subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/widgets/sync.cpp').read_text()
body=source[source.index('class Body :'):source.index('// Fetch the raw manifest')]
helper=source[source.index('static services::custom_apps::Result publicGet('):source.index('static bool safeRemoteId')]
download=source[source.index('services::custom_apps::Result download('):source.index('int syncFromBackend(')]
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)
 (p/'test.cpp').write_text(r'''
#include "core/app_store_qr.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>
#include <cstdint>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
unsigned now=0;unsigned millis(){return now;}
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_INTERNAL=4;
size_t heap_caps_get_free_size(int){return 32768;}size_t heap_caps_get_largest_free_block(int){return 16384;}
void* heap_caps_malloc(size_t n,int){return malloc(n);}void heap_caps_free(void* p){free(p);}
struct Stream {virtual ~Stream()=default;virtual size_t write(uint8_t)=0;virtual size_t write(const uint8_t*,size_t)=0;virtual int available()=0;virtual int read()=0;virtual int peek()=0;virtual void flush()=0;};
bool stopped=false,cancelled=false,rootBundle=false;int response=200,declared=-1,requests=0;std::string bytes="KAPP test";std::vector<std::string> replies;std::vector<int> statuses;int streamFailures=0;bool cancelDuringYield=false;
struct {template<class... T>void printf(const char*,T...) {}} Serial;
constexpr int WL_CONNECTED=1,HTTPC_DISABLE_FOLLOW_REDIRECTS=0;
struct {int status(){return WL_CONNECTED;}} WiFi;
int liveSecure=0,yields=0;
void yieldBetweenRequests(){assert(liveSecure==0);++yields;if(cancelDuringYield)cancelled=true;}
struct WiFiClientSecure {WiFiClientSecure(){assert(liveSecure==0);++liveSecure;}~WiFiClientSecure(){--liveSecure;}void setHandshakeTimeout(int){}void stop(){stopped=true;}};
struct HTTPClient {
 void addHeader(const char* name,const char* value){assert(!strcmp(name,"X-Kami-App-Protocol")&&!strcmp(value,"2"));}
 void setConnectTimeout(int){}void setTimeout(int){}void setFollowRedirects(int mode){assert(mode==HTTPC_DISABLE_FOLLOW_REDIRECTS);}
 bool begin(WiFiClientSecure&,const char* url){assert(rootBundle);return strncmp(url,"https://",8)==0;}
 int GET(){++requests;int status=response;if(!statuses.empty()){status=statuses.front();statuses.erase(statuses.begin());}if(!replies.empty()){bytes=replies.front();replies.erase(replies.begin());}return status;}int getSize(){return declared;}void end(){}
 int writeToStream(Stream* sink){
  if(streamFailures>0){--streamFailures;sink->write(reinterpret_cast<const uint8_t*>(bytes.data()),3);return -1;}
  for(size_t i=0;i<bytes.size();i+=7){size_t n=std::min(size_t(7),bytes.size()-i);
   if(sink->write(reinterpret_cast<const uint8_t*>(bytes.data()+i),n)!=n)return -1;
  }return bytes.size();
 }
};
namespace widgets {namespace storage {std::vector<std::pair<size_t,size_t>> updates;void reportDownloadProgress(size_t n,size_t total){updates.push_back({n,total});}}}
namespace services {
namespace tls {void useRootBundle(WiFiClientSecure&){rootBundle=true;}}
namespace custom_apps {
using Cancel=bool(*)();constexpr size_t kPackageBytes=42000;
enum class Result{Ok,Invalid,Cancelled,Unavailable,Io,Corrupt,NoMemory,Network};
struct Package{uint8_t* bytes=nullptr;size_t size=0;void reset(){free(bytes);bytes=nullptr;size=0;}~Package(){reset();}};
struct View{};
Result inspect(const uint8_t* p,size_t n,View&){return n>=4&&!memcmp(p,"KAPP",4)?Result::Ok:Result::Invalid;}
}}
int mbedtls_sha256_ret(const uint8_t* p,size_t n,uint8_t* out,int){
#ifdef __APPLE__
 CC_SHA256(p,n,out);
#else
 SHA256(p,n,out);
#endif
 return 0;
}
bool cancel(){return cancelled;}
namespace widgets {namespace sync {
'''+body+helper+download+r'''
}}
int main(){
 using namespace services::custom_apps;
 char identity[192];
 const char* base=KAMI_APP_STORE_URL "/api/app-store/builder-200001/package.kapp";
 std::string pinned=std::string(base)+"?v=11111111-1111-4111-8111-111111111111";
 assert(core::app_store::identityUrl(pinned.c_str(),identity)&&!strcmp(identity,base));
 for(const std::string candidate:{std::string(base)+"?v=bad",pinned+"&extra=1",std::string("https://other.example/app?v=11111111-1111-4111-8111-111111111111")}){
  assert(core::app_store::identityUrl(candidate.c_str(),identity)&&candidate==identity);
 }
 assert(!core::app_store::identityUrl(nullptr,identity));

 core::app_store::Download request;strcpy(request.url,"https://apps.example.com/test.kapp");
 uint8_t hash[32];mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>(bytes.data()),bytes.size(),hash,0);
 for(size_t i=0;i<32;++i)snprintf(request.sha256+i*2,3,"%02x",hash[i]);
 core::app_store::Download shortCode;
 assert(core::app_store::parse("100012",shortCode)&&!strcmp(shortCode.code,"100012"));
 assert(!core::app_store::parse("10001x",shortCode));
 assert(!core::app_store::parse("1000120",shortCode));
 Package out;assert(widgets::sync::download(request,out,cancel)==Result::Ok&&out.size==bytes.size()&&stopped);
 out.reset();
 const std::string original=bytes;bytes="100012";
 int before=requests;
 assert(widgets::sync::download(shortCode,out,cancel)==Result::Invalid&&requests==before+1);
 bytes=std::string("origami-app:https://apps.example.com/test.kapp#sha256=")+request.sha256;
 bytes.insert(20,1,'\0');
 assert(widgets::sync::download(shortCode,out,cancel)==Result::Invalid);
 bytes=original;
 replies={std::string("origami-app:https://apps.example.com/test.kapp#sha256=")+request.sha256,original};
 before=requests;
 assert(widgets::sync::download(shortCode,out,cancel)==Result::Ok&&requests==before+2&&out.size==original.size());
 assert(!shortCode.code[0]&&!strcmp(shortCode.url,request.url)&&yields==1);
 out.reset();before=requests;statuses={-1,200};
 assert(widgets::sync::download(request,out,cancel)==Result::Ok&&requests==before+2);
 out.reset();before=requests;streamFailures=1;
 assert(widgets::sync::download(request,out,cancel)==Result::Ok&&requests==before+2&&out.size==original.size());
 out.reset();before=requests;statuses={-1,200};cancelDuringYield=true;
 assert(widgets::sync::download(request,out,cancel)==Result::Cancelled&&requests==before+1&&!out.bytes);
 cancelDuringYield=false;cancelled=false;statuses.clear();
 out.reset();before=requests;statuses={503,503};
 assert(widgets::sync::download(request,out,cancel)==Result::Network&&requests==before+2&&!out.bytes);
 out.reset();bytes.back()='!';assert(widgets::sync::download(request,out,cancel)==Result::Corrupt&&!out.bytes);
 cancelled=true;int previous=requests;assert(widgets::sync::download(request,out,cancel)==Result::Cancelled&&requests==previous);cancelled=false;
 response=302;assert(widgets::sync::download(request,out,cancel)==Result::Network);response=200;
 declared=42001;assert(widgets::sync::download(request,out,cancel)==Result::Invalid);declared=-1;
 bytes=std::string(42001,'x');assert(widgets::sync::download(request,out,cancel)==Result::Invalid);
 strcpy(request.url,"http://apps.example.com/test.kapp");previous=requests;
 assert(widgets::sync::download(request,out,cancel)==Result::Invalid&&requests==previous);
 widgets::sync::Body bounded(10,cancel);uint8_t chunk[8]={};assert(bounded.write(chunk,8)==8);
 assert(bounded.write(chunk,8)==0&&bounded.size()==8);now=30001;assert(bounded.write(chunk,1)==0);
 bytes=original;declared=original.size();strcpy(request.url,"https://apps.example.com/test.kapp");
 widgets::storage::updates.clear();out.reset();assert(widgets::sync::download(request,out,cancel)==Result::Ok);
 assert(widgets::storage::updates.back()==std::make_pair(original.size(),original.size()));
 assert(std::any_of(widgets::storage::updates.begin(),widgets::storage::updates.end(),[](auto p){return p.first>0&&p.first<p.second;}));
 declared=-1;widgets::storage::updates.clear();out.reset();assert(widgets::sync::download(request,out,cancel)==Result::Ok);
 assert(widgets::storage::updates.back().second==0);
 puts("PASS production downloader: TLS, bounded retry, partial-response reset, cancellation between attempts, hashes, size cap and deadline");
}
''')
 cmd=['c++','-std=c++17','-fsanitize=address,undefined','-Wno-deprecated-declarations','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')]
 if sys.platform!='darwin':cmd.append('-lcrypto')
 subprocess.run(cmd,check=True);subprocess.run([str(p/'test')],check=True)
