"""Exercise actual core-I2C restart/reconnect while a peer owns the bus."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/hal/hal_io.cpp').read_text(encoding='utf8')
def block(marker):
    a=source.index(marker);b=source.index('{',a)+1;depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]
code=r'''
#include <cassert>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
std::mutex bus;thread_local bool locked=false;
std::atomic<bool> transaction{false},peerStarted{false};int ends=0,begins=0,reconnects=0;bool succeed=true;
namespace hal {namespace bus {
void lockI2c(){::bus.lock();locked=true;}
void unlockI2c(){assert(locked);locked=false;::bus.unlock();}
}}
constexpr int SDA_PIN=1,SCL_PIN=2,kTcaAddr=0x22;
struct WireType {
 void end(){assert(locked&&!transaction);++ends;}
 void begin(int,int){assert(locked&&!transaction);++begins;}
} Wire;
struct Tca {bool reconnect(WireType&,int){assert(locked&&!transaction);++reconnects;return succeed;}}s_tca;
struct {template<class... A>void printf(const char*,A...) {}} Serial;
void delay(unsigned){assert(locked);}
'''+block('static bool reconnectBus(')+'\n'+block('bool reinitAfterI2CRestart(')+'\n'+block('bool restartBusAndReconnect(')+r'''
int main(){
 assert(reinitAfterI2CRestart()&&ends==0&&begins==0&&reconnects==1&&!locked);
 std::thread peer([]{hal::bus::lockI2c();transaction=true;peerStarted=true;
   std::this_thread::sleep_for(std::chrono::milliseconds(30));transaction=false;hal::bus::unlockI2c();});
 while(!peerStarted)std::this_thread::yield();
 assert(restartBusAndReconnect());peer.join();assert(ends==1&&begins==1&&reconnects==2&&!locked);
 succeed=false;assert(!restartBusAndReconnect()&&!locked&&ends==2&&begins==2&&reconnects==7);
 assert(bus.try_lock());bus.unlock();
}
'''
with tempfile.TemporaryDirectory(prefix='camera-i2c-') as td:
    p=Path(td);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-pthread','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS actual I2C recovery: waits for active peer, holds lock through restart/reconnect, unlocks on failure')
