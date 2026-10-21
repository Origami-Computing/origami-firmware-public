#include "services/device_handoff.h"
#include "services/manufacturing.h"
#include "services/background_network.h"
#include "services/ota_access.h"
#include "hal/hal_sd.h"
#include "hal/hal_bus.h"
#include "core/watchdog.h"
#include <Arduino.h>
#include <SD.h>
#include <Preferences.h>
#include <nvs.h>
#include <cstring>
#include <mbedtls/sha256.h>
namespace services { namespace device_handoff {
namespace {
// Firmware content roots, never user media or synced widget data.
bool keep(const String& p){
    return p=="/boot"||p=="/eyes"||p=="/icons"||p=="/assets"||p=="/fonts"||p=="/origami_logo.bmp";
}
bool keepNamespace(const char* name){return !strcmp(name,"imu_cal")||!strcmp(name,"phy")||!strcmp(name,"factory")||!strcmp(name,"manufacturing");}
String childPath(const String& parent,const char* name){const char* base=strrchr(name,'/');return (parent=="/"?String(""):parent)+"/"+(base?base+1:name);}
bool digestTree(const String& path,mbedtls_sha256_context& hash,unsigned depth=0){
    if(depth>16)return false;
    File file=SD.open(path);if(!file)return false;
    mbedtls_sha256_update_ret(&hash,(const unsigned char*)path.c_str(),path.length());
    if(file.isDirectory()){
        for(;;){File child=file.openNextFile();if(!child)break;String p=childPath(path,child.name());child.close();if(!digestTree(p,hash,depth+1)){file.close();return false;}}
    }else{uint8_t buffer[512];while(file.available()){int n=file.read(buffer,sizeof(buffer));if(n<=0){file.close();return false;}mbedtls_sha256_update_ret(&hash,buffer,n);core::watchdog::feed();}}
    file.close();return true;
}
bool assetsHash(uint8_t output[32]){
    mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);mbedtls_sha256_starts_ret(&hash,0);
    // Fixed traversal roots keep hash order stable as other root entries disappear.
    const char* roots[]={"/boot","/eyes","/icons","/assets","/fonts","/origami_logo.bmp"};bool ok=true;
    for(auto p:roots)if(SD.exists(p)&&!digestTree(p,hash))ok=false;
    mbedtls_sha256_finish_ret(&hash,output);mbedtls_sha256_free(&hash);return ok;
}
bool removeTree(const String& path,unsigned depth=0){
    if(depth>16||path=="/"||keep(path))return false;
    File file=SD.open(path);if(!file)return !SD.exists(path);
    bool dir=file.isDirectory();file.close();
    if(dir){
        // Reopen after each deletion: FAT directory iteration can skip entries
        // when the current entry is removed.
        for(;;){file=SD.open(path);if(!file)return false;File child=file.openNextFile();if(!child){file.close();break;}String p=childPath(path,child.name());child.close();file.close();if(!removeTree(p,depth+1))return false;core::watchdog::feed();}
    }
    return (dir?SD.rmdir(path):SD.remove(path))&&!SD.exists(path);
}
bool scrubSd(){
    uint8_t before[32],after[32];if(!assetsHash(before))return false;
    for(;;){File root=SD.open("/");if(!root)return false;String target;
        for(;;){File child=root.openNextFile();if(!child)break;String path=childPath("/",child.name());child.close();if(!keep(path)){target=path;break;}}
        root.close();if(!target.length())break;if(!removeTree(target))return false;
    }
    return assetsHash(after)&&!memcmp(before,after,sizeof(before));
}
bool scrubNvs(const char* ota){
    // Enumerate namespaces from live entries; restart enumeration after erasure.
    for(;;){nvs_iterator_t it=nvs_entry_find("nvs",nullptr,NVS_TYPE_ANY);char victim[16]={};
        while(it){nvs_entry_info_t info;nvs_entry_info(it,&info);if(!keepNamespace(info.namespace_name)){strncpy(victim,info.namespace_name,15);break;}it=nvs_entry_next(it);}
        nvs_release_iterator(it);if(!victim[0])break;
        nvs_handle_t handle;if(nvs_open(victim,NVS_READWRITE,&handle)!=ESP_OK)return false;
        bool ok=nvs_erase_all(handle)==ESP_OK&&nvs_commit(handle)==ESP_OK;nvs_close(handle);if(!ok)return false;
    }
    Preferences factory;if(!factory.begin("factory",false))return false;
    bool ok=!factory.isKey("wifi_ssid")||factory.remove("wifi_ssid");factory.end();
    if(!ok||!ota_access::store(ota))return false;
    char readback[ota_access::kKeySize];return ota_access::read(readback)&&!strcmp(ota,readback);
}
}
void printStatus(){Preferences p;bool clean=p.begin("station",true)&&p.getBool("clean",false);p.end();Serial.printf("STATION SCRUB clean=%u schema=1\n",clean);}
bool scrubAndRestart(){
    if(!hal::sd::mounted())return false;
    char ota[ota_access::kKeySize];if(!ota_access::read(ota))return false;
    background_network::RadioPause pause;
    if(pause.acquire()!=background_network::RadioPause::Result::Acquired)return false;
    // Invalidate a previous receipt before any deletion.
    Preferences receipt;if(!receipt.begin("station",false))return false;
    if(receipt.putBool("clean",false)!=1){receipt.end();return false;}receipt.end();
    hal::bus::lockSpi();bool sdOk=scrubSd();hal::bus::unlockSpi();
    bool nvsOk=sdOk&&scrubNvs(ota);
    if(nvsOk)nvsOk=manufacturing::markClean();memset(ota,0,sizeof(ota));
    bool saved=false;if(receipt.begin("station",false)){saved=receipt.putBool("clean",sdOk&&nvsOk)==1;receipt.end();}
    Serial.printf("STATION SCRUB %s assets_verified=%u nvs_verified=%u\n",sdOk&&nvsOk&&saved?"OK":"ERR",sdOk,nvsOk);
    // No old in-RAM user settings or worker queues may repopulate cleared storage.
    Serial.flush();core::watchdog::unsubscribe();ESP.restart();return true;
}
} }
