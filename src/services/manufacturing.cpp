#include "services/manufacturing.h"
#include "services/ota_access.h"
#include "services/ota_client.h"
#include "services/onboarding_progress.h"
#include "services/media_volume.h"
#include "core/secrets.h"
#include "core/release_policy.h"
#include "core/menu_config.h"
#include "core/watchdog.h"
#include "hal/hal_sd.h"
#include "hal/hal_bus.h"
#include <SD.h>
#include <Arduino.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <mbedtls/sha256.h>
#include <cstring>
namespace services { namespace manufacturing {
namespace {
bool writeFailed=false;
struct Record {
 uint32_t schema=3, passed=0, failed=0;
 uint8_t elf[32]={};
 char testedVersion[32]={};
 uint32_t online=0, clean=0, unlocked=0;
 uint32_t imageVerified=0, imageBytes=0;
 uint8_t imageHash[32]={};
 uint32_t bench=0;
};
bool read(Record& r) {
 Preferences p; if(!p.begin("manufacturing",true))return false;
 bool ok=p.getBytesLength("report")==sizeof(r)&&p.getBytes("report",&r,sizeof(r))==sizeof(r);p.end();
 return ok&&r.schema==3&&!(r.passed&~kAll)&&!(r.failed&~kAll)&&!(r.passed&r.failed)&&r.online<=2&&r.clean<=1&&r.unlocked<=1&&r.imageVerified<=1&&r.bench<=1&&memchr(r.testedVersion,0,sizeof(r.testedVersion));
}
bool current(const Record& r){return !writeFailed&&!memcmp(r.elf,esp_ota_get_app_description()->app_elf_sha256,32);}
bool save(const Record& r){Preferences p;if(!p.begin("manufacturing",false)){writeFailed=true;return false;}bool ok=p.putBytes("report",&r,sizeof(r))==sizeof(r);p.end();if(!ok)writeFailed=true;return ok;}
// Keep each SD operation protected, but never hold the bus across traversal.
// A whole-card scan can otherwise starve the network worker's SD access long
// enough to trip its watchdog, even while this UI task feeds its own watchdog.
String nextInventoryPath(File& directory,const String& path){
 hal::bus::lockSpi();
 File child=directory.openNextFile();String result;
 if(child){String name=child.name();const char* base=strrchr(name.c_str(),'/');result=(path=="/"?String(""):path)+"/"+(base?base+1:name.c_str());child.close();}
 hal::bus::unlockSpi();
 core::watchdog::feed();vTaskDelay(1);
 return result;
}
bool countFiles(const String& path,uint32_t& count,unsigned depth=0){
 if(depth>16)return false;
 hal::bus::lockSpi();File f=SD.open(path);
 if(!f){hal::bus::unlockSpi();return false;}
 if(!f.isDirectory()){
  // Only this exact empty system journal is exempt.
  if(path!="/telemetry_queue/journal.jsonl"||f.size()!=0)++count;
  f.close();hal::bus::unlockSpi();return true;
 }
 hal::bus::unlockSpi();
 bool ok=true;
 for(;;){String p=nextInventoryPath(f,path);if(!p.length())break;if(!countFiles(p,count,depth+1)){ok=false;break;}}
 hal::bus::lockSpi();f.close();hal::bus::unlockSpi();return ok;
}
}
uint32_t manualSteps(){
 Record r;if(!read(r)||!current(r))return 0;
 struct Manual {uint8_t elf[32];uint32_t mask;};Manual m{};Preferences p;
 if(!p.begin("manufacturing",true))return 0;
 bool ok=p.getBytesLength("manual")==sizeof(m)&&p.getBytes("manual",&m,sizeof(m))==sizeof(m);p.end();
 return ok&&!memcmp(m.elf,r.elf,32)&&!(m.mask&~31u)?m.mask:0;
}
bool manualStep(unsigned step){return step>=1&&step<=5&&(manualSteps()&(1u<<(step-1)));}
bool setManualStep(unsigned step,bool complete){
 if(step<1||step>5)return false;
 Record r;if(!read(r)||!current(r)){if(!begin()||!read(r)||!current(r))return false;}
 struct Manual {uint8_t elf[32];uint32_t mask;};Manual m{};memcpy(m.elf,r.elf,32);
 // Changing earlier evidence invalidates all subsequent operator overrides.
 m.mask=manualSteps()&((1u<<(step-1))-1u);if(complete)m.mask|=1u<<(step-1);
 Preferences p;if(!p.begin("manufacturing",false))return false;
 bool ok=p.putBytes("manual",&m,sizeof(m))==sizeof(m);p.end();if(!ok)writeFailed=true;return ok;
}
bool begin(){Record r;memcpy(r.elf,esp_ota_get_app_description()->app_elf_sha256,32);snprintf(r.testedVersion,sizeof(r.testedVersion),"%s",OTA_FIRMWARE_VERSION);bool ok=save(r);if(ok){writeFailed=false;ok=setManualStep(1,false);}return ok;}
bool result(unsigned check,bool pass){Record r;if(check>=kChecks||!read(r)||!current(r))return false;uint32_t bit=1u<<check;r.passed&=~bit;r.failed&=~bit;if(pass)r.passed|=bit;else r.failed|=bit;r.clean=r.unlocked=0;return setManualStep(2,false)&&save(r);}
bool invalidate(unsigned check){Record r;if(check>=kChecks||!read(r)||!current(r))return false;uint32_t bit=1u<<check;r.passed&=~bit;r.failed&=~bit;if(check>=11&&check<=13)r.online=0;r.clean=r.unlocked=0;return setManualStep(2,false)&&save(r);}
bool accepted(){Record r;return read(r)&&current(r)&&r.passed==kAll&&!r.failed;}
bool report(uint32_t& passed,uint32_t& failed){Record r;if(!read(r)||!current(r))return false;passed=r.passed;failed=r.failed;return true;}
void printStatus(){
 Record r;bool present=read(r);bool fresh=present&&current(r);
 auto config=core::secrets::backendConfig();auto progress=onboarding_progress::load();
 const char* role=ota_access::role()==ota_access::Role::Production?"production":ota_access::role()==ota_access::Role::Developer?"developer":"unknown";
 Serial.printf("MFG STATUS schema=3 present=%u current=%u passed=%08lx failed=%08lx accepted=%u online=%u clean=%u unlocked=%u api=%u token=%u ota=%u role=%s production=%u tutorial_page=%u tutorial_done=%u speaker_ceiling=%u bench=%u manual=%lu candidate=%u\n",present,fresh,(unsigned long)r.passed,(unsigned long)r.failed,accepted(),fresh?r.online:0,fresh?r.clean:0,fresh?r.unlocked:0,config.apiKey[0]!=0,core::secrets::hasDeviceToken(),ota_client::hasAuthToken(),role,core::release_policy::productionOnly,progress.page,progress.completed,audio::media_volume::kSpeakerMaxGainBasisPoints,fresh?r.bench:0,(unsigned long)manualSteps(),core::release_policy::integrationCandidate);
 Serial.print("MFG BUILD tested=");for(uint8_t b:r.elf)Serial.printf("%02x",b);Serial.print(" running=");for(uint8_t b:esp_ota_get_app_description()->app_elf_sha256)Serial.printf("%02x",b);Serial.printf(" tested_version=%s image_verified=%u bytes=%lu sha256=",present?r.testedVersion:"unknown",fresh?r.imageVerified:0,(unsigned long)r.imageBytes);for(uint8_t b:r.imageHash)Serial.printf("%02x",b);Serial.println();
}
bool printImageHash(uint32_t bytes){
 const esp_partition_t* p=esp_ota_get_running_partition();if(!p||bytes<256||bytes>p->size)return false;
 mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);bool ok=mbedtls_sha256_starts_ret(&hash,0)==0;uint8_t buffer[1024],digest[32];
 for(uint32_t off=0;ok&&off<bytes;){size_t n=bytes-off;if(n>sizeof(buffer))n=sizeof(buffer);ok=esp_partition_read(p,off,buffer,n)==ESP_OK&&mbedtls_sha256_update_ret(&hash,buffer,n)==0;off+=n;core::watchdog::feed();}
 ok=ok&&mbedtls_sha256_finish_ret(&hash,digest)==0;mbedtls_sha256_free(&hash);
 if(ok){Serial.printf("MFG IMAGE bytes=%lu sha256=",(unsigned long)bytes);for(uint8_t b:digest)Serial.printf("%02x",b);Serial.println();}return ok;
}
bool markOnline(bool production){Record r;if(!read(r)||!current(r))return false;r.online=production?2:1;return save(r);}
bool verifyImage(uint32_t bytes,const char* expected){
 Record r;if(!expected||strlen(expected)!=64||!read(r)||!current(r))return false;
 const auto* p=esp_ota_get_running_partition();if(!p||bytes<256||bytes>p->size)return false;
 uint8_t wanted[32];for(unsigned i=0;i<32;++i){unsigned value=0;for(unsigned j=0;j<2;++j){char c=expected[i*2+j];if(c>='0'&&c<='9')value=value*16+c-'0';else if(c>='a'&&c<='f')value=value*16+c-'a'+10;else return false;}wanted[i]=value;}
 r.imageVerified=0;r.bench=0;if(!save(r))return false;
 mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);bool ok=mbedtls_sha256_starts_ret(&hash,0)==0;uint8_t buffer[1024],digest[32];
 for(uint32_t off=0;ok&&off<bytes;){size_t n=bytes-off;if(n>sizeof(buffer))n=sizeof(buffer);ok=esp_partition_read(p,off,buffer,n)==ESP_OK&&mbedtls_sha256_update_ret(&hash,buffer,n)==0;off+=n;core::watchdog::feed();}
 ok=ok&&mbedtls_sha256_finish_ret(&hash,digest)==0;mbedtls_sha256_free(&hash);
 if(!ok||memcmp(wanted,digest,32))return false;
 r.imageVerified=1;r.imageBytes=bytes;memcpy(r.imageHash,digest,32);return save(r);
}
bool markBench(){Record r;if(!read(r)||!current(r)||!r.imageVerified)return false;r.bench=1;r.clean=r.unlocked=0;return save(r);}
bool markClean(){Record r;if(!read(r)||!current(r))return false;r.clean=1;return save(r);}
bool markUnlock(){Record r;if(!read(r)||!current(r)||(!r.clean&&!manualStep(3)))return false;r.unlocked=1;return save(r);}
bool resetTutorial(){Preferences p;if(!p.begin("onboarding",false))return false;bool ok=p.clear();p.end();return ok;}
bool tutorialReady(){Preferences p;if(!p.begin("onboarding",true))return false;bool ready=!p.getBool("offered",false)&&!p.getUInt("guide_v5",0)&&!p.getUInt("guide_v4",0)&&!p.getUInt("guide_v3",0);p.end();return ready;}
bool readyToArm(bool testMode,bool candidateMode){
 Record r;auto c=core::secrets::backendConfig();
 if(!read(r)||!current(r)|| (candidateMode&&(!core::release_policy::integrationCandidate||!core::release_policy::productionOnly||testMode)))return false;
 const uint32_t m=manualSteps();
 return ((m&1u)||(r.imageVerified&&(testMode?r.bench==1:(!r.bench&&core::release_policy::productionOnly&&(ota_client::isOfficialBuild()||candidateMode)))))
 &&((m&2u)||(r.passed==kAll&&!r.failed&&r.online==2&&ota_client::hasAuthToken()))
 &&((m&4u)||(r.clean&&!c.apiKey[0]&&!core::secrets::hasDeviceToken()&&!core::secrets::wifiNetworkCount()));
 // The shipping control resets tutorial progress after app teardown and refuses
 // to arm if that reset fails. Prior tutorial progress must not prevent re-arm.
}
void printInventory(){
 auto c=core::secrets::backendConfig();uint32_t photos=0,recordings=0,music=0,other=0;bool ok=hal::sd::mounted();
 if(ok){hal::bus::lockSpi();File root=SD.open("/");ok=bool(root);hal::bus::unlockSpi();for(;;){String p=nextInventoryPath(root,"/");if(!p.length())break;if(p=="/boot"||p=="/eyes"||p=="/icons"||p=="/assets"||p=="/fonts"||p=="/origami_logo.bmp")continue;uint32_t* n=p=="/photos"?&photos:p=="/recordings"?&recordings:p=="/music"?&music:&other;if(!countFiles(p,*n))ok=false;}hal::bus::lockSpi();root.close();hal::bus::unlockSpi();}
 Serial.printf("MFG INVENTORY sd=%u scanned=%u photos=%lu recordings=%lu music=%lu other=%lu wifi=%d api=%u token=%u\n",hal::sd::mounted(),ok,(unsigned long)photos,(unsigned long)recordings,(unsigned long)music,(unsigned long)other,core::secrets::wifiNetworkCount(),c.apiKey[0]!=0,core::secrets::hasDeviceToken());
}
} }
