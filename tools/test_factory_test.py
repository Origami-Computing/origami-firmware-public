"""Exercise the production factory hardware owner with faultable host devices."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
FAKE = r"""
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <map>
inline uint32_t now=0; inline unsigned gyroUsers=0,cameraUsers=0,reservations=0,restores=0;
inline bool cameraOk=true,frameOk=true,micOk=true,storeOk=true,sdOk=true,wifiConnected=false;
inline bool gaugeOk=true,usb=true,moved=false; inline int peak=1600;
inline std::string network="office",savedSsid="office",sdData;
inline std::map<std::string,uint32_t> records;
inline unsigned millis(){return now;}
struct Esp { unsigned getPsramSize(){return 8*1048576-4096;} unsigned getFlashChipSize(){return 16*1048576;} };inline Esp ESP;
inline size_t physicalPsram=8*1048576;
inline size_t esp_spiram_get_size(){return physicalPsram;}
constexpr int MALLOC_CAP_SPIRAM=1;
inline void* heap_caps_malloc(size_t n,int){return malloc(n);} inline void heap_caps_free(void*p){free(p);}
class Preferences {public:bool begin(const char*,bool){return storeOk;}void end(){} size_t putString(const char*,const char*s){savedSsid=s;return strlen(s);}size_t getString(const char*,char*out,size_t n){strncpy(out,savedSsid.c_str(),n);return savedSsid.size();}};
struct Wifi { std::string SSID(){return network;} int RSSI(){return -45;} };inline Wifi WiFi;
#define HAL_CAMERA_FRAMESIZE_QVGA 5
#define HAL_CAMERA_PIXFORMAT_JPEG 4
namespace hal {
namespace camera {struct CameraFrame {const uint8_t*buf;size_t len;uint16_t width,height;int format;};}
namespace imu {inline bool acquireGyroscope(){++gyroUsers;return true;}inline void releaseGyroscope(){--gyroUsers;}inline bool readAxes(int32_t*a,int32_t*g){for(int i=0;i<3;++i){a[i]=moved?1000:0;g[i]=moved?30000:0;}return true;}}
namespace power {struct Status{uint16_t battery_mv=3800;};struct FuelGaugeTelemetry{uint16_t voltage_mv=3800;};inline Status read(){return {};}inline bool readFuelGauge(FuelGaugeTelemetry&){return gaugeOk;}inline bool externalPowerPresent(Status){return usb;}}
namespace sd {inline bool mounted(){return sdOk;}inline bool exists(const char*){return !sdData.empty();}inline bool writeFile(const char*,const uint8_t*p,size_t n){sdData.assign((const char*)p,n);return sdOk;}inline size_t readFile(const char*,char*b,size_t n){strncpy(b,sdData.c_str(),n);return sdData.size();}inline bool removeFile(const char*){sdData.clear();return sdOk;}}
namespace wifi {inline bool connected(){return wifiConnected;}struct Ip{unsigned addr;};inline Ip getSTAIP(){return {wifiConnected?1u:0u};}}
}
namespace core {namespace secrets {inline bool addWifiNetwork(const char*,const char*){return storeOk;}} namespace store {inline uint32_t getU32(const char*,const char*k,uint32_t def){return records.count(k)?records[k]:def;}inline bool putU32(const char*,const char*k,uint32_t v){if(!storeOk)return false;records[k]=v;return true;}}}
namespace services {
namespace wifi_provision {inline bool connectSaved(const char*){return true;}}
namespace background_network {class PeripheralReservation {bool held=false;public:enum class Result{Acquired};Result acquire(bool){if(!held){held=true;++reservations;}return Result::Acquired;}void release(){if(held){held=false;--reservations;}}};}
namespace camera {class Session {bool held=false,live=false;public:enum class PrepareResult{Ready};enum class Resume{DeferredWithInputRecovery};PrepareResult prepareCapture(bool){held=true;++reservations;return PrepareResult::Ready;}bool start(int,int){live=cameraOk;if(live)++cameraUsers;return live;}void stop(){if(live){--cameraUsers;live=false;}}void releaseBackground(){if(held){--reservations;held=false;}}void restoreUi(Resume){++restores;releaseBackground();}bool read(hal::camera::CameraFrame*f){static uint8_t bytes[128]={0xff,0xd8};*f={bytes,128,320,240,4};return frameOk;}void returnFrame(){}};}
namespace audio {class MicrophoneStream {public:using Sink=bool(*)(const int16_t*,uint32_t,void*);Sink sink=nullptr;void*user=nullptr;bool open(Sink s,void*u){sink=micOk?s:nullptr;user=u;return micOk;}void close(){sink=nullptr;}void poll(){static int16_t b[16000];for(auto&v:b)v=peak;if(sink)sink(b,16000,user);}};}
}
"""
FAKE += r"""
using String = std::string;
inline bool switchDown=false,downloadStarted=false,downloadComplete=false,backendHealthy=true;
namespace hal {namespace io {inline bool readPowerSwitchLevel(bool& down){down=switchDown;return true;}}}
namespace core {namespace time {inline bool isSynced(){return true;}inline void service(uint32_t){}}namespace watchdog {inline void unsubscribe(){}inline void subscribe(){}}}
namespace core {namespace secrets {struct Backend{char url[64]="https://backend.invalid";};inline Backend backendConfig(){return {};}}}
namespace services {namespace manufacturing {inline int onlineProof=0;inline bool markOnline(bool p){onlineProof=p?2:1;return true;}inline bool markPublicOnline(){onlineProof=3;return true;}}namespace tls {template<class T>inline void useRootBundle(T&){}}namespace ota_access {enum class Role{Unknown,Production,Developer};inline Role role(){return Role::Production;}}}
namespace services {namespace ota_client {enum class ReleaseChannel{Public,Production=Public,Staging,Private};inline ReleaseChannel channel=ReleaseChannel::Public;inline ReleaseChannel releaseChannel(){return channel;}struct ReleaseInfo{};inline bool fetchLatestRelease(ReleaseChannel,ReleaseInfo*){return true;}inline bool beginFlash(const ReleaseInfo&){downloadStarted=true;return true;}inline bool pumpFlash(int*w,int*t){*w=downloadComplete?100:0;*t=100;return !downloadComplete;}inline const char*lastError(){return "";}}}
namespace services {namespace firmware_update {class Session{public:enum class Result{Ready};Result reserveNetwork(){return Result::Ready;}void close(){}};}namespace audio {
class WavCapture{public:enum class CodecCheck{Required};enum class StartResult{Started};StartResult begin(const char*,const char*,char*p,size_t,CodecCheck,bool){strcpy(p,"/recordings/test.wav");return StartResult::Started;}bool drain(){return true;}bool finish(){return true;}void release(){}void close(){}void resetWaveform(){}struct Wave{const uint8_t*data()const{static uint8_t d[27]={};return d;}};Wave waveform()const{return {};}uint32_t samplesWritten(){return 48000;}};
class WavPlayer{public:bool isOpen(){return false;}void closeSession(){}static bool probeTrack(const char*){return true;}bool openSession(){return true;}bool startTrack(const char*){return true;}bool consumeWantsNextTrack(){return true;}};}}
class WiFiClientSecure{};
constexpr int HTTPC_DISABLE_FOLLOW_REDIRECTS=0;
#define OTA_BASE_URL "https://ota.invalid"
class HTTPClient{public:void setConnectTimeout(int){}void setTimeout(int){}void setFollowRedirects(int){}bool begin(WiFiClientSecure&,const String&){return true;}int GET(){return backendHealthy?200:500;}void end(){}};
"""
MAIN = r"""
#include "services/factory_test.h"
#include <cassert>
using namespace services::factory_test;
int main(){
 assert(!saveReport(kAll-1,0,true));assert(!completed());
 assert(saveReport(kAll,0,true)&&completed());storeOk=false;
 assert(!saveReport(0,1,false));storeOk=true;assert(saveReport(0,1,false)&&!completed());
 assert(configureWifi("office","synthetic") && savedSsid=="office");
 assert(!configureWifi("","synthetic"));
 Session s;
 wifiConnected=true;
 s.start(Check::Backend,0);s.tick(100);assert(s.passed());backendHealthy=false;s.start(Check::Backend,0);s.tick(100);assert(s.failed());backendHealthy=true;
 s.start(Check::Ota,0);s.tick(100);assert(downloadStarted&&!s.passed());downloadComplete=true;s.tick(200);assert(s.passed());
 now=0;s.start(Check::AudioQuality,0);s.tick(4100);assert(!s.operatorReady()&&s.audioPhase()==0&&!s.audioPlay());
 assert(s.audioRecord(5000)&&s.audioPhase()==1);s.tick(6000);assert(!s.operatorReady()&&!s.audioPlay());
 assert(s.audioRecord(9000)&&s.audioPhase()==2);assert(!s.operatorReady());assert(s.audioPlay());s.tick(10000);assert(s.operatorReady()&&!s.passed());
 assert(s.audioRecord(11000)&&!s.operatorReady());s.close();assert(!s.audioPlay());

 s.start(Check::Memory,0);assert(s.passed());
 physicalPsram=4*1048576;s.start(Check::Memory,0);assert(s.failed());physicalPsram=8*1048576;
 s.start(Check::Storage,0);assert(s.passed()&&sdData.empty());
 sdData="customer file";s.start(Check::Storage,0);assert(s.failed()&&sdData=="customer file");sdData.clear();
 s.start(Check::Power,0);s.tick(100);assert(s.passed());
 gaugeOk=false;s.start(Check::Power,0);s.tick(46000);assert(s.failed());gaugeOk=true;
 s.start(Check::Motion,0);assert(gyroUsers==1);s.tick(100);moved=true;
 s.tick(200);s.tick(300);s.tick(400);assert(s.passed()&&gyroUsers==0);
 s.start(Check::Motion,0);s.close();assert(gyroUsers==0);
 cameraOk=false;s.start(Check::Camera,0);assert(s.failed()&&reservations==0&&cameraUsers==0&&restores==1);
 cameraOk=true;s.start(Check::Camera,0);s.tick(100);s.tick(200);s.tick(300);
 assert(s.passed()&&reservations==0&&cameraUsers==0&&restores==2);
 frameOk=false;s.start(Check::Camera,0);s.tick(46000);assert(s.failed()&&reservations==0&&cameraUsers==0);
 frameOk=true;s.start(Check::Camera,0);s.close();assert(!reservations&&!cameraUsers);
 micOk=false;s.start(Check::Microphone,0);assert(s.failed()&&!reservations);
 micOk=true;now=0;s.start(Check::Microphone,0);now=2000;s.tick(now);now=2100;s.tick(now);assert(s.passed()&&!reservations);
 now=0;peak=32767;s.start(Check::Microphone,0);now=2000;s.tick(now);now=46000;s.tick(now);assert(s.failed()&&!reservations);
 now=0;s.start(Check::Wifi,0);wifiConnected=true;network="wrong";s.tick(100);assert(!s.passed());network="office";s.tick(200);assert(s.passed());
 wifiConnected=false;s.start(Check::Wifi,0);s.tick(46000);assert(s.failed());
}
"""
def main():
    with tempfile.TemporaryDirectory() as folder:
        d=Path(folder)
        (d/'fake.h').write_text(FAKE)
        for name in ['services/camera_session.h','services/microphone_stream.h','services/peripheral_reservation.h','services/wifi_provision.h','services/firmware_update.h','services/wav_capture.h','services/wav_player.h','services/ota_client.h','services/ota_access.h','services/tls_trust.h','services/manufacturing.h','core/board_time.h','core/watchdog.h','core/menu_config.h','hal/hal_io.h','HTTPClient.h','WiFiClientSecure.h','core/secrets.h','core/store.h','hal/hal_imu.h','hal/hal_power.h','hal/hal_sd.h','hal/hal_wifi.h','Preferences.h','WiFi.h','esp_heap_caps.h','soc/esp32s3/spiram.h']:
            p=d/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#include "fake.h"\n')
        (d/'main.cpp').write_text(MAIN)
        subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(d),'-I'+str(ROOT/'include'),str(ROOT/'src/services/factory_test.cpp'),str(d/'main.cpp'),'-o',str(d/'test')],check=True)
        subprocess.run([str(d/'test')],check=True)
    print('PASS factory checks, failure/timeout cleanup, wrong-network rejection, SD preservation and acceptance persistence')
if __name__=='__main__':main()
