"""Production custom-app store fault tests with real SHA-256 and faultable media.

No device access. ARDUINOJSON_SRC can point to pinned ArduinoJson 7.4.3 sources.
"""
from pathlib import Path
import os, subprocess, tempfile, sys, argparse
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--thread-sanitizer",action="store_true");args=parser.parse_args()
ROOT=Path(__file__).resolve().parents[1]
JSON=Path(os.environ.get('ARDUINOJSON_SRC', ROOT/'.pio/libdeps/esp32-s3-menu-n16r8-production/ArduinoJson/src'))
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory)
    def write(name,text):
        f=p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(text)
    write('Arduino.h',r'''#pragma once
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
inline uint32_t millis(){return 100;}
struct Print {std::string output;void println(const char* s){output+=s;output+="\n";}
 void printf(const char* format,...){char b[1024];va_list args;va_start(args,format);vsnprintf(b,sizeof(b),format,args);va_end(args);output+=b;}};
''')
    write('esp_heap_caps.h',r'''#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
extern bool noMemory;
inline void* heap_caps_malloc(size_t n,int){return noMemory?nullptr:malloc(n);}
inline void* heap_caps_realloc(void* p,size_t n,int){return noMemory?nullptr:realloc(p,n);}
inline void heap_caps_free(void* p){free(p);}
''')
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
typedef CC_SHA256_CTX mbedtls_sha256_context;
inline void mbedtls_sha256_init(mbedtls_sha256_context* c){memset(c,0,sizeof(*c));}
inline void mbedtls_sha256_free(mbedtls_sha256_context*){}
inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context* c,int){return CC_SHA256_Init(c)==1?0:-1;}
inline int mbedtls_sha256_update_ret(mbedtls_sha256_context* c,const unsigned char* p,size_t n){return CC_SHA256_Update(c,p,n)==1?0:-1;}
inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context* c,unsigned char* p){return CC_SHA256_Final(p,c)==1?0:-1;}
''')
    write('hal/hal_sd.h',r'''#pragma once
#include <stddef.h>
#include <stdint.h>
namespace hal { namespace sd {
bool mounted();bool exists(const char*);bool fileInfo(const char*,size_t&,uint32_t&);
size_t readFile(const char*,char*,size_t);int listDirs(const char*,char*,int,int);
} }
''')
    write('main.cpp',r'''
#include "services/custom_apps.h"
#include "widgets/storage.h"
#include "core/app_store_qr.h"
#include "tooling/custom_app_commands.h"
#include <mbedtls/sha256.h>
#include <thread>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <algorithm>
using namespace services::custom_apps;
using Bytes=std::vector<uint8_t>;
std::map<std::string,Bytes> files;
bool fatCatalog=false,enumerateCatalog=false;
bool pauseDownload=false,pauseInDownload=false,clearPauseOnRead=false;
bool noMemory=false,offline=false,ioRead=false,ioClose=false,corruptWrite=false;
int writes=0,failWrite=0,cancelPoint=0,checkpoints=0;
size_t cut=SIZE_MAX;uint64_t space=1024*1024;
namespace hal { namespace app_storage {
std::string key(Medium m,const char* path){return std::to_string(int(m))+":"+path;}
bool hasFlashStore(){return false;}
Result initializeBlankFlash(Cancel){return Result::Ok;}
Result mount(Medium){return offline?Result::Unavailable:Result::Ok;}
Result mkdir(Medium,const char*){return Result::Ok;}
Result freeBytes(Medium,uint64_t& n){n=space;return Result::Ok;}
Result read(Medium m,const char* path,uint8_t* out,size_t cap,size_t& n){n=0;if(ioRead)return Result::Io;auto it=files.find(key(m,path));if(it==files.end())return Result::Missing;
 if(it->second.size()>cap)return Result::TooLarge;n=it->second.size();if(n)memcpy(out,it->second.data(),n);return Result::Ok;}
Result write(Medium m,const char* path,const uint8_t* in,size_t n,Cancel cancel){if(cancel&&cancel())return Result::Cancelled;
 ++writes;auto& bytes=files[key(m,path)];size_t actual=writes==failWrite?std::min(n,cut):n;bytes.assign(in,in+actual);
 if(corruptWrite&&actual)bytes.back()^=1;
 return writes==failWrite||ioClose?Result::Io:Result::Ok;}
Result remove(Medium m,const char* path){return files.erase(key(m,path))?Result::Ok:Result::Missing;}
Result listDirs(Medium m,const char* path,char* out,size_t n,size_t stride,bool& overflow){memset(out,0,n*stride);overflow=false;
 if(enumerateCatalog){
  std::string prefix=key(m,path);if(*path)prefix+="/";std::set<std::string> names;
  for(const auto& file:files)if(file.first.compare(0,prefix.size(),prefix)==0){
   auto rest=file.first.substr(prefix.size());auto slash=rest.find('/');if(slash!=std::string::npos)names.insert(rest.substr(0,slash));
  }
  size_t at=0;for(const auto& name:names){if(at==n){overflow=true;break;}assert(name.size()<stride);strcpy(out+stride*at++,name.c_str());}
 }
 if(fatCatalog&&m==Medium::Sd&&n){if(!*path)strcpy(out,"LOCAL");else if(!strcmp(path,"LOCAL"))strcpy(out,"COUNTER");}return Result::Ok;}
} namespace sd {
bool mounted(){return false;}bool exists(const char*){return false;}
bool fileInfo(const char*,size_t&,uint32_t&){return false;}size_t readFile(const char*,char*,size_t){return 0;}
int listDirs(const char*,char*,int,int){return 0;}
} }
namespace widgets {namespace sync {
Result downloadResult=Result::Unavailable;
Result download(core::app_store::Download&,Package& out,Cancel cancel){
 if(pauseInDownload){pauseInDownload=false;pauseDownload=true;}
 if(cancel&&cancel())return Result::Cancelled;
 if(downloadResult!=Result::Ok)return downloadResult;
 return pack("qr-app","QR App","1","function widget.start() end",27,"{}",2,out);
}
bool workerAvailable=true;bool startPolling(){return workerAvailable;}void requestPoll(){}
int syncFromBackend(char*,size_t){return 0;}
int syncOne(const char*,const char*,char*,size_t,services::custom_apps::Identity*){return 0;}
}}
bool cancel(){return ++checkpoints==cancelPoint;}
bool workerPaused(){bool value=pauseDownload;if(clearPauseOnRead)pauseDownload=false;return value;}
Identity identity(){Identity i;i.medium=Medium::Flash;strcpy(i.id,"counter");return i;}
void resetFaults(){offline=ioRead=ioClose=corruptWrite=false;writes=failWrite=cancelPoint=checkpoints=0;cut=SIZE_MAX;space=1024*1024;noMemory=false;}
Package a,b;
void expectVersion(const Identity& id,const char* version){Package p;assert(load(id,p)==Result::Ok);View v;assert(inspect(p.bytes,p.size,v)==Result::Ok);assert(!strcmp(v.version,version));}
int main(){
 assert(validId("counter-1")&&!validId("../counter")&&!validId("Counter")&&!validId("a/b")&&!validId(""));
 assert(pack("counter","Counter","1","function widget.start() end",27,"{}",2,a)==Result::Ok);
 assert(pack("counter","Counter","2","function widget.start() end",27,"{}",2,b)==Result::Ok);
 View v;assert(inspect(a.bytes,a.size,v)==Result::Ok);assert(v.sourceBytes==27&&!strcmp(v.id,"counter"));
 for(size_t n=0;n<a.size;++n)assert(inspect(a.bytes,n,v)!=Result::Ok);
 Bytes damaged(a.bytes,a.bytes+a.size);damaged.back()^=1;assert(inspect(damaged.data(),damaged.size(),v)==Result::Invalid);
 auto id=identity();Package p;assert(load(id,p)==Result::Missing);
 assert(install(id,a.bytes,a.size)==Result::Ok);expectVersion(id,"1");auto baseline=files;
 {auto sd=id;sd.medium=Medium::Sd;assert(install(sd,a.bytes,a.size)==Result::Ok);
  fatCatalog=true;Catalog c;assert(catalog(c)==Result::Ok&&c.count==1);
  assert(c.entries[0].identity.medium==Medium::Sd&&!strcmp(c.entries[0].identity.origin,"local")&&!strcmp(c.entries[0].identity.id,"counter")&&!strcmp(c.entries[0].name,"Counter"));
  fatCatalog=false;files=baseline;}
 {files.clear();enumerateCatalog=true;
  for(size_t i=0;i<kCatalogEntries+1;++i){auto item=id;snprintf(item.origin,sizeof(item.origin),"%064x",unsigned(i+1));
   assert(install(item,a.bytes,a.size)==Result::Ok);Catalog c;assert(catalog(c)==Result::Ok);
   assert(c.count==std::min(i+1,kCatalogEntries));assert(c.overflow==(i>=kCatalogEntries));
  }
  enumerateCatalog=false;files=baseline;
 }
 writes=0;assert(install(id,a.bytes,a.size)==Result::Ok);assert(writes==0);
 // Every truncated byte of both inactive package write and activation record.
 for(int operation=1;operation<=2;++operation){size_t end=operation==1?b.size:80;
   for(size_t n=0;n<=end;++n){files=baseline;resetFaults();failWrite=operation;cut=n;
     auto r=install(id,b.bytes,b.size);assert(r==(operation==1?Result::Io:Result::Uncertain));resetFaults();
     if(operation==2&&n==80)expectVersion(id,"2");else expectVersion(id,"1");}}
 for(int point=1;point<=3;++point){files=baseline;resetFaults();cancelPoint=point;
   assert(install(id,b.bytes,b.size,cancel)==Result::Cancelled);resetFaults();expectVersion(id,"1");}
 files=baseline;resetFaults();space=1;assert(install(id,b.bytes,b.size)==Result::NoSpace);expectVersion(id,"1");
 resetFaults();offline=true;assert(install(id,b.bytes,b.size)==Result::Unavailable);resetFaults();
 ioRead=true;assert(install(id,b.bytes,b.size)==Result::Io);resetFaults();expectVersion(id,"1");
 noMemory=true;assert(load(id,p)==Result::NoMemory);resetFaults();
 // Corrupting the new data cannot publish it.
 files=baseline;corruptWrite=true;assert(install(id,b.bytes,b.size)==Result::Corrupt);resetFaults();expectVersion(id,"1");
 files=baseline;assert(install(id,b.bytes,b.size)==Result::Ok);expectVersion(id,"2");
 files["1:local/counter/code-1.kapp"].back()^=1;expectVersion(id,"1");
 // Repair must preserve the valid fallback activation record if interrupted.
 auto broken=files;failWrite=2;cut=30;writes=0;assert(install(id,b.bytes,b.size)==Result::Uncertain);resetFaults();expectVersion(id,"1");
 files=broken;assert(install(id,b.bytes,b.size)==Result::Ok);expectVersion(id,"2");
 // Owned loaded bytes survive all later updates and cleanup without a read lease.
 assert(load(id,p)==Result::Ok);Bytes retained(p.bytes,p.bytes+p.size);
 for(int i=0;i<20;++i)assert(install(id,a.bytes,a.size)==Result::Ok);
 assert(!memcmp(p.bytes,retained.data(),p.size));expectVersion(id,"1");
 size_t codeCount=0;for(auto& f:files)if(f.first.find("code-")!=std::string::npos)++codeCount;assert(codeCount<=2);
 // Same ID on another medium or origin does not replace local flash.
 Identity sd=id;sd.medium=Medium::Sd;assert(install(sd,b.bytes,b.size)==Result::Ok);expectVersion(sd,"2");expectVersion(id,"1");
 Identity cloud=id;assert(accountOrigin("https://example.invalid","account",cloud.origin));assert(validIdentity(cloud));
 assert(install(cloud,b.bytes,b.size)==Result::Ok);expectVersion(cloud,"2");expectVersion(id,"1");
 files["1:local/counter/data/save.json"]={'{','}'};
 assert(uninstall(id)==Result::Ok);assert(load(id,p)==Result::Missing);assert(files.count("1:local/counter/data/save.json"));
 assert(install(id,b.bytes,b.size)==Result::Ok);expectVersion(id,"2");
 // Corrupt reinstall after uninstall: a tombstone remains a valid fallback.
 auto saved=files;assert(uninstall(id)==Result::Ok);assert(install(id,a.bytes,a.size)==Result::Ok);
 for(auto& file:files)if(file.first.find("1:local/counter/code-")==0)file.second.back()^=1;
 assert(load(id,p)==Result::Missing);
 auto tornReinstall=files;writes=0;failWrite=2;cut=30;
 assert(install(id,b.bytes,b.size)==Result::Uncertain);resetFaults();assert(load(id,p)==Result::Missing);
 files=tornReinstall;assert(install(id,b.bytes,b.size)==Result::Ok);expectVersion(id,"2");
 // Explicit uninstall must not require executable code or healthy old records.
 for(auto& file:files)if(file.first.find("1:local/counter/code-")==0)file.second.back()^=1;
 assert(uninstall(id)==Result::Ok);assert(load(id,p)==Result::Missing);
 files["1:local/counter/active-0"]={1,2};files["1:local/counter/active-1"]={3,4};
 assert(uninstall(id)==Result::Ok);assert(load(id,p)==Result::Missing);
 assert(files.count("1:local/counter/data/save.json"));
 // A valid future-schema package must not silently downgrade, but can be removed.
 files=baseline;
 auto& future=files["1:local/counter/code-0.kapp"];future[4]=2;
 auto& activation=files["1:local/counter/active-0"];
 auto digest=[](const uint8_t* input,size_t n,uint8_t* output){mbedtls_sha256_context h;
   mbedtls_sha256_init(&h);mbedtls_sha256_starts_ret(&h,0);mbedtls_sha256_update_ret(&h,input,n);
   mbedtls_sha256_finish_ret(&h,output);mbedtls_sha256_free(&h);};
 digest(future.data(),future.size(),activation.data()+16);
 digest(activation.data(),48,activation.data()+48);
 assert(load(id,p)==Result::Incompatible);assert(uninstall(id)==Result::Ok);assert(load(id,p)==Result::Missing);
 files=saved;resetFaults();
 Identity bad=id;strcpy(bad.id,"another");assert(install(bad,a.bytes,a.size)==Result::Invalid);
 // Actual queued job owner, actual vendored Lua parser: syntax check never runs code.
 namespace jobs=widgets::storage;
 Package syntax;assert(pack("counter","Counter","x","error('MUST NOT RUN')",21,nullptr,0,syntax)==Result::Ok);
 assert(jobs::validateLua(syntax.bytes,syntax.size)==Result::Ok);
 Package invalid;assert(pack("counter","Counter","x","function (",10,nullptr,0,invalid)==Result::Ok);
 assert(jobs::validateLua(invalid.bytes,invalid.size)==Result::Invalid);
 widgets::sync::workerAvailable=false;
 assert(!jobs::request(jobs::Operation::Install,id,b.bytes,b.size));
 widgets::sync::workerAvailable=true;
 uint32_t ticket=jobs::request(jobs::Operation::Install,id,b.bytes,b.size);assert(ticket);
 assert(!jobs::request(jobs::Operation::Load,id));
 std::thread worker([]{jobs::process(nullptr);});worker.join();Result result;Package loaded;
 assert(!jobs::take(ticket+1,result));assert(jobs::take(ticket,result,&loaded)&&result==Result::Ok);assert(!jobs::take(ticket,result));
 for(unsigned i=0;i<200;++i){
   ticket=jobs::request(jobs::Operation::Load,id);assert(ticket);
   std::thread reader([]{jobs::process(nullptr);});
   while(!jobs::take(ticket,result,&loaded))std::this_thread::yield();
   reader.join();assert(result==Result::Ok);assert(inspect(loaded.bytes,loaded.size,v)==Result::Ok);
 }
 ticket=jobs::request(jobs::Operation::Remove,id);assert(ticket);jobs::cancel(ticket);jobs::process(nullptr);
 assert(jobs::take(ticket,result)&&result==Result::Cancelled);expectVersion(id,"2");
 ticket=jobs::request(jobs::Operation::Load,id);jobs::abandon(ticket);jobs::process(nullptr);
 ticket=jobs::request(jobs::Operation::Load,id);assert(ticket);jobs::process(nullptr);jobs::abandon(ticket);
 ticket=jobs::request(jobs::Operation::Load,id);assert(ticket);jobs::process(nullptr);assert(jobs::take(ticket,result));
 enumerateCatalog=true;Catalog listingResult;listingResult.count=7;
 ticket=jobs::request(jobs::Operation::Catalog);assert(ticket);noMemory=true;jobs::process(nullptr);noMemory=false;
 assert(jobs::take(ticket,result,nullptr,&listingResult)&&result==Result::NoMemory&&listingResult.count==0);
 ticket=jobs::request(jobs::Operation::Catalog);assert(ticket);jobs::process(nullptr);
 assert(jobs::take(ticket,result,nullptr,&listingResult)&&result==Result::Ok&&listingResult.count>0);
 ticket=jobs::request(jobs::Operation::Catalog);assert(ticket);jobs::process(nullptr);jobs::abandon(ticket);
 ticket=jobs::request(jobs::Operation::Catalog);assert(ticket);jobs::cancel(ticket);jobs::process(nullptr);
 assert(jobs::take(ticket,result,nullptr,&listingResult)&&result==Result::Cancelled&&listingResult.count==0);
 enumerateCatalog=false;
 // QR descriptor validation and actual queued install, with controlled transport.
 core::app_store::Download parsed;
 std::string qr="origami-app:https://apps.example.com/app.kapp#sha256="+std::string(64,'a');
 assert(core::app_store::parse(qr.c_str(),parsed));
 for(const auto& bad: {std::string(""),qr+"a",std::string("origami-app:http://apps.example.com/a#sha256=")+std::string(64,'a'),
     std::string("origami-app:https://user@host/a#sha256=")+std::string(64,'a'),
     std::string("origami-app:https://host/a#sha256=")+std::string(64,'g')}){
  assert(!core::app_store::parse(bad.c_str(),parsed));assert(!jobs::requestDownload(bad.c_str(),jobs::DownloadTarget::Flash));
 }
 ticket=jobs::requestDownload(qr.c_str(),jobs::DownloadTarget::Flash);assert(ticket&&jobs::networkPending());
 jobs::process(nullptr);assert(jobs::take(ticket,result)&&result==Result::Unavailable);
 widgets::sync::downloadResult=Result::Ok;
 ticket=jobs::requestDownload(qr.c_str(),jobs::DownloadTarget::Flash);assert(ticket);
 jobs::reportDownloadProgress(5,10);assert(jobs::downloadProgress(ticket).percent==50);
 assert(jobs::downloadProgress(ticket+1).phase==jobs::DownloadProgress::Phase::Preparing);
 pauseDownload=true;jobs::process(workerPaused);assert(jobs::downloadProgress(ticket).phase==jobs::DownloadProgress::Phase::Preparing);assert(jobs::pending()&&!jobs::take(ticket,result));
 pauseDownload=false;jobs::process(workerPaused);assert(jobs::take(ticket,result)&&result==Result::Ok);
 ticket=jobs::requestDownload(qr.c_str(),jobs::DownloadTarget::Flash);assert(ticket);
 pauseInDownload=true;clearPauseOnRead=true;jobs::process(workerPaused);clearPauseOnRead=false;assert(jobs::pending()&&!jobs::take(ticket,result));
 pauseDownload=false;jobs::process(workerPaused);assert(jobs::take(ticket,result)&&result==Result::Ok);
 ticket=jobs::requestDownload(qr.c_str(),jobs::DownloadTarget::Flash);assert(ticket);
 pauseDownload=true;jobs::process(workerPaused);jobs::cancel(ticket);jobs::process(workerPaused);
 assert(jobs::take(ticket,result)&&result==Result::Cancelled);pauseDownload=false;
 ticket=jobs::requestDownload(qr.c_str(),jobs::DownloadTarget::Flash);assert(ticket);jobs::cancel(ticket);jobs::process(nullptr);
 assert(jobs::take(ticket,result)&&result==Result::Cancelled);
 ticket=jobs::requestDownload(qr.c_str(),jobs::DownloadTarget::Flash);assert(ticket);jobs::process(nullptr);
 Identity installed;assert(jobs::take(ticket,result,nullptr,nullptr,&installed)&&result==Result::Ok);
 assert(strcmp(installed.origin,"local")&& !strcmp(installed.id,"qr-app"));expectVersion(installed,"1");
 ticket=jobs::requestDownload(qr.c_str());assert(ticket);jobs::process(nullptr);
 assert(jobs::take(ticket,result,nullptr,nullptr,&installed)&&result==Result::Ok);
 assert(installed.medium==Medium::Sd);expectVersion(installed,"1");
 // Real USB adapter drives the real job owner and store, without a serial device.
 Print serial;
 auto command=[&](const std::string& line){serial.output.clear();assert(tooling::handleCustomAppCommand(line.c_str(),serial));return serial.output;};
 enumerateCatalog=true;noMemory=true;assert(command("apps list 11111111").find("ERR no_memory")!=std::string::npos);noMemory=false;
 assert(!jobs::pending());
 assert(command("apps list 11111111").find("pending ok")!=std::string::npos);jobs::process(nullptr);
 assert(command("apps status 11111111").find("APPS ENTRY")!=std::string::npos);
 assert(command("apps status 11111111").find("APPS ENTRY")!=std::string::npos);
 enumerateCatalog=false;
 auto begin=std::string("apps begin 12345678 flash ")+std::to_string(a.size);
 assert(command(begin).find("receiving ok 0")!=std::string::npos);
 std::string first;
 for(size_t offset=0;offset<a.size;offset+=96){std::string encoded;char byte[3];
   for(size_t i=offset;i<std::min(offset+96,a.size);++i){snprintf(byte,sizeof(byte),"%02x",a.bytes[i]);encoded+=byte;}
   std::string chunk="apps chunk 12345678 "+std::to_string(offset)+" "+encoded;
   if(!offset)first=chunk;
   assert(command(chunk).find("receiving ok ")!=std::string::npos);
 }
 assert(command(first).find("receiving ok ")!=std::string::npos); // exact replay
 assert(command("apps chunk 12345678 0 ff").find("ERR replay")!=std::string::npos);
 assert(command("apps commit 12345678").find("pending ok")!=std::string::npos);
 jobs::process(nullptr);
 assert(command("apps status 12345678").find("complete ok")!=std::string::npos);expectVersion(id,"1");
 assert(command("apps commit 12345678").find("complete ok")!=std::string::npos);assert(!jobs::pending());
 assert(command("apps load 22345678 flash local counter").find("pending ok")!=std::string::npos);jobs::process(nullptr);
 assert(command("apps status 22345678").find("complete ok")!=std::string::npos);
 assert(command("apps get 22345678 0").find("APPS DATA 22345678 0 4b415050")!=std::string::npos);
 assert(command("apps remove 32345678 flash local counter").find("pending ok")!=std::string::npos);
 assert(command("apps cancel 32345678").find("pending ok")!=std::string::npos);jobs::process(nullptr);
 assert(command("apps status 32345678").find("complete cancelled")!=std::string::npos);expectVersion(id,"1");
 assert(command("apps download 32345679 flash "+qr).find("pending ok")!=std::string::npos);
 jobs::process(nullptr);assert(command("apps status 32345679").find("complete ok")!=std::string::npos);
 assert(command("apps download 32345679 flash "+qr).find("complete ok")!=std::string::npos); // receipt replay
 assert(command("apps download 3234567a flash http://bad").find("ERR busy_or_invalid_qr")!=std::string::npos);
 assert(command("apps begin 42345678 flash 100").find("receiving ok")!=std::string::npos);
 tooling::serviceCustomAppCommands(99); // UI cached timestamp can predate millis() in begin.
 assert(command("apps status 42345678").find("receiving ok")!=std::string::npos);
 tooling::serviceCustomAppCommands(30101);
 assert(command("apps status 42345678").find("complete cancelled")!=std::string::npos);
 puts("PASS production USB adapter: chunk replay, install, receipt replay, export and cooperative cancellation");
 puts("PASS actual worker jobs: Lua syntax-only validation, single admission, completion ownership, cancellation and concurrent handoff");
 puts("PASS production app store: framing, hashes, every interrupted write byte, fallback repair, cancellation, capacity, snapshots, namespaces and saved-data preservation");
}
''')
    sanitizer='thread' if args.thread_sanitizer else 'address,undefined'
    lua=list((ROOT/'lib/lua/src').glob('*.c'))
    lua=[f for f in lua if f.name not in ('lua.c','luac.c')]
    subprocess.run(['cc','-g','-fsanitize='+sanitizer,'-I'+str(ROOT/'lib/lua/src'),'-c',*map(str,lua)],cwd=p,check=True)
    cmd=['c++','-std=c++17','-g','-fsanitize='+sanitizer,'-pthread','-Wno-deprecated-declarations','-I'+str(p),'-I'+str(ROOT/'include'),'-I'+str(JSON),'-I'+str(ROOT/'lib/lua/src'),str(ROOT/'src/services/custom_apps.cpp'),str(ROOT/'src/widgets/storage.cpp'),str(ROOT/'src/tooling/custom_app_commands.cpp'),*map(str,p.glob('*.o')),str(p/'main.cpp'),'-o',str(p/'test')]
    if sys.platform!='darwin':cmd.append('-lcrypto')
    subprocess.run(cmd,check=True);subprocess.run([str(p/'test')],check=True)
