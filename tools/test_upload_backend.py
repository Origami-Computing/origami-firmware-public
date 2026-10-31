"""Run actual multipart request code with configuration changes at I/O boundaries.

Fake SD/socket I/O; no network, media, credentials or device mutation. The frozen
negative control demonstrates mixed inputs in the old borrowed-pointer path.
"""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--negative-control',action='store_true');p.add_argument('--tls-source-ref',help='Check a historical shared TLS configuration against current timeout acceptance');args=p.parse_args()
source=(ROOT/'src/upload_queue.cpp').read_text()
def block(source,marker):
 a=source.index(marker);b=source.index('{',a)+1;depth=1
 while depth:depth+=(source[b]=='{')-(source[b]=='}');b+=1
 return source[a:b]
if args.negative_control:
 fixture=ROOT/'test/fixtures/upload_backend';source=(fixture/'request.cpp').read_text()
 assert hashlib.sha256(source.encode()).hexdigest()==json.loads((fixture/'manifest.json').read_text())['sha256']
methods='\n'.join(block(source,m) for m in ['UploadQueue::UploadResult UploadQueue::doUpload(', 'bool UploadQueue::parseUrl('])
header=(ROOT/'include/upload_queue.h').read_text()
item=block(header,'struct QueueItem {')+';'
kind=block(header,'    enum class UploadResultKind')+';'
result=block(header,'    struct UploadResult {')+';'
code=r'''
#include "core/secrets.h"
#include "core/upload_receipt.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <functional>
#ifdef _WIN32
size_t strlcpy(char* dst,const char* src,size_t n){size_t len=strlen(src);if(n){size_t k=std::min(len,n-1);memcpy(dst,src,k);dst[k]=0;}return len;}
#endif
enum QueueItemStatus:uint8_t {QSTATUS_PENDING,QSTATUS_UPLOADING,QSTATUS_COMPLETED,QSTATUS_FAILED};
char liveUrl[192]="http://one.invalid:8794/base",liveKey[192]="key-one";
unsigned ticks=0, lastFeed=0, maxFeedGap=0;
namespace core {namespace secrets {
const char* backendUrl(){return liveUrl;}const char* apiKey(){return liveKey;}
BackendConfig backendConfig(){BackendConfig c;memcpy(c.url,liveUrl,192);memcpy(c.apiKey,liveKey,192);return c;}
}namespace watchdog {void feed(){maxFeedGap=std::max(maxFeedGap,ticks-lastFeed);lastFeed=ticks;}}}
bool initialTls=false; int connectCalls=0; int stallCount=0;
int mutation=0;void change(int where){if(mutation==where){strcpy(liveUrl,initialTls?"http://two.invalid/other":"https://two.invalid/other");strcpy(liveKey,"key-two");}}
namespace hal {namespace bus {void lockSpi(){}void unlockSpi(){}}}
unsigned millis(){return ++ticks;}void vTaskDelay(unsigned){}
#define pdMS_TO_TICKS(n) (n)
struct Log {template<class...T>void printf(const char*,T...){}}Serial;
std::string wire,host,response;unsigned port;bool secure,verified,stall=false;unsigned handshakeSeconds=120;
struct WiFiClientSecure;
namespace services {namespace tls {void useRootBundle(WiFiClientSecure&);}}
constexpr int FILE_READ=0;
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
void* heap_caps_malloc(size_t n,int caps){assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));return malloc(n);}
size_t fileBytes=13;unsigned writeDelay=0;
size_t shortWrite=0;int writeCalls=0,failWrite=0;bool earlyEof=false,fragmented=false,disconnected=false;
unsigned availableCalls=0;std::function<void()> onWrite;
struct File {
 size_t remaining=fileBytes;explicit operator bool()const{return true;}
 size_t size(){return fileBytes;}void close(){}
 size_t read(uint8_t* buf,size_t n){if(earlyEof)return 0;n=std::min(n,remaining);memset(buf,'x',n);remaining-=n;return n;}
};
struct FakeSD {File open(const char*,int){change(1);return {};}}SD;
struct WiFiClient {
 bool tls=false;size_t pos=0;
 bool connect(const char* h,uint16_t p,int32_t){++connectCalls;host=h;port=p;secure=tls;change(2);if(stall || stallCount-->0){assert(tls && (handshakeSeconds==4 || handshakeSeconds==8));assert(ticks-lastFeed<10);ticks+=handshakeSeconds*1000;return false;}return true;}
 void setNoDelay(bool){}void stop(){}void flush(){}
 void printf(const char* text){wire+=text;}
 template<class...T>void printf(const char* fmt,T...args){char buf[1024];int n=snprintf(buf,sizeof(buf),fmt,args...);assert(n>=0&&n<int(sizeof(buf)));wire.append(buf,n);}
 void print(const char* s){wire+=s;}size_t write(const uint8_t* s,size_t n){assert(n<=1024);ticks+=writeDelay;++writeCalls;if(onWrite)onWrite();if(writeCalls==failWrite)return 0;if(shortWrite)n=std::min(n,shortWrite);wire.append((const char*)s,n);return n;}
 bool connected(){return !disconnected;}int available(){if(fragmented && (++availableCalls%3))return 0;return response.size()-pos;}
 char read(){return response[pos++];}
};
struct WiFiClientSecure:WiFiClient {WiFiClientSecure(){tls=true;}void setInsecure(){} void setCACertBundle(const uint8_t*){verified=true;} void setHandshakeTimeout(unsigned seconds){handshakeSeconds=seconds;} };
const uint8_t roots[]={0};
namespace services {namespace tls { TLS_CONFIGURATION }}
'''+item+'\nclass UploadQueue {public:\n'+kind+'\n'+result+r'''
 bool m_workerStop=false,m_uploadPaused=false;
 UploadResult doUpload(QueueItem&);
 UploadResult doVideoUpload(QueueItem&){assert(false);return {UploadResultKind::Aborted,0,false,0};}
 bool parseUrl(const char*,char*,size_t,uint16_t*,char*,size_t);
};
'''+methods+r'''
std::string receiptResponse(int status=200,const char* kind="audio") {
 std::string body=std::string("{\"uploaded\":true,\"id\":\"7245f6c7-4546-8dcf-becc-e276c07394a3\",\"kind\":\"")+kind+"\"}";
 return "HTTP/1.1 "+std::to_string(status)+" test\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
}
int main(){
 unsigned cases=0;
 for(bool tls:{false,true})for(int point:{0,1,2})for(int http:{200,401,503})for(const char* type:{"image","audio"}){
  initialTls=tls;mutation=point;strcpy(liveUrl,tls?"https://one.invalid:8794/base":"http://one.invalid:8794/base");strcpy(liveKey,"key-one");
  verified=false;wire.clear();response=receiptResponse(http,type);
  QueueItem item{};strcpy(item.filePath,"/synthetic.fixture");strcpy(item.fileName,"fixture.bin");strcpy(item.type,type);strcpy(item.mimeType,"application/octet-stream");
  UploadQueue queue;auto result=queue.doUpload(item);
  assert(result.kind==UploadQueue::UploadResultKind::HttpResponse&&result.httpCode==http&&result.attemptStarted);
  assert(host=="one.invalid"&&port==8794);
  const bool oldPath=NEGATIVE;
  if(!oldPath)assert(verified==tls);
  assert(secure==(initialTls!=(oldPath&&point==1)));
  assert(wire.find(std::string("X-Api-Key: ")+(oldPath&&point?"key-two":"key-one")+"\r\n")!=std::string::npos);
  assert(wire.find("POST /base/api/chat HTTP/1.1\r\n")==0);
  assert(wire.find(std::string("name=\"platform\"\r\n\r\n")+(oldPath?"origami":"device")+"\r\n")!=std::string::npos);
  assert((wire.find("timeline_only")!=std::string::npos)==(std::string(type)=="audio"));
  auto start=wire.find("Content-Length: ")+strlen("Content-Length: ");
  auto body=wire.find("\r\n\r\n")+4;
  assert(std::stoul(wire.substr(start))==wire.size()-body);++cases;
 }
 if(!NEGATIVE){
  stall=true;initialTls=true;mutation=0;strcpy(liveUrl,"https://one.invalid/base");
  wire.clear();ticks=100000;lastFeed=0;handshakeSeconds=120;
  QueueItem item{};strcpy(item.filePath,"/synthetic.fixture");strcpy(item.fileName,"fixture.bin");strcpy(item.type,"audio");strcpy(item.mimeType,"audio/wav");
  UploadQueue queue;auto result=queue.doUpload(item);
  assert(result.kind==UploadQueue::UploadResultKind::GlobalDeferral&&!result.attemptStarted&&result.deferMs==5000);
  assert(wire.empty()&&handshakeSeconds==8);
  stall=false;stallCount=1;connectCalls=0;wire.clear();response=receiptResponse();
  result=queue.doUpload(item);
  assert(connectCalls==2 && result.kind==UploadQueue::UploadResultKind::HttpResponse);
  assert(wire.find("POST ")==wire.rfind("POST ")); // Exactly one request after reconnect.
  stallCount=0;connectCalls=0;
  result=queue.doUpload(item);assert(connectCalls==1);
  stall=true;queue.m_uploadPaused=true;connectCalls=0;wire.clear();
  result=queue.doUpload(item);assert(connectCalls==1 && wire.empty());
  queue.m_uploadPaused=false;

  // Ten successful 2-second chunk writes must feed on progress, not every
  // eight chunks. No loop may hide a single stuck write by feeding blindly.
  stall=false;fileBytes=4096*10;writeDelay=2000;ticks=lastFeed=maxFeedGap=0;
  wire.clear();response=receiptResponse();
  result=queue.doUpload(item);
  assert(result.kind==UploadQueue::UploadResultKind::HttpResponse&&result.httpCode==200);
  assert(maxFeedGap<=4005&&ticks>=24000);
  fileBytes=13;writeDelay=0;ticks=lastFeed=maxFeedGap=0;
  for(int failure:{1,2,3,4}) {
    wire.clear();response=receiptResponse();writeCalls=0;failWrite=failure;
    result=queue.doUpload(item);
    assert(result.kind==UploadQueue::UploadResultKind::RetryableFailure && result.attemptStarted);
  }
  failWrite=0;writeCalls=0;
  onWrite=[&]{if(writeCalls==3)queue.m_uploadPaused=true;};
  result=queue.doUpload(item);
  assert(result.kind==UploadQueue::UploadResultKind::Aborted);
  onWrite=nullptr;queue.m_uploadPaused=false;
  failWrite=0;shortWrite=3;fragmented=true;wire.clear();response=receiptResponse(201);
  result=queue.doUpload(item);
  assert(result.kind==UploadQueue::UploadResultKind::HttpResponse && result.httpCode==201);
  auto length=wire.find("Content-Length: ")+strlen("Content-Length: ");
  assert(std::stoul(wire.substr(length))==wire.size()-(wire.find("\r\n\r\n")+4));
  shortWrite=0;fragmented=false;earlyEof=true;wire.clear();
  result=queue.doUpload(item);
  assert(result.kind==UploadQueue::UploadResultKind::RetryableFailure);
  earlyEof=false;
  for(const char* bad:{"HTTP/1.1 20", "HTTP/1.1 20x broken\r\n", "junk 200 OK\r\n", "HTTP/1.1 200"}) {
    response=bad;disconnected=true;wire.clear();
    result=queue.doUpload(item);
    assert(result.kind==UploadQueue::UploadResultKind::RetryableFailure);
  }
  disconnected=false;
  for(const char* bad:{"HTTP/1.1 200 OK\r\n", "HTTP/1.1 204 OK\r\nContent-Length: 0\r\n\r\n",
      "HTTP/1.1 200 OK\r\nContent-Length: 17\r\n\r\n{\"uploaded\":true}"}) {
    response=bad;result=queue.doUpload(item);
    assert(result.kind==UploadQueue::UploadResultKind::RetryableFailure && result.attemptStarted);
  }
  response=receiptResponse(200,"image");result=queue.doUpload(item);
  assert(result.kind==UploadQueue::UploadResultKind::RetryableFailure);
  response=receiptResponse();fragmented=true;result=queue.doUpload(item);
  assert(result.kind==UploadQueue::UploadResultKind::HttpResponse);fragmented=false;
  puts("PASS short writes, four envelope failures, early SD EOF, fragmented status and malformed/truncated responses");
  puts("PASS stalled TLS defers without sending a request; slow successful chunks feed on progress");
 }
 printf("PASS %u actual multipart request cases; %s\n",cases,NEGATIVE?"old path reproduces mixed configuration":"owned inputs stay stable through SD open and connect");
}
'''
tls_source=subprocess.check_output(['git','show',args.tls_source_ref+':src/services/tls_trust.cpp'],cwd=ROOT,text=True) if args.tls_source_ref else (ROOT/'src/services/tls_trust.cpp').read_text()
code=code.replace('TLS_CONFIGURATION',block(tls_source,'void useRootBundle('))
code=code.replace('NEGATIVE','true' if args.negative_control else 'false')
with tempfile.TemporaryDirectory() as directory:
 path=Path(directory);(path/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),'-I'+os.environ.get('ARDUINOJSON_SRC',str(ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src')),str(path/'test.cpp'),'-o',str(path/'test')],check=True)
 subprocess.run([str(path/'test')],check=True)
