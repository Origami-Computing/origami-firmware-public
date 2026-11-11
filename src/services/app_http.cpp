#include "services/app_http.h"
#include "services/tls_trust.h"
#include "core/board_time.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <new>
namespace services { namespace app_http { namespace {
portMUX_TYPE mutex=portMUX_INITIALIZER_UNLOCKED;
struct Storage {Request request,work;Response response,scratch;uint32_t token=0,started=0;bool pending=false,done=false,owned=false;};
Storage* storage=nullptr; // Retained PSRAM; UI is the only allocator, worker borrows.
uint32_t workerToken=0;bool (*workerPause)()=nullptr;
bool tokenChar(char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||strchr("!#$%&'*+-.^_`|~",c);}
bool valid(const Request& r){
 size_t n=strnlen(r.url,sizeof(r.url));if(n==sizeof(r.url)||n<8||r.headerCount>kMaxHeaders||r.bodySize>kMaxRequest||r.timeoutMs<1000||r.timeoutMs>30000)return false;
 size_t start=!strncmp(r.url,"https://",8)?8:!strncmp(r.url,"http://",7)?7:0;if(!start)return false;
 const char* host=r.url+start;size_t authority=strcspn(host,"/?");if(!authority)return false;
 for(size_t i=0;i<n;++i)if((unsigned char)r.url[i]<=32||r.url[i]=='\\'||r.url[i]=='#')return false;
 for(size_t i=0;i<authority;++i)if(host[i]=='@')return false;
 const char* methods[]={"GET","POST","PUT","PATCH","DELETE","HEAD","OPTIONS"};bool method=false;
 if(strnlen(r.method,sizeof(r.method))==sizeof(r.method))return false;
 for(const char* m:methods)if(!strcmp(r.method,m))method=true;if(!method)return false;
 for(size_t i=0;i<r.headerCount;++i){const auto& h=r.headers[i];size_t a=strnlen(h.name,sizeof(h.name)),b=strnlen(h.value,sizeof(h.value));if(!a||a==sizeof(h.name)||b==sizeof(h.value))return false;
  for(size_t j=0;j<a;++j)if(!tokenChar(h.name[j]))return false;
  for(size_t j=0;j<b;++j)if((unsigned char)h.value[j]<32||h.value[j]==127)return false;
  if(!strcasecmp(h.name,"Host")||!strcasecmp(h.name,"Content-Length")||!strcasecmp(h.name,"Transfer-Encoding")||!strcasecmp(h.name,"Connection"))return false;
 }return true;
}
void expireLocked(){if(storage&&storage->pending&&uint32_t(millis()-storage->started)>=storage->request.timeoutMs){storage->pending=false;storage->done=true;storage->response.status=-3;storage->response.size=0;}}
bool stopped(){bool stale;portENTER_CRITICAL(&mutex);expireLocked();stale=!storage||!storage->pending||storage->token!=workerToken;portEXIT_CRITICAL(&mutex);return stale||(workerPause&&workerPause());}
class Sink:public Stream {public:
 size_t write(uint8_t c)override{return write(&c,1);}size_t write(const uint8_t* p,size_t n)override{auto& r=storage->scratch;if(stopped()||n>kMaxBody-r.size)return 0;memcpy(r.body+r.size,p,n);r.size+=n;r.body[r.size]=0;return n;}
 int available()override{return 0;}int read()override{return -1;}int peek()override{return -1;}void flush()override{}
};
template<class Client> void perform(Client& client){
 auto& r=storage->work;auto& out=storage->scratch;
 HTTPClient http;http.setConnectTimeout(4000);http.setTimeout(2000);http.setReuse(false);http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
 if(!http.begin(client,r.url)){out.status=-1;return;}
 http.setUserAgent("KamiApp/1");
 for(size_t i=0;i<r.headerCount;++i)http.addHeader(r.headers[i].name,r.headers[i].value);
 if(stopped()){http.end();return;}
 out.status=http.sendRequest(r.method,reinterpret_cast<uint8_t*>(r.body),r.bodySize);
 if(out.status>0&&strcmp(r.method,"HEAD")&&out.status!=204&&out.status!=304){
  Sink sink;const int expected=http.getSize();
  if(expected>int(kMaxBody))out.status=-2;
  else if(expected!=0) {const int received=http.writeToStream(&sink);if(received<0||size_t(received)!=out.size||(expected>=0&&size_t(expected)!=out.size))out.status=-2;}
 }
 http.end();client.stop();
}
} // namespace
uint32_t request(const Request& r){
 if(!valid(r))return 0;
 if(!storage){auto* s=static_cast<Storage*>(heap_caps_malloc(sizeof(Storage),MALLOC_CAP_SPIRAM));if(!s)return 0;new(s)Storage{};portENTER_CRITICAL(&mutex);storage=s;portEXIT_CRITICAL(&mutex);}
 portENTER_CRITICAL(&mutex);auto& s=*storage;if(!++s.token)++s.token;s.request=r;s.started=millis();s.pending=true;s.done=false;s.owned=true;s.response.status=0;s.response.size=0;auto token=s.token;portEXIT_CRITICAL(&mutex);return token;
}
void stop(uint32_t token){portENTER_CRITICAL(&mutex);if(storage&&storage->token==token){storage->pending=false;storage->done=false;storage->owned=false;}portEXIT_CRITICAL(&mutex);}
bool ready(uint32_t token){portENTER_CRITICAL(&mutex);expireLocked();bool r=storage&&storage->token==token&&storage->done;portEXIT_CRITICAL(&mutex);return r;}
bool take(uint32_t token,Response& r){portENTER_CRITICAL(&mutex);expireLocked();bool ok=storage&&storage->token==token&&storage->done;if(ok){r=storage->response;storage->done=false;}portEXIT_CRITICAL(&mutex);return ok;}
bool hasOwner(){portENTER_CRITICAL(&mutex);bool owned=storage&&storage->owned;portEXIT_CRITICAL(&mutex);return owned;}
bool isActive(){portENTER_CRITICAL(&mutex);expireLocked();bool active=storage&&storage->pending;portEXIT_CRITICAL(&mutex);return active;}
bool pollDue(){const bool synced=core::time::isSynced();portENTER_CRITICAL(&mutex);expireLocked();bool due=storage&&storage->pending&&(!strncmp(storage->request.url,"http://",7)||synced);portEXIT_CRITICAL(&mutex);return due;}
void poll(bool (*cancelled)()){
 if(!cancelled||cancelled()||!pollDue())return;
 portENTER_CRITICAL(&mutex);bool pending=storage&&storage->pending;if(pending){storage->work=storage->request;workerToken=storage->token;}portEXIT_CRITICAL(&mutex);if(!pending)return;
 workerPause=cancelled;auto& out=storage->scratch;out.status=-1;out.size=0;out.body[0]=0;
 if(!strncmp(storage->work.url,"https://",8)){WiFiClientSecure client;tls::useRootBundle(client);perform(client);}else{WiFiClient client;perform(client);}
 const bool paused=cancelled();portENTER_CRITICAL(&mutex);expireLocked();if(storage->token==workerToken&&storage->pending&&!paused){storage->response=out;storage->pending=false;storage->done=true;}portEXIT_CRITICAL(&mutex);workerPause=nullptr;
}
}}
