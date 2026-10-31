"""Fault-test the actual two-slot video resume-state owner with fake SD storage.

No device/media changes. Uses pinned ArduinoJson through the protocol validator.
"""
from pathlib import Path
import os, subprocess, tempfile, sys
ROOT=Path(__file__).resolve().parents[1]
JSON=Path(os.environ.get('ARDUINOJSON_SRC',ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src'))
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory)
    def write(name,text):
        file=p/name;file.parent.mkdir(parents=True,exist_ok=True);file.write_text(text)
    write('esp_heap_caps.h',r'''#pragma once
#include <stddef.h>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
void* heap_caps_malloc(size_t,int);void heap_caps_free(void*);
''')
    # Host crypto adapter: exercise production framing with real SHA-256.
    write('mbedtls/sha256.h',r'''#pragma once
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
typedef SHA256_CTX CC_SHA256_CTX;
#define CC_SHA256_Init SHA256_Init
#define CC_SHA256_Update SHA256_Update
#define CC_SHA256_Final SHA256_Final
#endif
#include <cstring>
extern int cryptoFail;
typedef CC_SHA256_CTX mbedtls_sha256_context;
inline void mbedtls_sha256_init(mbedtls_sha256_context* c){memset(c,0,sizeof(*c));}
inline void mbedtls_sha256_free(mbedtls_sha256_context* c){memset(c,0,sizeof(*c));}
inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context* c,int){return cryptoFail==1?-1:CC_SHA256_Init(c)==1?0:-1;}
inline int mbedtls_sha256_update_ret(mbedtls_sha256_context* c,const unsigned char* p,size_t n){return cryptoFail==2?-1:CC_SHA256_Update(c,p,n)==1?0:-1;}
inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context* c,unsigned char* p){return cryptoFail==3?-1:CC_SHA256_Final(p,c)==1?0:-1;}
''')
    write('hal/hal_sd.h',r'''#pragma once
#include <stddef.h>
#include <stdint.h>
namespace hal {namespace sd {
struct StreamReader {void* handle=nullptr;size_t size=0,position=0;};
bool fileInfo(const char*,size_t&,uint32_t&);
bool mounted();bool exists(const char*);bool ensureDir(const char*);
bool openStream(const char*,StreamReader*);size_t readStream(StreamReader*,uint8_t*,size_t);void closeStream(StreamReader*);
bool writeFile(const char*,const uint8_t*,size_t);bool removeFile(const char*);
}}
''')
    write('main.cpp',r'''
#include "services/video_upload_state.h"
#include "hal/hal_sd.h"
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <zlib.h>
using namespace services::video_upload;
std::map<std::string,std::vector<uint8_t>> files;
bool mounted=true,noMemory=false,failOpen=false,failRead=false,failWrite=false,failRemove=false,failDir=false,corruptWrite=false,failReadback=false;
size_t shortWrite=SIZE_MAX,fragment=SIZE_MAX;
int allocations=0,opened=0,writes=0,cryptoFail=0;
size_t maxRead=0,overrideSize=SIZE_MAX;uint32_t modified=1;bool failInfo=false;
std::string unavailablePath;
void* heap_caps_malloc(size_t n,int caps){assert(caps==3);if(noMemory)return nullptr;++allocations;return malloc(n);}
void heap_caps_free(void* p){if(p){--allocations;free(p);}}
namespace hal {namespace sd {
bool mounted(){return ::mounted;}
bool fileInfo(const char* path,size_t& size,uint32_t& time){if(failInfo||!::mounted||!files.count(path))return false;size=overrideSize==SIZE_MAX?files[path].size():overrideSize;time=modified;return true;}
bool exists(const char* path){return files.count(path);}
bool ensureDir(const char*){return !failDir;}
bool openStream(const char* path,StreamReader* r){if(failOpen||path==unavailablePath||!files.count(path))return false;r->handle=&files[path];r->size=files[path].size();r->position=0;++opened;return true;}
size_t readStream(StreamReader* r,uint8_t* out,size_t n){maxRead=std::max(maxRead,n);if(failRead)return 0;const auto& data=*static_cast<std::vector<uint8_t>*>(r->handle);n=std::min(std::min(n,data.size()-r->position),fragment);memcpy(out,data.data()+r->position,n);r->position+=n;return n;}
void closeStream(StreamReader* r){if(r->handle){--opened;r->handle=nullptr;}}
bool writeFile(const char* path,const uint8_t* data,size_t n){++writes;if(failWrite)return false;const size_t actual=std::min(n,shortWrite);files[path]=std::vector<uint8_t>(data,data+actual);if(corruptWrite&&actual)files[path][actual-1]^=1;if(failReadback)failOpen=true;return actual==n;}
bool removeFile(const char* path){if(failRemove)return false;return files.erase(path);}
}}
Resume initial(){Resume r;strcpy(r.media.name,"VID_fixture.avi");memset(r.backendBinding,'a',64);return r;}
Resume finalized(){auto r=initial();r.media.bytes=10000000;memset(r.media.sha256,'b',64);return r;}
Resume admitted(){auto r=finalized();strcpy(r.session,"22222222-2222-4222-8222-222222222222");strcpy(r.endpoint,"https://project.supabase.co/storage/v1/upload/resumable/sign");return r;}
Resume active(){auto r=admitted();strcpy(r.location,"https://project.supabase.co/storage/v1/upload/resumable/sign/resource");return r;}
void sequence(std::vector<uint8_t>& data,uint32_t value) {
 for(unsigned i=0;i<4;++i){data[4+i]=uint8_t(value>>(8*i));data[12+i]=0;}
 const uint32_t crc=uint32_t(crc32(0,data.data(),data.size()));
 for(unsigned i=0;i<4;++i)data[12+i]=uint8_t(crc>>(8*i));
}
int checkpoints=0,cancelAt=0,mutateAt=0;
bool checkpoint(void*){++checkpoints;if(checkpoints==mutateAt)++modified;return checkpoints!=cancelAt;}
void checkIdentify(){
 const char* path="/videos/VID_fixture.avi";
 std::vector<uint8_t> data(2051);for(size_t i=0;i<data.size();++i)data[i]=uint8_t(i);
 files[path]=data;
 auto r=initial();maxRead=0;checkpoints=0;
 assert(identify(path,r,checkpoint,nullptr)==Identify::Ready);
 assert(r.media.bytes==2051&&!strcmp(r.media.sha256,"67060da644f4b4c17a08c6e8c62e2f11c40251a030a936ed0e6bfb1084075945"));
 assert(checkpoints==5&&maxRead==1024&&!opened&&!allocations);
 assert(identify(path,r,checkpoint,nullptr)==Identify::Ready);
 files[path][100]^=1;assert(identify(path,r,checkpoint,nullptr)==Identify::Changed);files[path]=data;
 for(int cancel=1;cancel<=5;++cancel){
  auto pending=initial();auto before=pending;checkpoints=0;cancelAt=cancel;
  assert(identify(path,pending,checkpoint,nullptr)==Identify::Cancelled);
  assert(!memcmp(&pending,&before,sizeof(before))&&!opened&&!allocations);
 }
 cancelAt=0;
 for(int fail=1;fail<=3;++fail){
  auto pending=initial();cryptoFail=fail;
  assert(identify(path,pending,checkpoint,nullptr)==Identify::Unavailable&&!pending.media.bytes&&!pending.media.sha256[0]);
  assert(!opened&&!allocations);
 }
 cryptoFail=0;
 auto pending=initial();fragment=7;
 assert(identify(path,pending,checkpoint,nullptr)==Identify::Ready&&!strcmp(r.media.sha256,pending.media.sha256));fragment=SIZE_MAX;
 pending=initial();checkpoints=0;mutateAt=5;
 assert(identify(path,pending,checkpoint,nullptr)==Identify::Changed&&!pending.media.bytes);mutateAt=0;
 failInfo=true;assert(identify(path,pending,checkpoint,nullptr)==Identify::Unavailable);failInfo=false;
 failRead=true;assert(identify(path,pending,checkpoint,nullptr)==Identify::Unavailable);failRead=false;
 failOpen=true;assert(identify(path,pending,checkpoint,nullptr)==Identify::Unavailable);failOpen=false;
 noMemory=true;assert(identify(path,pending,checkpoint,nullptr)==Identify::NoMemory);noMemory=false;
 overrideSize=2052;assert(identify(path,pending,checkpoint,nullptr)==Identify::Changed);
 overrideSize=core::video_upload::kMaximumBytes+1u;assert(identify(path,pending,checkpoint,nullptr)==Identify::Invalid);overrideSize=SIZE_MAX;
 assert(identify("/videos/other.avi",pending,checkpoint,nullptr)==Identify::Invalid);
 assert(identify(path,pending,nullptr,nullptr)==Identify::Invalid);
 mounted=false;assert(identify(path,pending,checkpoint,nullptr)==Identify::Unavailable);mounted=true;
 files[path].clear();assert(identify(path,pending,checkpoint,nullptr)==Identify::Invalid);
 files.clear();assert(!opened&&!allocations);
 puts("PASS bounded media hashing, independent SHA vector, fragmentation, cancellation, changed source and cleanup faults");
}
int main(){
 checkIdentify();
 core::secrets::BackendConfig backend{};char bound[65],again[65];
 strcpy(backend.url,"https://fixture.invalid");strcpy(backend.apiKey,"fixture-account");
 assert(binding(backend,bound)&&!strcmp(bound,"cc058876e4d3f293a602c8d1a0aa8fa6f0c164bda5ca6abaed90d4afbff3a1c5"));
 assert(binding(backend,again)&&!strcmp(bound,again));
 strcpy(backend.apiKey,"different-account");assert(binding(backend,again)&&strcmp(bound,again));
 strcpy(backend.apiKey,"fixture-account");strcpy(backend.url,"https://other.invalid");
 assert(binding(backend,again)&&strcmp(bound,again));
 strcpy(backend.apiKey,"AAAAAAAA-AAAA-4AAA-8AAA-AAAAAAAAAAAA");assert(binding(backend,bound));
 strcpy(backend.apiKey,"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");assert(binding(backend,again)&&!strcmp(bound,again));
 strcpy(backend.apiKey,"kami_ABC");assert(binding(backend,bound));
 strcpy(backend.apiKey,"kami_abc");assert(binding(backend,again)&&strcmp(bound,again));
 backend.apiKey[0]=0;assert(!binding(backend,bound)&&!bound[0]);
 memset(backend.apiKey,'a',sizeof(backend.apiKey));assert(!binding(backend,bound));
 strcpy(backend.apiKey,"bad\r\nkey");assert(!binding(backend,bound));
 strcpy(backend.apiKey,"valid");memset(backend.url,'a',sizeof(backend.url));assert(!binding(backend,bound));

 Resume out;
 assert(load("VID_1.avi",out)==Load::Missing);
 for(const char* id:{"","../x","a/b",".hidden","twenty-four-characters-xx"})assert(load(id,out)==Load::InvalidId);
 noMemory=true;assert(load("VID_1.avi",out)==Load::NoMemory);assert(save("VID_1.avi",initial())==Save::NoMemory);noMemory=false;
 mounted=false;assert(load("VID_1.avi",out)==Load::Unavailable);assert(save("VID_1.avi",initial())==Save::Unavailable);assert(!erase("VID_1.avi"));mounted=true;
 failDir=true;assert(save("VID_1.avi",initial())==Save::Unavailable);failDir=false;
 failWrite=true;assert(save("VID_1.avi",initial())==Save::Unavailable);failWrite=false;
 assert(load("VID_1.avi",out)==Load::Missing);
 shortWrite=10;assert(save("VID_1.avi",initial())==Save::Unavailable);shortWrite=SIZE_MAX;
 assert(load("VID_1.avi",out)==Load::Corrupt&&save("VID_1.avi",initial())==Save::Corrupt);
 assert(erase("VID_1.avi"));
 assert(save("VID_1.avi",initial())==Save::Saved);assert(files.size()==1);
 assert(load("VID_1.avi",out)==Load::Ready&&out.media.bytes==0&&!out.session[0]);
 int before=writes;assert(save("VID_1.avi",initial())==Save::Saved&&writes==before);
 assert(save("VID_1.avi",finalized())==Save::Saved);assert(files.size()==2);
 assert(save("VID_1.avi",admitted())==Save::Saved);
 assert(save("VID_1.avi",active())==Save::Saved);
 assert(load("VID_1.avi",out)==Load::Ready&&!strcmp(out.location,active().location));
 // Never silently bind old media to another backend, file, session or generation.
 auto changed=active();changed.backendBinding[0]='c';assert(save("VID_1.avi",changed)==Save::Conflict);
 changed=active();changed.media.sha256[0]='c';assert(save("VID_1.avi",changed)==Save::Conflict);
 changed=active();changed.media.bytes++;assert(save("VID_1.avi",changed)==Save::Conflict);
 changed=active();strcpy(changed.media.name,"another.avi");assert(save("VID_1.avi",changed)==Save::Conflict);
 changed=active();changed.session[0]='3';assert(save("VID_1.avi",changed)==Save::Conflict);
 assert(save("VID_1.avi",initial())==Save::Conflict);
 changed=active();changed.generation=1;assert(save("VID_1.avi",changed)==Save::Conflict);
 changed.location[0]=0;assert(save("VID_1.avi",changed)==Save::Saved);
 assert(save("VID_1.avi",active())==Save::Conflict);
 auto skipped=changed;skipped.generation=3;assert(save("VID_1.avi",skipped)==Save::Conflict);
 assert(load("VID_1.avi",out)==Load::Ready&&out.generation==1&&!out.location[0]);
 // If a slot cannot be read, do not guess whether it is older than the other.
 failRead=true;assert(load("VID_1.avi",out)==Load::Unavailable);failRead=false;
 unavailablePath="/upload_queue/video/VID_1.avi.1";assert(load("VID_1.avi",out)==Load::Unavailable);unavailablePath.clear();
 fragment=7;assert(load("VID_1.avi",out)==Load::Ready);fragment=SIZE_MAX;
 // Short write at every byte preserves a prior valid record and reports failure.
 const auto stable=files;auto next=changed;next.generation=2;
 assert(save("VID_1.avi",next)==Save::Saved);
 const auto complete=files;
 size_t bytes=0;for(const auto& entry:files)bytes=std::max(bytes,entry.second.size());
 for(size_t cut=0;cut<bytes;++cut){
  files=stable;shortWrite=cut;auto result=save("VID_1.avi",next);
  if(result==Save::Saved)continue; // Cut may exceed this shorter record's length.
  assert(result==Save::Unavailable);shortWrite=SIZE_MAX;
  assert(load("VID_1.avi",out)==Load::Ready&&out.generation==1);
 }
 shortWrite=SIZE_MAX;files=stable;corruptWrite=true;
 assert(save("VID_1.avi",next)==Save::Unavailable);corruptWrite=false;
 assert(load("VID_1.avi",out)==Load::Ready&&out.generation==1);
 // Lost readback is an uncertain save, never a false success. A retry discovers
 // the valid written slot rather than overwriting identity or advancing again.
 files=stable;failReadback=true;assert(save("VID_1.avi",next)==Save::Unavailable);
 failReadback=failOpen=false;assert(load("VID_1.avi",out)==Load::Ready&&out.generation==2);
 before=writes;assert(save("VID_1.avi",next)==Save::Saved&&writes==before);
 // Every bit in the newest persisted record is covered by validation/checksum.
 files=complete;std::string newest;
 for(const auto& entry:files)if(entry.second[4]==6)newest=entry.first;
 assert(!newest.empty());
 for(size_t i=0;i<complete.at(newest).size();++i)for(unsigned bit=0;bit<8;++bit){
  files=complete;files[newest][i]^=uint8_t(1u<<bit);
  assert(load("VID_1.avi",out)==Load::Ready&&out.generation==1);
 }
 files=complete;
 for(auto& entry:files)sequence(entry.second,6);
 assert(load("VID_1.avi",out)==Load::Corrupt&&save("VID_1.avi",next)==Save::Corrupt);
 files=complete;sequence(files[newest],5u+0x80000000u);
 assert(load("VID_1.avi",out)==Load::Corrupt);
 files=complete;
 for(auto& entry:files)sequence(entry.second,entry.first==newest?0u:0xffffffffu);
 assert(load("VID_1.avi",out)==Load::Ready&&out.generation==2);
 auto afterWrap=next;afterWrap.generation=3;assert(save("VID_1.avi",afterWrap)==Save::Saved);
 assert(load("VID_1.avi",out)==Load::Ready&&out.generation==3);
 files=complete;files[newest].push_back(0);
 assert(load("VID_1.avi",out)==Load::Ready&&out.generation==1);
 files=complete;for(auto& entry:files)entry.second[0]='x';
 assert(load("VID_1.avi",out)==Load::Corrupt&&save("VID_1.avi",next)==Save::Corrupt);
 // Metadata cleanup never touches the source clip.
 files["/videos/VID_fixture.avi"]={1,2,3};failRemove=true;assert(!erase("VID_1.avi"));failRemove=false;
 assert(erase("VID_1.avi")&&load("VID_1.avi",out)==Load::Missing);
 assert(files.size()==1&&files.count("/videos/VID_fixture.avi"));
 changed=active();strcpy(changed.location,"https://elsewhere.invalid/storage/v1/upload/resumable/sign/id");assert(save("VID_2",changed)==Save::Invalid);
 changed=initial();strcpy(changed.session,active().session);assert(save("VID_2",changed)==Save::Invalid);
 changed=active();memset(changed.backendBinding,'x',sizeof(changed.backendBinding));assert(save("VID_2",changed)==Save::Invalid);
 assert(!allocations&&!opened);
 puts("PASS resume persistence: initial binding, immutable identity, generation fencing, readback, all-byte short writes, bit corruption and media-preserving cleanup");
}
''')
    exe=p/'test'
    subprocess.run(['c++','-Wno-deprecated-declarations','-std=c++11','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),'-I'+str(JSON),str(ROOT/'src/services/video_upload_state.cpp'),str(p/'main.cpp'),'-lz']+([] if sys.platform=='darwin' else ['-lcrypto'])+['-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
