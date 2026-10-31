"""Exercise the production video coordinator with deterministic state/transport owners."""
from pathlib import Path
import subprocess,tempfile,os
ROOT=Path(__file__).resolve().parents[1]
JSON=Path(os.environ.get('ARDUINOJSON_SRC',ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src'))
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)
 (p/'esp_heap_caps.h').write_text('''#pragma once
#include <stddef.h>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
void* heap_caps_malloc(size_t,int);void heap_caps_free(void*);
''')
 (p/'main.cpp').write_text(r'''
#include "services/video_upload.h"
#include "services/video_upload_transfer.h"
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <cstdio>
using namespace services::video_upload;
using core::video_upload::State;
Resume persisted;core::secrets::BackendRequest config;
bool stopped=false,noMemory=false,saveOk=true,changeDuringRequest=false;
Load loaded=Load::Ready;Identify identified=Identify::Ready;
TransferResult transferred{Transfer::Progress,204};int allocations=0,saves=0,hashes=0,transfers=0,tokenClears=0;
struct Reply {BackendOperation op;BackendOutcome outcome;unsigned code;State state;unsigned generation;};
std::vector<Reply> replies;size_t at=0;
void* heap_caps_malloc(size_t n,int caps){assert(caps==3);if(noMemory)return nullptr;++allocations;return malloc(n);}
void heap_caps_free(void* p){if(p){--allocations;free(p);}}
namespace core {namespace secrets {
BackendRequest backendRequest(){return config;}
bool setDeviceTokenFor(const BackendRequest& request,const char* token){assert(request.revision==config.revision&&!token[0]);++tokenClears;config.token[0]=0;return true;}
}}
namespace services {namespace video_upload {
bool binding(const core::secrets::BackendConfig& backend,char (&out)[65]){if(!backend.apiKey[0])return false;memset(out,!strcmp(backend.apiKey,"account")?'b':'c',64);out[64]=0;return true;}
Load load(const char*,Resume& out){out=persisted;return loaded;}
Save save(const char*,const Resume& r){++saves;if(!saveOk)return Save::Unavailable;persisted=r;return Save::Saved;}
Identify identify(const char*,Resume& r,bool (*check)(void*),void* context){++hashes;if(!check(context))return Identify::Cancelled;if(identified==Identify::Ready){r.media.bytes=100;memset(r.media.sha256,'a',64);}return identified;}
BackendResult request(BackendOperation op,const core::secrets::BackendRequest&,const Resume& r,core::video_upload::Response& out,bool (*check)(void*),void* context){
 assert(check(context));assert(at<replies.size());const auto expected=replies[at++];assert(op==expected.op);
 out={};out.state=expected.state;out.generation=expected.generation;
 strcpy(out.session,"11111111-1111-4111-8111-111111111111");
 if(op==BackendOperation::Admit&&out.state==State::Pending){out.hasUpload=true;strcpy(out.endpoint,"https://storage.invalid/storage/v1/upload/resumable/sign");strcpy(out.token,"scoped");}
 if(changeDuringRequest)++config.revision;
 return {expected.outcome,expected.code};
}
TransferResult transfer(const char*,const char*,Resume& r,const core::video_upload::Response& admission,bool (*check)(void*),void* context){
 assert(check(context)&&admission.hasUpload&&r.session[0]&&r.media.bytes);assert(!strcmp(persisted.endpoint,r.endpoint));++transfers;return transferred;
}
}}
bool checkpoint(void*){return !stopped;}
void reset(bool session=true){
 assert(!allocations);persisted={};config={};strcpy(config.backend.url,"https://backend.invalid");strcpy(config.backend.apiKey,"account");strcpy(config.token,"bearer");config.revision=7;
 strcpy(persisted.media.name,"VID_test.avi");persisted.media.bytes=100;memset(persisted.media.sha256,'a',64);memset(persisted.backendBinding,'b',64);
 if(session){strcpy(persisted.session,"11111111-1111-4111-8111-111111111111");strcpy(persisted.endpoint,"https://storage.invalid/storage/v1/upload/resumable/sign");strcpy(persisted.location,"https://storage.invalid/storage/v1/upload/resumable/sign/old");}
 stopped=noMemory=changeDuringRequest=false;saveOk=true;loaded=Load::Ready;identified=Identify::Ready;
 transferred={Transfer::Progress,204};at=0;saves=hashes=transfers=tokenClears=0;replies.clear();
}
void reply(BackendOperation op,State state=State::Pending,unsigned generation=0,BackendOutcome outcome=BackendOutcome::Reply,unsigned code=200){replies.push_back({op,outcome,code,state,generation});}
Result run(){auto result=advance("VID_test.avi","/videos/VID_test.avi",checkpoint,nullptr);assert(!allocations&&at==replies.size());return result;}
int main(){
 reset(false);persisted.media.bytes=0;persisted.media.sha256[0]=0;assert(run().step==Step::Progress&&hashes==1&&saves==1&&persisted.media.bytes);
 reset(false);persisted.media.bytes=0;persisted.media.sha256[0]=0;saveOk=false;assert(run().step==Step::Deferred&&!persisted.media.bytes);
 reset(false);reply(BackendOperation::Admit);assert(run().step==Step::Progress&&saves==1&&persisted.session[0]&&transfers==0);
 reset(false);reply(BackendOperation::Admit,State::Complete);assert(run().step==Step::Complete&&!transfers);
 reset();reply(BackendOperation::Status,State::Verifying);assert(run().step==Step::Progress&&!transfers&&!saves);
 reset();reply(BackendOperation::Status,State::Complete);assert(run().step==Step::Complete&&!transfers);
 reset();reply(BackendOperation::Status);reply(BackendOperation::Admit);assert(run().step==Step::Progress&&transfers==1&&!hashes);
 reset();reply(BackendOperation::Status);reply(BackendOperation::Admit);reply(BackendOperation::Complete,State::Complete);transferred={Transfer::Verify,200};assert(run().step==Step::Complete&&transfers==1);
 reset();reply(BackendOperation::Status);reply(BackendOperation::Admit);reply(BackendOperation::Complete,State::Verifying);transferred={Transfer::Verify,200};assert(run().step==Step::Progress);
 reset();reply(BackendOperation::Status,State::Failed);reply(BackendOperation::Recover,State::Pending,1);assert(run().step==Step::Progress&&persisted.generation==1&&!persisted.location[0]&&!persisted.endpoint[0]);
 // Recovery succeeded remotely, but its reply/save was lost. Status repairs the
 // saved generation and clears old provider state before any new transfer.
 reset();reply(BackendOperation::Status,State::Pending,1);assert(run().step==Step::Progress&&persisted.generation==1&&!persisted.location[0]&&!transfers);
 reset();reply(BackendOperation::Status,State::Pending,1);saveOk=false;assert(run().step==Step::Deferred&&persisted.generation==0&&persisted.location[0]);
 reset();persisted.generation=3;reply(BackendOperation::Status,State::Failed,3);assert(run().step==Step::Rejected);
 reset();reply(BackendOperation::Status);reply(BackendOperation::Admit);reply(BackendOperation::Complete,State::Complete);transferred={Transfer::Retry,404};assert(run().step==Step::Complete&&persisted.location[0]);
 reset();reply(BackendOperation::Status);reply(BackendOperation::Admit);reply(BackendOperation::Complete,State::Pending,0,BackendOutcome::HttpError,503);transferred={Transfer::Retry,410};assert(run().step==Step::Retry&&!persisted.location[0]);
 reset();reply(BackendOperation::Status,State::Pending,0,BackendOutcome::HttpError,401);assert(run().step==Step::Deferred&&tokenClears==1);
 reset();reply(BackendOperation::Status,State::Pending,0,BackendOutcome::HttpError,404);assert(run().step==Step::Rejected&&!transfers);
 reset();reply(BackendOperation::Status);changeDuringRequest=true;assert(run().step==Step::Cancelled&&!transfers&&!saves);
 reset();strcpy(config.backend.apiKey,"other-account");assert(run().step==Step::Rejected);
 reset();config.token[0]=0;assert(run().step==Step::Deferred);
 for(auto state:{Load::Missing,Load::Corrupt,Load::InvalidId}){reset();loaded=state;assert(run().step==Step::Rejected);}
 reset();loaded=Load::Unavailable;assert(run().step==Step::Deferred);
 reset();noMemory=true;assert(run().step==Step::Deferred);
 reset();stopped=true;assert(run().step==Step::Cancelled);
 reset();reply(BackendOperation::Status);reply(BackendOperation::Admit);transferred={Transfer::Cancelled,0};assert(run().step==Step::Cancelled);
 puts("PASS actual video coordinator: identity/session persistence, scoped renewal, verification, lost recovery, URL expiry, cancellation and account changes");
}
''')
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),'-I'+str(JSON),str(ROOT/'src/services/video_upload.cpp'),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
