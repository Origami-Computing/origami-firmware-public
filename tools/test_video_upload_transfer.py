"""Exercise production TUS transport with deterministic socket/SD/state faults.
No live network/device changes; TLS verification invocation is checked, not TLS itself.
"""
from pathlib import Path
import subprocess,tempfile,os
ROOT=Path(__file__).resolve().parents[1]
JSON=Path(os.environ.get('ARDUINOJSON_SRC',ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src'))
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)
 def write(name,text):
  f=p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(text)
 write('esp_heap_caps.h','''#pragma once
#include <stddef.h>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
void* heap_caps_malloc(size_t,int);void heap_caps_free(void*);
''')
 write('hal/hal_sd.h','''#pragma once
#include <stddef.h>
#include <stdint.h>
namespace hal {namespace sd {
struct StreamReader{void* handle=nullptr;size_t size=0,position=0;};
bool openStream(const char*,StreamReader*);size_t readStream(StreamReader*,uint8_t*,size_t);
bool seekStream(StreamReader*,size_t);void closeStream(StreamReader*);
}}
''')
 write('WiFiClientSecure.h','''#pragma once
#include <string>
#include <vector>
#include <stdint.h>
#include <stddef.h>
extern std::vector<std::string> replies,sent;
extern bool connectOk,writeOk,trusted;
extern size_t fragment;
uint32_t millis();void delay(unsigned);
class WiFiClientSecure {
 int index=-1;size_t at=0;
 public:
 int setTimeout(unsigned seconds){return seconds==1?0:-1;}
 int connect(const char*,uint16_t,int);
 size_t write(const uint8_t*,size_t);
 int available(){return index>=0&&at<replies[index].size();}
 int read(){return available()?static_cast<unsigned char>(replies[index][at++]):-1;}
 bool connected(){return available();}
 void stop(){index=-1;}
};
''')
 write('mbedtls/base64.h','''#pragma once
#include <stddef.h>
int mbedtls_base64_encode(unsigned char*,size_t,size_t*,const unsigned char*,size_t);
''')
 write('main.cpp',r'''
#include "services/video_upload_transfer.h"
#include "WiFiClientSecure.h"
#include "hal/hal_sd.h"
#include <cassert>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
using namespace services::video_upload;
using namespace core::video_upload;
std::vector<std::string> replies,sent;
bool connectOk=true,writeOk=true,trusted=false,saveOk=true,loadOk=true,noMemory=false,readOk=true,cancel=false;
size_t fragment=SIZE_MAX,seeked=0,cancelAfter=SIZE_MAX;int opened=0,allocations=0,saves=0,allocationCalls=0,failAllocation=0;
uint32_t ticks=0;
Resume persisted;
uint32_t millis(){return ticks++;}void delay(unsigned n){ticks+=n;}
void* heap_caps_malloc(size_t n,int caps){assert(caps==3);++allocationCalls;if(noMemory||allocationCalls==failAllocation)return nullptr;++allocations;return malloc(n);}
void heap_caps_free(void* ptr){if(ptr){--allocations;free(ptr);}}
namespace services {namespace tls {void useRootBundle(WiFiClientSecure&){trusted=true;}}}
int WiFiClientSecure::connect(const char* host,uint16_t port,int timeout){
 assert(trusted&&std::string(host)=="fixture.invalid"&&port==443&&timeout==5000);trusted=false;
 if(!connectOk)return 0;index=sent.size();assert(size_t(index)<replies.size());sent.emplace_back();at=0;return 1;
}
size_t WiFiClientSecure::write(const uint8_t* p,size_t n){assert(n<=1024);if(!writeOk)return 0;n=std::min(n,fragment);sent[index].append((const char*)p,n);return n;}
int mbedtls_base64_encode(unsigned char* out,size_t cap,size_t* used,const unsigned char* in,size_t n){
 const char* alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
 *used=4*((n+2)/3);if(cap<=*used)return -1;
 size_t o=0;for(size_t i=0;i<n;i+=3){unsigned x=unsigned(in[i])<<16;if(i+1<n)x|=unsigned(in[i+1])<<8;if(i+2<n)x|=in[i+2];out[o++]=alphabet[x>>18];out[o++]=alphabet[(x>>12)&63];out[o++]=i+1<n?alphabet[(x>>6)&63]:'=';out[o++]=i+2<n?alphabet[x&63]:'=';}out[o]=0;return 0;
}
namespace hal {namespace sd {
bool openStream(const char*,StreamReader* r){r->handle=r;r->size=persisted.media.bytes;++opened;return true;}
void closeStream(StreamReader* r){if(r->handle){r->handle=nullptr;--opened;}}
bool seekStream(StreamReader* r,size_t at){r->position=at;seeked=at;return true;}
size_t readStream(StreamReader* r,uint8_t* out,size_t n){assert(n<=1024);if(!readOk)return 0;for(size_t i=0;i<n;++i)out[i]=uint8_t(r->position+i);r->position+=n;return n;}
}}
namespace services {namespace video_upload {
bool binding(const core::secrets::BackendConfig& b,char (&out)[65]){
 if(!b.url[0]||!b.apiKey[0])return false;
 memset(out,!strcmp(b.apiKey,"fixture-account")?'b':'c',64);out[64]=0;return true;
}
Load load(const char*,Resume& out){if(!loadOk)return Load::Unavailable;out=persisted;return Load::Ready;}
Save save(const char*,const Resume& r){++saves;assert(sent.size()==1&&sent[0].find("PATCH")==std::string::npos);if(!saveOk)return Save::Unavailable;persisted=r;return Save::Saved;}
}}
bool checkpoint(void*){return !cancel&&(sent.size()<2||sent.back().size()<cancelAfter);}
std::string head(uint32_t offset){return "HTTP/1.1 200 OK\r\nTus-Resumable: 1.0.0\r\nUpload-Length: "+std::to_string(persisted.media.bytes)+"\r\nUpload-Offset: "+std::to_string(offset)+"\r\n\r\n";}
std::string ack(uint32_t offset){return "HTTP/1.1 204 No Content\r\nTus-Resumable: 1.0.0\r\nUpload-Offset: "+std::to_string(offset)+"\r\n\r\n";}
void reset(){sent.clear();replies.clear();ticks=0;seeked=0;saves=0;allocationCalls=0;assert(!opened&&!allocations);}
int main(){
 Resume r;strcpy(r.media.name,"VID_test.avi");r.media.bytes=kChunkBytes+100;memset(r.media.sha256,'a',64);memset(r.backendBinding,'b',64);
 strcpy(r.session,"11111111-1111-4111-8111-111111111111");strcpy(r.endpoint,"https://fixture.invalid/storage/v1/upload/resumable/sign");persisted=r;
 Response a;a.state=State::Pending;a.hasUpload=true;strcpy(a.session,r.session);strcpy(a.endpoint,r.endpoint);strcpy(a.token,"synthetic.scoped.token");strcpy(a.object,"device-video/owner/test.avi");
 auto run=[&](){auto result=transfer("VID_test.avi","/videos/VID_test.avi",r,a,checkpoint,nullptr);assert(!opened&&!allocations);return result.outcome;};
 const std::string creation="HTTP/1.1 201 Created\r\nTus-Resumable: 1.0.0\r\nLocation: /storage/v1/upload/resumable/sign/resource\r\n\r\n";
 replies={creation};assert(run()==Transfer::Progress&&saves==1&&r.location[0]);
 assert(sent[0].find("Content-Length: 0\r\n")!=std::string::npos&&sent[0].find("x-signature: synthetic.scoped.token\r\n")!=std::string::npos);
 assert(sent[0].find("Authorization:")==std::string::npos&&sent[0].find("X-Api-Key:")==std::string::npos);
 reset();replies={head(0),ack(kChunkBytes)};assert(run()==Transfer::Progress&&sent.size()==2);
 auto body=sent[1].substr(sent[1].find("\r\n\r\n")+4);assert(body.size()==kChunkBytes);
 for(size_t i=0;i<body.size();++i)assert(uint8_t(body[i])==uint8_t(i));
 // Lost reply: retry uses the provider's offset, never the previous local count.
 reset();replies={head(kChunkBytes),""};assert(run()==Transfer::Retry&&seeked==kChunkBytes);
 reset();replies={head(kChunkBytes+100)};assert(run()==Transfer::Verify&&sent.size()==1);
 reset();fragment=7;replies={head(kChunkBytes),ack(kChunkBytes+100)};assert(run()==Transfer::Progress);fragment=SIZE_MAX;
 reset();replies={head(kChunkBytes),ack(kChunkBytes+99)};assert(run()==Transfer::Retry);
 reset();replies={head(kChunkBytes+101)};assert(run()==Transfer::Retry&&sent.size()==1);
 reset();replies={head(0),ack(kChunkBytes)};readOk=false;assert(run()==Transfer::Storage);readOk=true;
 reset();replies={head(0),ack(kChunkBytes)};cancelAfter=3000;
 assert(run()==Transfer::Cancelled&&sent[1].size()<5000);cancelAfter=SIZE_MAX;
 reset();++persisted.generation;assert(run()==Transfer::Invalid&&sent.empty());--persisted.generation;
 reset();cancel=true;assert(run()==Transfer::Cancelled&&sent.empty());cancel=false;
 noMemory=true;assert(run()==Transfer::NoMemory);noMemory=false;
 loadOk=false;assert(run()==Transfer::Storage);loadOk=true;
 connectOk=false;assert(run()==Transfer::Retry);connectOk=true;
 reset();replies={head(0)};writeOk=false;assert(run()==Transfer::Retry);writeOk=true;
 reset();strcpy(r.location,"https://foreign.invalid/storage/v1/upload/resumable/sign/resource");persisted=r;assert(run()==Transfer::Invalid&&sent.empty());
 r.location[0]=0;persisted=r;
 reset();saveOk=false;replies={creation};assert(run()==Transfer::Storage&&!r.location[0]&&sent.size()==1);saveOk=true;
 reset();replies={"HTTP/1.1 201 Created\r\nTus-Resumable: 1.0.0\r\nLocation: https://foreign.invalid/storage/v1/upload/resumable/sign/id\r\n\r\n"};assert(run()==Transfer::Retry&&!saves);

 reset();
 core::secrets::BackendRequest backend{};strcpy(backend.backend.url,"https://fixture.invalid");
 strcpy(backend.backend.apiKey,"fixture-account");strcpy(backend.token,"fixture-device");Response output;
 JsonDocument doc;
 auto json=[&](const char* state="pending",unsigned generation=0){
  doc.clear();doc["name"]=r.media.name;doc["bytes"]=r.media.bytes;doc["sha256"]=r.media.sha256;
  doc["session_id"]=r.session;doc["generation"]=generation;doc["state"]=state;doc["uploaded"]=!strcmp(state,"complete");
  if(!strcmp(state,"complete")){doc["id"]=r.session;doc["kind"]="video";}
  std::string body;serializeJson(doc,body);return body;
 };
 auto frame=[](const std::string& body){return "HTTP/1.1 200 OK\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;};
 auto backendRun=[&](BackendOperation operation){const auto result=services::video_upload::request(operation,backend,r,output,checkpoint,nullptr);assert(!opened&&!allocations);return result;};
 for(auto op:{BackendOperation::Admit,BackendOperation::Status,BackendOperation::Complete,BackendOperation::Recover}){
  reset();replies={frame(json(op==BackendOperation::Complete?"complete":"pending",op==BackendOperation::Recover?1:0))};
  assert(backendRun(op).outcome==BackendOutcome::Reply);
  assert(sent[0].find("Authorization: Bearer fixture-device\r\n")!=std::string::npos);
  assert(sent[0].find("X-Api-Key: fixture-account\r\n")!=std::string::npos&&sent[0].find("x-signature:")==std::string::npos);
  if(op==BackendOperation::Status)assert(sent[0].find("GET /api/device/video/uploads/")==0);
  if(op==BackendOperation::Complete)assert(output.state==State::Complete&&sent[0].substr(sent[0].size()-2)=="{}");
 }
 const std::string valid=frame(json());
 for(size_t n=0;n<valid.size();++n){reset();replies={valid.substr(0,n)};assert(backendRun(BackendOperation::Status).outcome==BackendOutcome::Retry);}
 reset();replies={frame(json()+"garbage")};assert(backendRun(BackendOperation::Status).outcome==BackendOutcome::Retry);
 reset();json();doc["bytes"]=1;std::string wrong;serializeJson(doc,wrong);replies={frame(wrong)};assert(backendRun(BackendOperation::Status).outcome==BackendOutcome::Retry);
 reset();replies={frame(json("pending",1))};assert(backendRun(BackendOperation::Status).outcome==BackendOutcome::Reply);
 reset();replies={frame(json("pending",2))};assert(backendRun(BackendOperation::Recover).outcome==BackendOutcome::Retry);
 reset();replies={"HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n"};assert(backendRun(BackendOperation::Complete).outcome==BackendOutcome::Retry);
 reset();replies={"HTTP/1.1 403 Forbidden\r\n\r\n"};auto denied=backendRun(BackendOperation::Admit);assert(denied.outcome==BackendOutcome::HttpError&&denied.httpStatus==403);
 reset();replies={"HTTP/1.1 302 Found\r\nLocation: https://foreign.invalid\r\n\r\n"};assert(backendRun(BackendOperation::Admit).outcome==BackendOutcome::HttpError&&sent.size()==1);
 reset();body=json();char lengthHex[20];snprintf(lengthHex,sizeof(lengthHex),"%zx",body.size());
 replies={"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Security-Policy: "+std::string(500,'x')+"\r\n\r\n"+lengthHex+"\r\n"+body+"\r\n0\r\n\r\n"};
 assert(backendRun(BackendOperation::Status).outcome==BackendOutcome::Reply);
 reset();replies={"HTTP/1.1 200 OK\r\nContent-Length: 4097\r\n\r\n"};assert(backendRun(BackendOperation::Status).outcome==BackendOutcome::Retry);
 reset();strcpy(backend.backend.apiKey,"different-account");assert(backendRun(BackendOperation::Admit).outcome==BackendOutcome::Invalid&&sent.empty());strcpy(backend.backend.apiKey,"fixture-account");
 cancel=true;assert(backendRun(BackendOperation::Status).outcome==BackendOutcome::Cancelled);cancel=false;
 noMemory=true;assert(backendRun(BackendOperation::Status).outcome==BackendOutcome::NoMemory);noMemory=false;

 reset();replies={frame(json())};assert(backendRun(BackendOperation::Status).outcome==BackendOutcome::Reply);
 const int allocationPoints=allocationCalls;assert(allocationPoints>2&&allocationPoints<100);
 for(int fail=1;fail<=allocationPoints;++fail){
  reset();replies={frame(json())};failAllocation=fail;
  const auto result=backendRun(BackendOperation::Status);
  assert(result.outcome==BackendOutcome::NoMemory||result.outcome==BackendOutcome::Reply);
 }
 failAllocation=0;reset();json();auto upload=doc["upload"].to<JsonObject>();
 upload["endpoint"]="https://fixture.invalid/storage/v1/upload/resumable/sign";upload["bucket"]="media";
 upload["object"]="device-video/11111111-1111-4111-8111-111111111111/11111111-1111-4111-8111-111111111111.avi";
 upload["token"]=std::string(2047,'t');upload["chunk_bytes"]=kChunkBytes;upload["content_type"]="video/x-msvideo";
 std::string large;serializeJson(doc,large);replies={frame(large)};
 assert(backendRun(BackendOperation::Admit).outcome==BackendOutcome::Reply&&strlen(output.token)==2047);
 puts("PASS backend operations, credential separation, bounded/chunked JSON, every-byte interruption, receipt matching and redirects");
 reset();puts("PASS production TUS create/save/HEAD/PATCH, byte-exact chunks, lost replies, offset recovery, faults and credential fencing");
}
''')
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),'-I'+str(JSON),str(ROOT/'src/services/video_upload_transfer.cpp'),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
