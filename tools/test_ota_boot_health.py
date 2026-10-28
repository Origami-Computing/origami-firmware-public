#!/usr/bin/env python3
"""Link the actual Arduino early-acceptance block against our OTA health owner."""
from pathlib import Path
import argparse, subprocess, tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--arduino-core', type=Path, default=Path.home()/'.platformio/packages/framework-arduinoespressif32/cores/esp32/esp32-hal-misc.c')
a = p.parse_args()
root = Path(__file__).resolve().parents[1]
source = (root/'src/services/ota_client.cpp').read_text()
sdk = a.arduino_core.read_text()
def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

hook = function(source, 'extern "C" bool verifyRollbackLater()') if 'extern "C" bool verifyRollbackLater()' in source else ''
weak = sdk[sdk.index('bool verifyOta() __attribute__'):sdk.index('#endif', sdk.index('bool verifyOta() __attribute__'))]
startup = sdk[sdk.index('void initArduino()'):sdk.index('    //init proper ref tick', sdk.index('void initArduino()'))] + '}\n'
common = r'''
#pragma once
#include <cstdint>
#include <cassert>
struct esp_partition_t {};
enum esp_ota_img_states_t { ESP_OTA_IMG_UNDEFINED, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID };
using esp_err_t=int;
constexpr int ESP_OK=0;
extern unsigned ticks;
extern int accepted, rolledBack, removed, acceptError;
extern esp_ota_img_states_t state;
const esp_partition_t* esp_ota_get_running_partition();
int esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t*);
int esp_ota_mark_app_valid_cancel_rollback();
int esp_ota_mark_app_invalid_rollback_and_reboot();
#define CONFIG_APP_ROLLBACK_ENABLE 1
#define log_e(...) ((void)0)
'''
app = r'''
#include "common.h"
#include <cstring>
unsigned ticks=0;int accepted=0,rolledBack=0,removed=0,acceptError=0;
esp_ota_img_states_t state=ESP_OTA_IMG_PENDING_VERIFY;
const esp_partition_t* esp_ota_get_running_partition(){static esp_partition_t p;return &p;}
int esp_ota_get_state_partition(const esp_partition_t*,esp_ota_img_states_t*s){*s=state;return ESP_OK;}
int esp_ota_mark_app_valid_cancel_rollback(){++accepted;if(!acceptError)state=ESP_OTA_IMG_VALID;return acceptError;}
int esp_ota_mark_app_invalid_rollback_and_reboot(){++rolledBack;return ESP_OK;}
unsigned millis(){return ticks;}
struct {template<class...T>void printf(const char*,T...){} void println(const char*){}} Serial;
struct Preferences {bool begin(const char*,bool){return true;}void remove(const char*k){assert(!strcmp(k,"github_token"));++removed;}void end(){}};
#define OTA_HARDWARE_ID "fixture"
#define OTA_LAYOUT_ID "fixture"
const char* buildKindName(){return "official";}
static bool s_initialized=false,s_bootPendingVerify=false,s_bootHealthHandled=false;
static uint32_t s_bootHealthStartMs=0;
extern "C" void initArduino();
''' + hook + '\n' + function(source, 'void init()') + '\n' + function(source, 'void serviceBootHealth(bool setupHealthy)') + r'''
void reset(){ticks=0;accepted=rolledBack=removed=acceptError=0;state=ESP_OTA_IMG_PENDING_VERIFY;s_initialized=s_bootPendingVerify=s_bootHealthHandled=false;}
int main(){
 reset();initArduino();assert(accepted==0&&rolledBack==0&&state==ESP_OTA_IMG_PENDING_VERIFY);
 init();ticks=29999;serviceBootHealth(true);assert(accepted==0&&removed==0);
 ticks=30000;serviceBootHealth(true);assert(accepted==1&&state==ESP_OTA_IMG_VALID&&removed==1);
 serviceBootHealth(false);assert(accepted==1&&rolledBack==0);
 reset();initArduino();init();ticks=30000;serviceBootHealth(false);assert(rolledBack==1&&accepted==0&&removed==0);
 reset();state=ESP_OTA_IMG_VALID;initArduino();init();ticks=30000;serviceBootHealth(true);assert(accepted==0&&removed==0);
 reset();initArduino();init();ticks=30000;acceptError=1;serviceBootHealth(true);assert(accepted==1&&removed==0&&s_bootPendingVerify);
 acceptError=0;serviceBootHealth(true);assert(accepted==2&&removed==1&&!s_bootPendingVerify);
 reset();ticks=0xfffffff0u;initArduino();init();ticks+=30000u;serviceBootHealth(true);assert(accepted==1);
}
'''
with tempfile.TemporaryDirectory() as directory:
    d = Path(directory)
    (d/'common.h').write_text(common)
    (d/'sdk.cpp').write_text('#include "common.h"\nextern "C" {\n'+weak+startup+'}\n')
    (d/'app.cpp').write_text(app)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(d/'sdk.cpp'),str(d/'app.cpp'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
print('PASS actual Arduino startup defers acceptance; timed healthy boot, rollback, retry and clock wrap')
