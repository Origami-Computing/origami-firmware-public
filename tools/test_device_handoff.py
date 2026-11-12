"""Run the real scrub service against a faultable SD tree and NVS namespaces."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
FAKE=r'''
#pragma once
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
struct String:std::string{using std::string::string;String(const std::string&s):std::string(s){}};
struct Logger{void flush(){}template<class...T>void printf(const char*,T...){}};inline Logger Serial;
inline bool restarted=false,radioBusy=false,sdMounted=true,receiptWrites=true,cleanMarked=false;
struct Esp{void restart(){restarted=true;}};inline Esp ESP;
struct Entry{bool dir;std::string data;};inline std::map<std::string,Entry> fs;
inline std::string removeFail;
class File{std::string path;size_t offset=0,cursor=0;std::vector<std::string> children;public:File(){}File(const std::string&p):path(p){if(fs.count(path)&&fs[path].dir){std::string prefix=path=="/"?"/":path+"/";for(auto&e:fs)if(e.first!=path&&e.first.compare(0,prefix.size(),prefix)==0&&e.first.find('/',prefix.size())==std::string::npos)children.push_back(e.first);}}operator bool()const{return fs.count(path);}bool isDirectory(){return fs[path].dir;}const char*name(){return path.c_str();}void close(){}File openNextFile(){return cursor<children.size()?File(children[cursor++]):File();}bool available(){return fs.count(path)&&offset<fs[path].data.size();}int read(uint8_t*b,size_t n){auto&v=fs[path].data;n=std::min(n,v.size()-offset);memcpy(b,v.data()+offset,n);offset+=n;return n;}};
struct Sd{File open(const String&p){return File(p);}bool exists(const String&p){return fs.count(p);}bool remove(const String&p){if(p==removeFail)return false;return fs.erase(p);}bool rmdir(const String&p){return remove(p);}};inline Sd SD;
inline std::map<std::string,std::map<std::string,std::string>> nvs;
class Preferences{std::string ns;public:bool begin(const char*p,bool){ns=p;return true;}void end(){}bool isKey(const char*k){return nvs[ns].count(k);}bool remove(const char*k){return nvs[ns].erase(k);}size_t putBool(const char*k,bool b){if(!receiptWrites)return 0;nvs[ns][k]=b?"1":"0";return 1;}bool getBool(const char*k,bool b){return isKey(k)?nvs[ns][k]=="1":b;}};
constexpr int ESP_OK=0,NVS_TYPE_ANY=0,NVS_READWRITE=1;
using nvs_handle_t=std::string*;struct Iterator{std::vector<std::string> names;size_t i=0;};using nvs_iterator_t=Iterator*;struct nvs_entry_info_t{char namespace_name[16];};
inline nvs_iterator_t nvs_entry_find(const char*,const char*,int){auto*p=new Iterator;for(auto&n:nvs)if(!n.second.empty())p->names.push_back(n.first);if(p->names.empty()){delete p;return nullptr;}return p;}
inline void nvs_entry_info(nvs_iterator_t p,nvs_entry_info_t*i){strncpy(i->namespace_name,p->names[p->i].c_str(),sizeof(i->namespace_name));}
inline nvs_iterator_t nvs_entry_next(nvs_iterator_t p){if(++p->i<p->names.size())return p;delete p;return nullptr;}
inline void nvs_release_iterator(nvs_iterator_t p){delete p;}
inline int nvs_open(const char*n,int,nvs_handle_t*h){*h=new std::string(n);return 0;}inline int nvs_erase_all(nvs_handle_t h){nvs[*h].clear();return 0;}inline int nvs_commit(nvs_handle_t){return 0;}inline void nvs_close(nvs_handle_t h){delete h;}
struct mbedtls_sha256_context{uint32_t hash=0;};inline void mbedtls_sha256_init(mbedtls_sha256_context*p){p->hash=0;}inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context*,int){return 0;}inline int mbedtls_sha256_update_ret(mbedtls_sha256_context*p,const unsigned char*b,size_t n){for(size_t i=0;i<n;++i)p->hash=p->hash*31+b[i];return 0;}inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context*p,uint8_t*out){for(int i=0;i<32;++i)out[i]=p->hash>>(8*(i%4));return 0;}inline void mbedtls_sha256_free(mbedtls_sha256_context*){}
namespace hal{namespace sd{inline bool mounted(){return sdMounted;}}namespace bus{inline int locks=0;inline void lockSpi(){assert(!locks);++locks;}inline void unlockSpi(){assert(locks==1);--locks;}}}
namespace core{namespace watchdog{inline void feed(){}inline void unsubscribe(){}}}
namespace services{namespace ota_client{enum class ReleaseChannel{Public,Private};inline ReleaseChannel channel=ReleaseChannel::Private;inline ReleaseChannel releaseChannel(){return channel;}}namespace background_network{class RadioPause{public:enum class Result{Acquired,Busy};Result acquire(){return radioBusy?Result::Busy:Result::Acquired;}};}namespace manufacturing{inline bool markClean(){cleanMarked=true;return true;}}namespace ota_access{constexpr int kKeySize=65;inline bool read(char(&key)[kKeySize]){key[0]=0;auto s=nvs["secrets"]["ota_access"];if(s.size()!=64)return false;memcpy(key,s.c_str(),65);return true;}inline bool store(const char*k){nvs["secrets"]["ota_access"]=k;return true;}}}
'''
MAIN=r'''
#include "fake.h"
#include "services/device_handoff.h"
void reset(){restarted=radioBusy=cleanMarked=false;sdMounted=receiptWrites=true;removeFail.clear();fs={{"/",{true,""}},{"/boot",{true,""}},{"/boot/intro.oani",{false,"firmware asset"}},{"/photos",{true,""}},{"/photos/a.jpg",{false,"personal photo"}},{"/photos/b.jpg",{false,"another photo"}},{"/recordings",{true,""}},{"/recordings/a.wav",{false,"personal voice"}},{"/notes",{true,""}},{"/notes/a.txt",{false,"personal note"}},{"/queue.json",{false,"pending upload"}}};nvs={{"factory",{{"accepted","all"},{"wifi_ssid","private"}}},{"manufacturing",{{"report","current report"}}},{"imu_cal",{{"bias","calibration"}}},{"phy",{{"cal","radio"}}},{"secrets",{{"ota_access",std::string(64,'a')},{"api_key","user credential"}}},{"alarms",{{"one","alarm"}}},{"onboarding",{{"offered","1"}}}};}
int main(){
 reset();assert(services::device_handoff::scrubAndRestart());assert(restarted&&cleanMarked&&!hal::bus::locks);assert(fs.size()==3&&fs.count("/boot/intro.oani"));assert(nvs["secrets"].size()==1&&nvs["secrets"]["ota_access"]==std::string(64,'a'));assert(nvs["factory"].size()==1&&nvs["factory"].count("accepted"));assert(nvs["manufacturing"].count("report")&&nvs["imu_cal"].count("bias")&&nvs["phy"].count("cal"));assert(nvs["alarms"].empty()&&nvs["onboarding"].empty());assert(nvs["station"]["clean"]=="1");
 reset();nvs["secrets"].erase("ota_access");assert(!services::device_handoff::scrubAndRestart());assert(!restarted&&fs.count("/photos/a.jpg"));
 services::ota_client::channel=services::ota_client::ReleaseChannel::Public;assert(services::device_handoff::scrubAndRestart());assert(restarted&&cleanMarked&&nvs["secrets"]["ota_access"].empty());
 services::ota_client::channel=services::ota_client::ReleaseChannel::Private;
 reset();removeFail="/photos/a.jpg";assert(services::device_handoff::scrubAndRestart());assert(restarted&&!cleanMarked&&fs.count("/photos/a.jpg"));assert(nvs["station"]["clean"]=="0");assert(nvs["secrets"].count("api_key"));
 reset();radioBusy=true;assert(!services::device_handoff::scrubAndRestart());assert(!restarted&&fs.count("/photos/a.jpg"));
 reset();sdMounted=false;assert(!services::device_handoff::scrubAndRestart());assert(!restarted);
 reset();receiptWrites=false;assert(!services::device_handoff::scrubAndRestart());assert(!restarted&&fs.count("/photos/a.jpg"));
}
'''
with tempfile.TemporaryDirectory() as folder:
 d=Path(folder);(d/'fake.h').write_text(FAKE)
 for name in ('Arduino.h','SD.h','Preferences.h','nvs.h','mbedtls/sha256.h','services/background_network.h','services/ota_access.h','services/ota_client.h','services/manufacturing.h','hal/hal_sd.h','hal/hal_bus.h','core/watchdog.h'):
  p=d/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "fake.h"\n')
 (d/'main.cpp').write_text(MAIN)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(d),'-I'+str(ROOT/'include'),str(ROOT/'src/services/device_handoff.cpp'),str(d/'main.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
print('PASS actual scrub owner: recursive personal-data deletion, assets/calibration/report/OTA preservation, failed deletion/receipt and busy/missing-SD refusal')
