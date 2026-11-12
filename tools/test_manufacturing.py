"""Compile the actual manufacturing owner against atomic NVS and image fakes."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FAKE = r'''
#pragma once
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
struct String:std::string {using std::string::string;String(const std::string&s):std::string(s){}};
struct Logger{void print(const char*){}void println(){}template<class...T>void printf(const char*,T...){}};inline Logger Serial;
inline std::map<std::string,std::map<std::string,std::vector<uint8_t>>> nvs;
inline bool writes=true,opens=true,token=false,ota=true,official=true;inline int wifi=0;
class Preferences {std::string ns;public:bool getBool(const char*,bool d){return d;}uint32_t getUInt(const char*,uint32_t d){return d;}bool begin(const char*n,bool){ns=n;return opens;}void end(){}size_t getBytesLength(const char*k){return nvs[ns][k].size();}size_t getBytes(const char*k,void*out,size_t cap){auto&v=nvs[ns][k];if(v.size()!=cap)return 0;memcpy(out,v.data(),cap);return cap;}size_t putBytes(const char*k,const void*p,size_t n){if(!writes)return 0;const auto*b=(const uint8_t*)p;nvs[ns][k]=std::vector<uint8_t>(b,b+n);return n;}bool clear(){if(!writes)return false;nvs[ns].clear();return true;}};
struct Description {uint8_t app_elf_sha256[32]={1};};inline Description description;
inline const Description*esp_ota_get_app_description(){return &description;}
struct esp_partition_t{uint32_t size=0x600000;};inline esp_partition_t partition;
inline const esp_partition_t*esp_ota_get_running_partition(){return &partition;}
constexpr int ESP_OK=0;inline int esp_partition_read(const esp_partition_t*,uint32_t,void*p,size_t n){memset(p,0,n);return 0;}
struct mbedtls_sha256_context{};inline uint8_t imageByte=0xaa;
inline void mbedtls_sha256_init(mbedtls_sha256_context*){}inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context*,int){return 0;}inline int mbedtls_sha256_update_ret(mbedtls_sha256_context*,const unsigned char*,size_t){return 0;}inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context*,uint8_t*out){memset(out,imageByte,32);return 0;}inline void mbedtls_sha256_free(mbedtls_sha256_context*){}
inline std::map<std::string,size_t> files;
inline std::map<std::string,std::vector<std::string>> dirs;
inline int busDepth=0,peerRuns=0,heldOps=0,maxHeldOps=0;
inline void sdOperation(){assert(busDepth==1);++heldOps;}
inline void vTaskDelay(int){assert(busDepth==0);++peerRuns;}
class File{std::string path;size_t next=0;public:File(const std::string&p=""):path(p){}size_t size(){sdOperation();return files[path];}operator bool()const{return files.count(path)!=0||dirs.count(path)!=0;}bool isDirectory(){sdOperation();return dirs.count(path)!=0;}File openNextFile(){sdOperation();auto&children=dirs[path];return next<children.size()?File(children[next++]):File();}const char*name(){sdOperation();return path.c_str();}void close(){sdOperation();}bool available(){return false;}int read(uint8_t*,size_t){return 0;}};
struct Sd{File open(const String&p){sdOperation();return File(p);}};inline Sd SD;
namespace hal{namespace sd{inline bool mounted(){return true;}}namespace bus{inline void lockSpi(){assert(busDepth==0);++busDepth;heldOps=0;}inline void unlockSpi(){assert(busDepth==1);if(heldOps>maxHeldOps)maxHeldOps=heldOps;--busDepth;}}}
namespace core{namespace secrets{struct Backend{char apiKey[96]={};};inline Backend backend;inline Backend backendConfig(){return backend;}inline bool hasDeviceToken(){return token;}inline int wifiNetworkCount(){return wifi;}}namespace watchdog{inline void feed(){}}}
namespace services{namespace ota_access{enum class Role{Unknown,Production,Developer};inline Role role(){return Role::Production;}}namespace ota_client{enum class ReleaseChannel{Public,Private,Staging};inline ReleaseChannel channel=ReleaseChannel::Private;inline ReleaseChannel releaseChannel(){return channel;}inline bool hasAuthToken(){return ota;}inline bool isOfficialBuild(){return official;}}namespace onboarding_progress{struct Progress{unsigned page=0;bool completed=false;bool offered=false;};inline Progress progress;inline Progress load(){return progress;}}namespace audio{namespace media_volume{constexpr uint16_t kSpeakerMaxGainBasisPoints=2000;}}}
#define OTA_FIRMWARE_VERSION "v0.1.213"
'''
MAIN = r'''
#include "fake.h"
#include "services/manufacturing.h"
#include "src/services/manufacturing.cpp"
using namespace services::manufacturing;
int main(){
 uint32_t filesCount=0;files["/telemetry_queue/journal.jsonl"]=0;assert(countFiles("/telemetry_queue/journal.jsonl",filesCount)&&filesCount==0);files["/telemetry_queue/journal.jsonl"]=1;assert(countFiles("/telemetry_queue/journal.jsonl",filesCount)&&filesCount==1);files["/photos/empty.jpg"]=0;assert(countFiles("/photos/empty.jpg",filesCount)&&filesCount==2);
 dirs["/"]={"/photos"};dirs["/photos"]={"/photos/nested"};
 for(int i=0;i<1000;++i){auto name="/photos/nested/"+std::to_string(i);files[name]=42;dirs["/photos/nested"].push_back(name);}
 filesCount=0;assert(countFiles("/photos",filesCount)&&filesCount==1000);
 assert(peerRuns>=1000&&maxHeldOps<=4&&busDepth==0);
 printInventory();assert(busDepth==0);
 filesCount=0;assert(!countFiles("/photos/missing",filesCount)&&busDepth==0);
 assert(!accepted()&&!readyToArm());assert(begin());uint32_t p=0,f=0;assert(report(p,f)&&p==0&&f==0);
 for(unsigned i=0;i<kChecks;++i)assert(result(i,true));assert(accepted());assert(!readyToArm());
 assert(!verifyImage(100,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
 assert(!verifyImage(1024,"invalid"));
 assert(verifyImage(1024,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
 assert(markOnline(true));assert(markClean());assert(readyToArm());assert(markUnlock());
 using services::ota_client::ReleaseChannel;
 services::ota_client::channel=ReleaseChannel::Public;ota=false;
 assert(!readyToArm());assert(markPublicOnline());assert(readyToArm());
 services::ota_client::channel=ReleaseChannel::Private;assert(!readyToArm());assert(!markPublicOnline());
 ota=true;assert(!readyToArm());assert(markOnline(true));assert(readyToArm());

 assert(!readyToArm(true));assert(markBench());assert(!readyToArm());assert(markClean());official=false;
 assert(readyToArm(true)&&!readyToArm());ota=false;assert(!readyToArm(true));ota=true;
 assert(verifyImage(1024,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));official=true;
 assert(!readyToArm(true));assert(markClean());assert(readyToArm());
 token=true;assert(!readyToArm());token=false;core::secrets::backend.apiKey[0]='x';assert(!readyToArm());core::secrets::backend.apiKey[0]=0;
 wifi=1;assert(!readyToArm());wifi=0;official=false;assert(!readyToArm());official=true;assert(tutorialReady());ota=false;assert(!readyToArm());ota=true;
 services::onboarding_progress::progress.page=1;assert(readyToArm());services::onboarding_progress::progress.page=0;assert(readyToArm());
 official=false;
 assert(!readyToArm());
 assert(readyToArm(false,true)==core::release_policy::integrationCandidate);
 assert(!readyToArm(true,true));official=true;
 assert(invalidate(3));assert(!accepted()&&!readyToArm());assert(result(3,false));assert(report(p,f)&&!(p&(1u<<3))&&(f&(1u<<3)));
 writes=false;assert(!result(3,true));assert(!accepted()&&!report(p,f));writes=true;assert(begin());for(unsigned i=0;i<kChecks;++i)assert(result(i,true));assert(accepted());
 assert(markOnline(true)&&markClean());imageByte=0xbb;assert(!verifyImage(1024,"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));assert(!readyToArm());
 description.app_elf_sha256[0]=2;assert(!accepted()&&!report(p,f)&&!readyToArm());assert(begin());assert(report(p,f)&&p==0&&f==0);
 assert(begin());assert(!setManualStep(0,true)&&!setManualStep(6,true));
 assert(setManualStep(1,true)&&setManualStep(2,true)&&setManualStep(3,true));assert(manualSteps()==7);
 services::onboarding_progress::progress.page=18;assert(!accepted()&&readyToArm());services::onboarding_progress::progress.page=0; // Overrides permit the action without fabricating measured passes.
 assert(markUnlock());assert(setManualStep(4,true)&&setManualStep(5,true)&&manualSteps()==31);
 assert(setManualStep(3,false)&&manualSteps()==3);assert(!readyToArm());
 assert(setManualStep(3,true));description.app_elf_sha256[0]=3;assert(manualSteps()==0&&!readyToArm());
 assert(setManualStep(1,true)&&manualSteps()==1);assert(setManualStep(2,true));
 assert(result(0,true)&&manualSteps()==1);assert(begin()&&manualSteps()==0);
 writes=false;assert(!setManualStep(1,true)&&manualSteps()==0);writes=true;assert(begin());
 opens=false;assert(!accepted()&&!begin());opens=true;
 nvs["manufacturing"]["report"].resize(1);assert(!accepted());assert(begin());
 nvs["manufacturing"]["report"][0]=9;assert(!accepted());
}
'''


def main():
    with tempfile.TemporaryDirectory() as folder:
        d = Path(folder)
        (d / 'fake.h').write_text(FAKE)
        for name in ('Arduino.h', 'Preferences.h', 'SD.h', 'esp_ota_ops.h', 'esp_app_format.h',
                     'mbedtls/sha256.h', 'services/ota_access.h', 'services/ota_client.h',
                     'services/onboarding_progress.h', 'services/media_volume.h', 'core/secrets.h',
                     'core/menu_config.h', 'core/watchdog.h', 'hal/hal_sd.h', 'hal/hal_bus.h'):
            path = d / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('#include "fake.h"\n')
        (d / 'main.cpp').write_text(MAIN)
        for candidate in (0,1):
            subprocess.run(['c++', '-std=c++17', '-DORIGAMI_PRODUCTION_BUILD=1', f'-DORIGAMI_INTEGRATION_CANDIDATE={candidate}', '-fsanitize=address,undefined',
                            '-I' + str(d), '-I' + str(ROOT / 'include'), '-I' + str(ROOT),
                            str(d / 'main.cpp'), '-o', str(d / 'test')], check=True)
            subprocess.run([str(d / 'test')], check=True)
    print('PASS actual manufacturing owner: persisted masks, failed writes, stale builds, image mismatch, corruption and shipping gates')


if __name__ == '__main__':
    main()
