#include "services/shipping.h"
#include "services/manufacturing.h"
#include "core/shipping_gesture.h"
#include "hal/shipping_power.h"
#include "services/background_network.h"
#include "services/automation.h"
#include "services/vibration.h"
#include "hal/hal_audio.h"
#include "hal/hal_ble.h"
#include "hal/hal_camera.h"
#include "hal/hal_sd.h"
#include "hal/hal_wifi.h"
#include "core/watchdog.h"
#include <Arduino.h>
#include <esp_partition.h>
#include <nvs_flash.h>
#include <nvs.h>

namespace services { namespace shipping {
namespace {
constexpr char partition[]="shipping";
constexpr uint32_t normal=0x53485000,locked=0x53485001;
bool open(nvs_handle_t& h,nvs_open_mode_t mode) {
    return nvs_flash_init_partition(partition)==ESP_OK &&
        nvs_open_from_partition(partition,"shipping",mode,&h)==ESP_OK;
}
bool save(uint32_t value) {
    nvs_handle_t h;
    if(!open(h,NVS_READWRITE)) return false;
    bool ok=nvs_set_u32(h,"state",value)==ESP_OK && nvs_commit(h)==ESP_OK;
    uint32_t readback=0;
    ok=ok && nvs_get_u32(h,"state",&readback)==ESP_OK && readback==value;
    nvs_close(h); return ok;
}
}
const char* name(State s) {
    switch(s){case State::Unsupported:return "unsupported";case State::Normal:return "normal";
        case State::Locked:return "locked";default:return "fault";}
}
State state() {
    if(!esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_NVS,partition)) return State::Unsupported;
    if(nvs_flash_init_partition(partition)!=ESP_OK) return State::Fault;
    nvs_handle_t h;
    esp_err_t result=nvs_open_from_partition(partition,"shipping",NVS_READONLY,&h);
    if(result==ESP_ERR_NVS_NOT_FOUND) return State::Normal; // never armed
    if(result!=ESP_OK) return State::Fault;
    uint32_t value=0; result=nvs_get_u32(h,"state",&value); nvs_close(h);
    if(result==ESP_ERR_NVS_NOT_FOUND) return State::Fault; // incomplete record, never silently unlock
    if(result!=ESP_OK) return State::Fault;
    return value==normal?State::Normal:value==locked?State::Locked:State::Fault;
}
bool bootGate() {
    State s=state();
    if(s==State::Normal || s==State::Unsupported) return false;
    Serial.begin(115200); Serial.setTxTimeoutMs(5);
    const bool prepared=hal::shipping::prepare();
    Serial.printf("[shipping] boot state=%s outputs_off=%u\n",name(s),prepared);
    if(!prepared) { hal::shipping::sleep(30); return false; }
    core::shipping::Gesture gesture(millis());
    char command[48]={}; size_t used=0;
    const uint32_t began=millis();
    bool gestureDone=false;
    // USB is maintenance access, NOT authorization to start normal operation.
    // A short boot-only window permits status/recovery without starting drivers.
    while(millis()-began<15000u) {
        hal::shipping::Inputs in{};
        const bool valid=hal::shipping::read(in);
        if(!valid) break;
        if(!gestureDone && s==State::Locked) {
            auto result=gesture.update(in.buttons,millis());
            if(result==core::shipping::Gesture::Result::Accepted) {
                if(save(normal)) { manufacturing::markUnlock(); Serial.println("[shipping] unlocked=gesture"); return true; }
                s=State::Fault;
            }
            gestureDone=result!=core::shipping::Gesture::Result::Pending;
        }
        if(in.usb) {
            while(Serial.available()) {
                char c=Serial.read();
                if(c=='\r') continue;
                if(c=='\n') {
                    command[used]=0;
                    if(!strcmp(command,"shipping status")) Serial.printf("[shipping] state=%s outputs_off=%u buttons=%u usb=%u tutorial_ready=%u\n",name(s),hal::shipping::outputsOff(),in.buttons,in.usb,manufacturing::tutorialReady());
                    if(!strcmp(command,"shipping recover CONFIRM") && save(normal)) {
                        Serial.println("[shipping] unlocked=usb-recovery"); return true;
                    }
                    used=0;
                } else if(used<sizeof(command)-1) command[used++]=c;
                else used=0;
            }
        } else if(gestureDone || s==State::Fault) break;
        delay(10);
    }
    hal::shipping::sleep(); return false;
}
bool enter() {
    if(state()!=State::Normal) return false;
    background_network::RadioPause pause;
    if(pause.acquire()!=background_network::RadioPause::Result::Acquired) return false;
    // Commit before any irreversible rail transition; reset always sees lock.
    if(!save(locked)) return false;
    automation::prepareForBatterySleep(); vibration::stopAll();
    hal::ble::deinit(); hal::wifi::powerDown(); hal::camera::deinit();
    hal::audio::deinitMic(); hal::sd::end();
    const bool prepared=hal::shipping::prepare();
    Serial.printf("[shipping] committed outputs_off=%u\n",prepared);
    core::watchdog::unsubscribe();
    // Restart through the actual early gate; USB maintenance remains reachable
    // for 15 seconds. Battery-only boards reject absent gestures and sleep.
    Serial.flush(); ESP.restart();
    return true;
}
} }
