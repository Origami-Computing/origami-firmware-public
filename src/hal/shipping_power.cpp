#include "hal/shipping_power.h"
#include "hal/hal_bus.h"
#include "Pins.h"
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

namespace hal { namespace shipping {
namespace {
constexpr uint8_t address=0x22;
constexpr uint8_t outputs[3]={0x9c,0x84,0x78};
constexpr uint8_t levels[3]={0,0,0x40}; // camera PWDN asserted; all enables low
bool writeReg(uint8_t reg,uint8_t value) {
    Wire.beginTransmission(address); Wire.write(reg); Wire.write(value);
    return Wire.endTransmission()==0;
}
bool readReg(uint8_t reg,uint8_t& value) {
    Wire.beginTransmission(address); Wire.write(reg);
    if(Wire.endTransmission(false)!=0 || Wire.requestFrom(address,uint8_t(1))!=1) return false;
    value=Wire.read(); return true;
}
}
bool prepare() {
    hal::bus::lockI2c();
    Wire.begin(SDA_PIN,SCL_PIN); Wire.setTimeOut(50);
    bool ok=true;
    for(uint8_t p=0;p<3;++p) ok=writeReg(4+p,levels[p]) && ok;
    for(uint8_t p=0;p<3;++p) ok=writeReg(12+p,uint8_t(~outputs[p])) && ok;
    hal::bus::unlockI2c();
    // Drop direct pins before releasing any existing deep-sleep holds.
    pinMode(LCD_RESET_PIN,OUTPUT); digitalWrite(LCD_RESET_PIN,LOW);
    pinMode(LCD_CS_PIN,OUTPUT); digitalWrite(LCD_CS_PIN,HIGH);
    pinMode(SD_CS,OUTPUT); digitalWrite(SD_CS,HIGH);
    gpio_hold_en((gpio_num_t)LCD_RESET_PIN);
    gpio_hold_en((gpio_num_t)LCD_CS_PIN);
    gpio_hold_en((gpio_num_t)SD_CS);
    gpio_deep_sleep_hold_en();
    pinMode(IO_EXPANDER_INT_B_PIN,INPUT_PULLUP);
    return ok && outputsOff();
}
bool read(Inputs& out) {
    uint8_t a=0,b=0,c=0;
    hal::bus::lockI2c();
    bool ok=readReg(0,a) && readReg(1,b) && readReg(2,c);
    hal::bus::unlockI2c();
    if(!ok) return false;
    out.buttons=(!(b&(1u<<5))?1:0)|(!(b&(1u<<6))?2:0)|
                (!(b&(1u<<4))?4:0)|(!(b&(1u<<3))?8:0);
    out.usb=!(c&(1u<<1));
    return true;
}
bool outputsOff() {
    hal::bus::lockI2c(); bool ok=true;
    for(uint8_t p=0;p<3;++p) {
        uint8_t latch=0,dir=0;
        ok=readReg(4+p,latch) && readReg(12+p,dir) && ok;
        ok=ok && ((latch&outputs[p])==levels[p]) && ((dir&outputs[p])==0);
    }
    hal::bus::unlockI2c(); return ok;
}
void sleep(uint32_t retrySeconds,bool expectedUsb) {
    Inputs unused{}; const bool ok=read(unused); // acknowledge all ports
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if(!ok || digitalRead(IO_EXPANDER_INT_B_PIN)==LOW) retrySeconds=30;
    else if(esp_sleep_enable_ext0_wakeup((gpio_num_t)IO_EXPANDER_INT_B_PIN,0)!=ESP_OK) retrySeconds=30;
    if(ok && unused.usb!=expectedUsb) retrySeconds=1;
    if(retrySeconds) esp_sleep_enable_timer_wakeup(uint64_t(retrySeconds)*1000000ULL);
    Serial.printf("[shipping] sleeping retry_s=%lu\n",(unsigned long)retrySeconds);
    Serial.flush(); esp_deep_sleep_start();
}
} }
