"""Exercise actual radio suspension with offline BLE retaining camera DMA RAM."""
from pathlib import Path
import os, shlex, subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/hal/hal_wifi.cpp').read_text()

def function(signature):
    start = source.index(signature + ' {')
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

code = r'''
#include <cassert>
#include <cstdio>
bool s_wasConnected, s_restorePending, s_wifiSuspended, online, ble;
int releases, wifiStops;
enum class RadioState { Suspended, OfflineBle };
RadioState state;
void setRadioState(RadioState s,const char*) {state=s;}
bool connected(){return online;}
namespace hal { namespace ble {
bool started(){return ::ble;}
void deinit(){assert(s_wifiSuspended && state==RadioState::Suspended);::ble=false;++releases;}
}}
constexpr int WIFI_OFF=0,ESP_OK=0,ESP_ERR_WIFI_NOT_INIT=1;
using esp_err_t=int;
int deinitResult;
int esp_wifi_deinit(){return deinitResult;}
struct {
 void softAPdisconnect(bool){assert(!ble);}
 void disconnect(bool){assert(!ble);}
 void mode(int m){assert(m==WIFI_OFF&&!ble);++wifiStops;}
} WiFi;
struct {void println(const char*){} template<class... T>void printf(const char*,T...) {}} Serial;
void yield(){}
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(unsigned ms){assert(ms==80);}
''' + function('void stop()') + '\n' + function('void powerDown()') + r'''
int main(){
 for(bool deep:{false,true})for(bool initialBle:{false,true})for(bool connected:{false,true})
 for(int result:{ESP_OK,ESP_ERR_WIFI_NOT_INIT,99}){
  ble=initialBle;online=connected;releases=wifiStops=0;deinitResult=result;
  s_restorePending=true;s_wifiSuspended=false;state=RadioState::OfflineBle;
  if(deep)powerDown();else stop();
  assert(!ble && s_wifiSuspended && !s_restorePending && s_wasConnected==online);
  assert(releases==int(initialBle)&&wifiStops==1);
  // Repeated suspension must not attempt to tear down an absent BLE stack.
  if(deep)powerDown();else stop();
  assert(releases==int(initialBle)&&wifiStops==2);
 }
 puts("PASS normal/deep camera radio suspension: BLE released before Wi-Fi teardown, state preserved, repeated teardown safe");
}
'''
code = '#include <initializer_list>\n' + code
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.cpp').write_text(code)
    compiler=shlex.split(os.environ.get('CXX','c++'))
    flags=[] if os.name=='nt' else ['-fsanitize=address,undefined']
    exe=p/('test.exe' if os.name=='nt' else 'test')
    subprocess.run(compiler+['-std=c++11',*flags,str(p/'test.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
