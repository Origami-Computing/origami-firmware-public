"""Exercise the production streaming playback pump, including stale UI timestamps."""
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[1]
s=(root/'src/services/agent_speech.cpp').read_text();a=s.index('void tick(');b=s.index('{',a)+1;depth=1
while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
code=r'''
#include <cstdint>
#include <algorithm>
#include <cassert>
struct Player {
 bool next=false;int starts=0;bool openSession(){return true;}bool startPcmStream(int){++starts;return true;}
 uint32_t appendPcmStream(const int16_t*,uint32_t n){return n;}
 bool finishPcmStream(){next=true;return true;}bool consumeWantsNextTrack(){bool n=next;next=false;return n;}
} player;
constexpr uint32_t capacity=48000;
int16_t samples[capacity],*pending=samples;
uint32_t readAt=0,queued=0,lastData=1001,drainAt=0;
bool active=true,started=false,finished=false,ending=false;
namespace agent_voice {bool capturing=false;bool isCapturing(){return capturing;}}
namespace agent_latency {void milestone(const char*){}}
struct Logger {void println(const char*){}} Serial;
void stop(){active=started=finished=ending=false;queued=0;drainAt=0;}
''' + s[a:b] + r'''
int main(){
 // pollReplies can update lastData after a screen's now timestamp was sampled.
 queued=2400;tick(1000);assert(active&&started&&player.starts==1&&queued==0);
 finished=true;tick(1002);assert(active&&ending);tick(1122);assert(!active);
 active=true;lastData=0xfffffff0u;tick(10);assert(active); // millis wrap
 tick(15000);assert(!active);
 active=true;lastData=100;agent_voice::capturing=true;tick(100);assert(!active);
 agent_voice::capturing=false;active=true;finished=true;tick(100);assert(!active);
 active=true;queued=2399;tick(100);assert(active&&!started); // bounded startup buffer
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS production speech pump: stale tick, clock wrap, prebuffer, finish drain, capture cancellation and empty reply')
