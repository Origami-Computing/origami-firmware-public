"""Verify Camera configuration and the actual app row-to-field save adapters.

Compiles the real settings service, app adapters and HAL Tuning type with an in-memory
store. Covers every byte value, per-field writes and nonpersisted manual state.
This does not emulate NVS or exercise a sensor.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'src/apps/camera_menu_app.cpp').read_text()


def block(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + (';' if marker.startswith('enum ') else '')


functions = ['static int settingsRowCount(', 'static SettingsRow settingsRowAt(', 'static hal::camera::Tuning camPreviewTuning(', 'static void persistTopLevelSetting(', 'static void persistManualSetting(']
code = r'''
#include "hal/hal_camera.h"
#include "services/camera_settings.h"
#include <algorithm>
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
std::map<std::string,unsigned> saved;
std::vector<std::string> reads,writes;
bool writable=true, autofocusSupported=false;
namespace hal {namespace camera {bool supportsAutofocus(){return autofocusSupported;}}}
namespace core {namespace store {
unsigned get(const char* ns,const char* key,unsigned fallback) {
 assert(std::string(ns)=="camera"); reads.push_back(key);
 auto found=saved.find(key);return found==saved.end()?fallback:found->second;
}
uint8_t getU8(const char* ns,const char* key,uint8_t fallback){return get(ns,key,fallback);}
bool getBool(const char* ns,const char* key,bool fallback){return get(ns,key,fallback);}
bool putU8(const char* ns,const char* key,uint8_t value){
 assert(std::string(ns)=="camera");writes.push_back(key);
 if(!writable)return false;saved[key]=value;return true;
}
bool putBool(const char* ns,const char* key,bool value){return putU8(ns,key,value);}
}}
'''
code += '\n'.join(block('enum '+name+' {') for name in ['SettingsRow','ManualRow'])
code += r'''
using CapturePreset=services::camera::CaptureResolution;
using CameraMode=services::camera::Mode;
constexpr auto CM_DEFAULT=CameraMode::Default, CM_MANUAL=CameraMode::Manual;
constexpr auto CR_VGA=CapturePreset::Vga;
services::camera::Settings s_settings;
int s_shootMode=0;
auto& s_cameraMode=s_settings.mode;
auto& s_capturePreset=s_settings.capture;
auto& s_autofocusOn=s_settings.autofocus;
auto& s_manualTuning=s_settings.manual;
void loadCameraPrefs(){s_settings.load();}
hal::camera::Tuning effectiveTuning(){return s_settings.tuning();}
'''+'\n'.join(block(marker) for marker in functions)+r'''
int clamp(int value,int low,int high){return std::max(low,std::min(high,value));}
int main(){
 for(bool supported:{false,true})for(auto mode:{CM_DEFAULT,CM_MANUAL}){
  autofocusSupported=supported;s_settings.mode=mode;
  const std::vector<SettingsRow> base=supported ?
    std::vector<SettingsRow>{SR_MODE,SR_AUTOFOCUS,SR_CAPTURE,SR_VIDEO} :
    std::vector<SettingsRow>{SR_MODE,SR_CAPTURE,SR_VIDEO};
  auto expected=base;if(mode==CM_MANUAL)expected.push_back(SR_MANUAL);
  assert(settingsRowCount()==int(expected.size()));
  for(int i=0;i<settingsRowCount();++i){
   assert(settingsRowAt(i)==expected[i]);
   writes.clear();persistTopLevelSetting(settingsRowAt(i));
   if(expected[i]==SR_CAPTURE)assert(writes==std::vector<std::string>{"cap"});
   if(expected[i]==SR_VIDEO||expected[i]==SR_MANUAL)assert(writes.empty());
  }
 }
 s_settings.mode=CM_DEFAULT;saved.clear();writes.clear();reads.clear();

 for(int shoot=0;shoot<3;++shoot){
  s_shootMode=shoot;auto t=camPreviewTuning();
  assert(t.allowNightMode==(shoot==0));
  assert(t.exposure_ctrl&&t.gain_ctrl);
  assert(s_settings.tuning().allowNightMode); // Photo/settings profile unchanged.
 }

 const std::vector<std::string> expectedReads={"mode","af","cap","m_ae","m_gain","m_wb","m_sat","m_con","m_sharp"};
 loadCameraPrefs();assert(reads==expectedReads&&writes.empty());
 assert(s_cameraMode==CM_DEFAULT&&s_autofocusOn&&s_capturePreset==CR_VGA);
 assert(s_manualTuning.ae_level==0&&s_manualTuning.gainceiling_idx==1&&s_manualTuning.wb_mode==0);
 assert(s_manualTuning.saturation==0&&s_manualTuning.contrast==0&&s_manualTuning.sharpness==0);
 // Existing reload overwrites persisted fields only; it is not a whole-object reset.
 s_manualTuning.brightness=2;s_manualTuning.hmirror=false;s_manualTuning.vflip=true;
 s_manualTuning.whitebal=false;s_manualTuning.singleShotAf=true;
 for(unsigned byte=0;byte<256;++byte){
  for(const auto& key:expectedReads)saved[key]=byte;
  reads.clear();loadCameraPrefs();assert(reads==expectedReads&&writes.empty());
  assert(int(s_cameraMode)==std::min(byte,3u)&&int(s_capturePreset)==std::min(byte,2u));
  assert(s_autofocusOn==bool(byte));
  assert(s_manualTuning.ae_level==clamp(int(byte)-128,-2,2));
  assert(s_manualTuning.saturation==clamp(int(byte)-128,-2,2));
  assert(s_manualTuning.contrast==clamp(int(byte)-128,-2,2));
  assert(s_manualTuning.sharpness==clamp(int(byte)-128,-3,3));
  assert(s_manualTuning.gainceiling_idx==std::min(byte,3u)&&s_manualTuning.wb_mode==std::min(byte,4u));
  assert(s_manualTuning.brightness==2&&!s_manualTuning.hmirror&&s_manualTuning.vflip);
  assert(!s_manualTuning.whitebal&&s_manualTuning.singleShotAf);
 }
 saved.clear();loadCameraPrefs();
 s_manualTuning.ae_level=-2;s_manualTuning.gainceiling_idx=3;s_manualTuning.wb_mode=4;
 s_manualTuning.saturation=2;s_manualTuning.contrast=-1;s_manualTuning.sharpness=-3;
 // Fixed profiles ignore manual-only values; Manual copies them, then forces continuous AF.
 const int profiles[4][6]={{0,0,2,-1,-1,0},{-2,0,1,-2,0,0},{1,2,0,0,-1,0},{-2,3,4,2,-1,-3}};
 for(int mode=0;mode<4;++mode)for(bool af:{false,true}){
  s_cameraMode=CameraMode(mode);s_autofocusOn=af;auto t=effectiveTuning();
  assert(t.ae_level==profiles[mode][0]&&t.gainceiling_idx==profiles[mode][1]);
  assert(t.wb_mode==profiles[mode][2]&&t.saturation==profiles[mode][3]);
  assert(t.contrast==profiles[mode][4]&&t.sharpness==profiles[mode][5]);
  assert(t.autofocus==af&&!t.singleShotAf);
  assert(t.brightness==(mode==int(CM_MANUAL)?2:0));
  assert(t.hmirror==(mode!=int(CM_MANUAL))&&t.vflip==(mode==int(CM_MANUAL)));
  assert(t.whitebal==(mode!=int(CM_MANUAL)));
 }
 s_capturePreset=CapturePreset::FiveMp;s_cameraMode=CM_MANUAL;s_autofocusOn=true;
 const char* keys[]={"mode","af","cap","m_ae","m_gain","m_wb","m_sat","m_con","m_sharp"};
 const unsigned values[]={3,1,2,126,3,4,130,127,125};
 for(int field=0;field<9;++field)for(bool ok:{false,true}){
  saved={{"unrelated",42}};writes.clear();writable=ok;
  if(field<3)persistTopLevelSetting(field);else persistManualSetting(field-3);
  assert(writes.size()==1&&writes[0]==keys[field]);
  assert(saved.size()==(ok?2u:1u)&&saved["unrelated"]==42);
  if(ok)assert(saved[keys[field]]==values[field]);
  assert(s_manualTuning.sharpness==-3&&s_cameraMode==CM_MANUAL); // Failure does not revert the draft.
 }
 writes.clear();assert(!s_settings.save(services::camera::Setting(255)));assert(writes.empty());
 writable=false;assert(!s_settings.save(services::camera::Setting::Mode));
 writable=true;assert(s_settings.save(services::camera::Setting::Mode));
 writes.clear();persistTopLevelSetting(SR_MANUAL);persistTopLevelSetting(-1);
 persistManualSetting(MR_README);persistManualSetting(999);assert(writes.empty());
 puts("PASS Camera settings: defaults/read order, 256 byte values per field, preserved manual state, 8 profile/AF combinations, isolated successful/failed writes and non-setting rows");
}
'''
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root/'Arduino.h').write_text('#pragma once\n#include <stdint.h>\n#include <stddef.h>\n')
    (root/'core').mkdir()
    (root/'core/store.h').write_text('''#pragma once
#include <stdint.h>
namespace core {namespace store {
uint8_t getU8(const char*,const char*,uint8_t);
bool getBool(const char*,const char*,bool);
bool putU8(const char*,const char*,uint8_t);
bool putBool(const char*,const char*,bool);
}}
''')
    (root/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(root/'test.cpp'),str(ROOT/'src/services/camera_settings.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
