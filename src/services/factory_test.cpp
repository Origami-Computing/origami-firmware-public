#include "services/factory_test.h"
#include "services/wifi_provision.h"
#include "core/secrets.h"
#include "core/store.h"
#include "hal/hal_imu.h"
#include "hal/hal_power.h"
#include "hal/hal_sd.h"
#include "hal/hal_wifi.h"
#include "services/ota_client.h"
#include "services/ota_access.h"
#include "services/tls_trust.h"
#include "services/manufacturing.h"
#include "core/board_time.h"
#include "core/watchdog.h"
#include "core/menu_config.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <soc/esp32s3/spiram.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
namespace services { namespace factory_test {
const char* name(Check c) {
    static const char* names[] = {"Flash + PSRAM", "SD storage", "Battery + USB", "Buttons + encoder", "Motion sensors", "Display colors", "Speaker", "Vibration", "Microphone", "Camera frames", "Office Wi-Fi", "Internet + TLS", "Backend access", "OTA download", "Camera quality", "Recording playback"};
    return static_cast<unsigned>(c) < kCount ? names[static_cast<unsigned>(c)] : "Results";
}
bool completed() { return core::store::getU32("factory", "accepted", 0) == kAll; }
bool saveReport(uint32_t passed, uint32_t failed, bool accepted) {
    if (accepted && (passed != kAll || failed)) return false;
    // Invalidate acceptance first so a partial write cannot certify a new run.
    return core::store::putU32("factory", "accepted", 0) &&
           core::store::putU32("factory", "passed", passed) &&
           core::store::putU32("factory", "failed", failed) &&
           (!accepted || core::store::putU32("factory", "accepted", kAll));
}
bool configureWifi(const char* ssid, const char* password) {
    if (!ssid || !password || !*ssid || strlen(ssid)>32 || strlen(password)>63) return false;
    if (!core::secrets::addWifiNetwork(ssid, password)) return false;
    Preferences p;
    if (!p.begin("factory", false)) return false;
    bool ok = p.putString("wifi_ssid", ssid) == strlen(ssid); p.end();
    // Joining is deferred until the Wi-Fi test, after camera teardown.
    return ok;
}
void Session::close() {
    recording_.close(); playback_.closeSession(); network_.close();
    if(recordingPath_[0]&&!playback_.isOpen()){hal::sd::removeFile(recordingPath_);recordingPath_[0]=0;}
    mic_.close(); audioReservation_.release();
    camera_.stop();
    if (cameraHeld_) camera_.restoreUi(camera::Session::Resume::DeferredWithInputRecovery);
    else camera_.releaseBackground();
    cameraHeld_ = false;
    if (gyro_) hal::imu::releaseGyroscope();
    gyro_ = false; check_ = Check::Count;
}
bool Session::samples(const int16_t* data, uint32_t count, void* context) {
    auto& s = *static_cast<Session*>(context);
    // Ignore codec startup transient before measuring speech.
    if (millis()-s.started_ < 1000) return true;
    s.samples_ += count;
    for (uint32_t i=0;i<count;++i) s.peak_ = std::max(s.peak_, std::abs(int(data[i])));
    return true;
}
void Session::start(Check check, uint32_t now) {
    close();
    if(playback_.isOpen()){check_=Check::Count;passed_=false;failed_=true;snprintf(detail_,sizeof(detail_),"Playback cleanup pending; retry");return;}
    check_=check; started_=now; sampled_=0; samples_=frames_=0;
    peak_=movingSamples_=0; baseline_=passed_=failed_=false;
    phase_=0;operatorReady_=false;
    snprintf(detail_,sizeof(detail_),"Checking...");
    if (check==Check::Memory) {
        auto* block=static_cast<uint32_t*>(heap_caps_malloc(4096,MALLOC_CAP_SPIRAM));
        bool ok=block && esp_spiram_get_size()>=8*1024*1024 && ESP.getFlashChipSize()>=16*1024*1024;
        if(block){volatile uint32_t* probe=block;for(unsigned i=0;i<1024;++i)probe[i]=0x5aa50000u^i;
            for(unsigned i=0;i<1024;++i)ok=ok && probe[i]==(0x5aa50000u^i);heap_caps_free(block);}
        passed_=ok;failed_=!ok;snprintf(detail_,sizeof(detail_),"Flash %uMB / PSRAM %uMB",ESP.getFlashChipSize()/1048576,unsigned(esp_spiram_get_size()/1048576));
    } else if(check==Check::Storage) {
        const char* path="/.factory-test-tmp"; char readback[32]={};
        const char payload[]="Kami SD read/write check";
        // Never replace an existing file, including a prior interrupted test.
        bool ok=hal::sd::mounted() && !hal::sd::exists(path);
        bool wrote=false;
        if(ok){wrote=true;ok=hal::sd::writeFile(path,reinterpret_cast<const uint8_t*>(payload),sizeof(payload)-1);}
        if(ok)ok=hal::sd::readFile(path,readback,sizeof(readback))==sizeof(payload)-1 && !strcmp(readback,payload);
        if(wrote)ok=hal::sd::removeFile(path) && ok;
        passed_=ok;failed_=!ok;snprintf(detail_,sizeof(detail_),"%s",ok?"Read / write / delete verified":"SD missing, I/O error or test file exists");
    } else if(check==Check::Motion) {
        gyro_=hal::imu::acquireGyroscope();failed_=!gyro_;
        snprintf(detail_,sizeof(detail_),"Tilt and rotate the board");
    } else if(check==Check::Microphone) {
        bool held=audioReservation_.acquire(true)==background_network::PeripheralReservation::Result::Acquired;
        failed_=!held || !mic_.open(samples,this);
        started_=millis();snprintf(detail_,sizeof(detail_),"Speak near the microphone");
    } else if(check==Check::Camera) {
        cameraHeld_=camera_.prepareCapture(false)==camera::Session::PrepareResult::Ready;
        failed_=!cameraHeld_ || !camera_.start(HAL_CAMERA_FRAMESIZE_QVGA,HAL_CAMERA_PIXFORMAT_JPEG);
        started_=millis();snprintf(detail_,sizeof(detail_),"%s",failed_?"Camera unavailable / init failed":"Reading JPEG frames...");
    } else if(check==Check::Wifi) {
        Preferences p;ssid_[0]=0;
        if(p.begin("factory",true)){p.getString("wifi_ssid",ssid_,sizeof(ssid_));p.end();}
        failed_=!*ssid_ || !wifi_provision::connectSaved(ssid_);
        snprintf(detail_,sizeof(detail_),"%s",failed_?"Provision office Wi-Fi over USB":"Waiting for office network + DHCP");

    } else if(check==Check::InternetTls||check==Check::Backend||check==Check::Ota) {
        failed_=network_.reserveNetwork()!=firmware_update::Session::Result::Ready;
        snprintf(detail_,sizeof(detail_),"Waiting for Wi-Fi and network time");
    } else if(check==Check::AudioQuality) {
        snprintf(detail_,sizeof(detail_),"A: record a take. B: play it back.");
    }
    if(failed_) { close(); }
}
bool Session::audioRecord(uint32_t now) {
    if(check_!=Check::AudioQuality||failed_)return false;
    operatorReady_=false;
    if(phase_==1){
        elapsed_=now-started_;
        bool captured=recording_.samplesWritten()>=32000;
        bool saved=recording_.finish();recording_.release();recording_.close();
        failed_=!captured||!saved||!audio::WavPlayer::probeTrack(recordingPath_);
        phase_=2;snprintf(detail_,sizeof(detail_),failed_?"Take too short / save failed. Retry.":"Take saved. B: play recording");
        if(failed_)close();return !failed_;
    }
    playback_.closeSession();if(playback_.isOpen())return false;
    if(recordingPath_[0]){hal::sd::removeFile(recordingPath_);recordingPath_[0]=0;}
    recording_.resetWaveform();elapsed_=0;
    failed_=recording_.begin("/recordings","FACTORY",recordingPath_,sizeof(recordingPath_),audio::WavCapture::CodecCheck::Required,true)!=audio::WavCapture::StartResult::Started;
    phase_=1;started_=now;snprintf(detail_,sizeof(detail_),failed_?"Recording unavailable. Retry.":"Recording... A: stop (15s maximum)");
    if(failed_)close();return !failed_;
}
bool Session::audioPlay() {
    if(check_!=Check::AudioQuality||phase_!=2||failed_)return false;
    operatorReady_=false;
    failed_=!playback_.openSession()||!playback_.startTrack(recordingPath_);
    phase_=3;snprintf(detail_,sizeof(detail_),failed_?"Playback failed. Retry.":"Playing your recording...");
    if(failed_)close();return !failed_;
}
unsigned Session::audioSeconds(uint32_t now) const {return (phase_==1?now-started_:elapsed_)/1000;}
void Session::audioWave(char* out) const {
    for(unsigned i=0;i<27;++i)out[i]='A'+(recording_.waveform().data()[i]*25+254)/255;
    out[27]=0;
}
void Session::tick(uint32_t now) {
    if(passed_ || failed_)return;
    if(check_==Check::Microphone)mic_.poll();
    if(check_==Check::AudioQuality && phase_==1 && !recording_.drain()){failed_=true;close();return;}
    if(now-sampled_<100)return;sampled_=now;
    if(check_==Check::AudioQuality){
        if(phase_==1 && now-started_>=15000)audioRecord(now);
        if(phase_==3 && playback_.consumeWantsNextTrack()){
            playback_.closeSession();operatorReady_=true;phase_=2;
            snprintf(detail_,sizeof(detail_),"Heard clear speech? C: pass");
        }
    }else if(check_==Check::InternetTls||check_==Check::Backend||check_==Check::Ota){
        core::time::service(now);
        if(hal::wifi::connected()&&core::time::isSynced()){
            if(check_==Check::Ota){
                if(phase_==0){ota_client::ReleaseInfo release{};core::watchdog::unsubscribe();bool ok=ota_client::fetchLatestRelease(ota_client::ReleaseChannel::Production,&release);core::watchdog::subscribe();
                    if(ok)ok=ota_client::beginFlash(release);failed_=!ok;phase_=1;snprintf(detail_,sizeof(detail_),"%s",ok?"Verifying signed production download":ota_client::lastError());}
                if(!failed_){int written=0,total=0;bool busy=ota_client::pumpFlash(&written,&total);if(!busy){passed_=total>0&&written==total&&!ota_client::lastError()[0];failed_=!passed_;if(passed_)manufacturing::markOnline(ota_access::role()==ota_access::Role::Production);}}
            }else if(phase_==0){
                WiFiClientSecure client;tls::useRootBundle(client);HTTPClient http;http.setConnectTimeout(5000);http.setTimeout(5000);http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
                const auto backend=core::secrets::backendConfig();String url=check_==Check::Backend?String(backend.url)+"/health":String(OTA_BASE_URL)+"/access";
                core::watchdog::unsubscribe();bool began=http.begin(client,url);int code=began?http.GET():0;http.end();core::watchdog::subscribe();
                passed_=check_==Check::Backend?code==200:(code==200||code==401||code==403);failed_=!passed_;phase_=1;snprintf(detail_,sizeof(detail_),"Verified TLS / HTTP %d",code);
            }
        }
    }else if(check_==Check::Motion) {
        int32_t a[3],g[3];
        if(hal::imu::readAxes(a,g)){
            int span=0,rotation=0;
            for(int i=0;i<3;++i){if(!baseline_)low_[i]=high_[i]=a[i];low_[i]=std::min(low_[i],a[i]);high_[i]=std::max(high_[i],a[i]);span=std::max(span,int(high_[i]-low_[i]));rotation=std::max(rotation,std::abs(int(g[i])));}
            baseline_=true;if(rotation>20000)++movingSamples_;
            passed_=span>500 && movingSamples_>=3;
            snprintf(detail_,sizeof(detail_),"Accel span %d mg / gyro samples %d",span,movingSamples_);
        }
    }else if(check_==Check::Power){
        auto status=hal::power::read();hal::power::FuelGaugeTelemetry gauge{};
        bool fresh=hal::power::readFuelGauge(gauge);
        passed_=fresh && gauge.voltage_mv>=2800 && gauge.voltage_mv<=4400 && hal::power::externalPowerPresent(status);
        snprintf(detail_,sizeof(detail_),"Battery %umV / gauge %s / USB %s",status.battery_mv,fresh?"OK":"missing",hal::power::externalPowerPresent(status)?"OK":"missing");
    }else if(check_==Check::Microphone){
        passed_=samples_>=32000 && peak_>=800 && peak_<32760;
        snprintf(detail_,sizeof(detail_),"Samples %lu / peak %d",(unsigned long)samples_,peak_);
    }else if(check_==Check::Camera){
        hal::camera::CameraFrame frame{};
        if(camera_.read(&frame)){
            if(frame.buf && frame.len>100 && frame.width && frame.height && frame.format==HAL_CAMERA_PIXFORMAT_JPEG && frame.buf[0]==0xff && frame.buf[1]==0xd8)++frames_;
            camera_.returnFrame();
        }
        passed_=frames_>=3;snprintf(detail_,sizeof(detail_),"Valid JPEG frames: %lu / 3",(unsigned long)frames_);
    }else if(check_==Check::Wifi){
        passed_=hal::wifi::connected() && WiFi.SSID()==ssid_ && hal::wifi::getSTAIP().addr!=0;
        if(passed_)snprintf(detail_,sizeof(detail_),"Office connected / RSSI %d dBm",int(WiFi.RSSI()));
    }
    if(check_!=Check::AudioQuality && now-started_>(check_==Check::Ota?180000u:45000u) && !passed_ && !operatorReady_)failed_=true;
    if(passed_||failed_)close();
}
} }
