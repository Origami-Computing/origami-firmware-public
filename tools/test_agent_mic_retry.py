"""Exercise the production mic retry under transient and persistent pressure."""
from pathlib import Path
import subprocess, sys
root = Path(__file__).resolve().parents[1]
source = (root/'src/services/agent_voice.cpp').read_text()
start = source.index('    bool started=mic.startStream')
end = source.index('    if (!started) {', start)
body = source[start:end]
out = root/'.cache/agent-mic-retry'
out.mkdir(parents=True, exist_ok=True)
cpp = out/'retry.cpp'
exe = out/'retry.exe'
cpp.write_text('''#include <cstdint>
#include <cassert>
#include <cstddef>
uint32_t now, readyAt; int attempts, failures;
uint32_t millis(){return now;}
void vTaskDelay(uint32_t n){now+=n;}
#define pdMS_TO_TICKS(x) (x)
namespace background_network {
bool kamiVoiceNetworkReady(){return now>=readyAt;}
}
struct Mic { bool startStream(int,void*){return ++attempts>failures;} };
Mic mic;
struct Logger { template<class... T> void printf(const char*,T...){} } Serial;
int sink=0;void* user=nullptr;
bool run(){
'''+body+'''return started;}
void reset(int fail,uint32_t ready){now=0;attempts=0;failures=fail;readyAt=ready;}
int main(){
 reset(0,10000); assert(run()); assert(attempts==1 && now==0);
 reset(1,400); assert(run()); assert(attempts==2 && now==401);
 reset(2,400); assert(!run()); assert(attempts==2 && now==401);
 reset(1,10000); assert(!run()); assert(attempts==1 && now==1500);
 reset(1,0); assert(run()); assert(attempts==2 && now==1);
}
''')
subprocess.run(([sys.argv[1], 'c++'] if len(sys.argv)>1 else ['c++'])+['-std=c++11', str(cpp), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
print('PASS fast mic startup, transient allocation recovery, retry limit and bounded shutdown wait')
