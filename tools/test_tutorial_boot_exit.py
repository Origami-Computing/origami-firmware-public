"""Execute the production boot/escape adapter with a clock, pin and navigation fixture."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/apps/tutorial_app.cpp').read_text()
start=source.index('void serviceBootOffer(){');end=source.index('{',start)+1;depth=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
code=r'''
#include <stdint.h>
#include <initializer_list>
#include <cassert>
bool s_active=false,s_talkPending=false,s_agentStarted=false,s_bootChecked=false,s_exitHolding=false;
uint32_t s_exitHoldAt=0,now=0;
unsigned pushed=0,popped=0,saved=0,stopped=0,suppressed=0,marks=0;
int app;
struct {bool wifiOnly=false;void agentReturned(){}}s_flow;
uint32_t millis(){return now;}void save(){++saved;}void stopSpeech(){++stopped;}
struct {void println(const char*){}}Serial;
namespace hal {namespace input {bool a=false,b=false;bool buttonADown(){return a;}bool buttonBDown(){return b;}void suppressUntilButtonsReleased(){++suppressed;}}}
namespace services {void notifyUserActivity(){} namespace onboarding_progress {
struct Progress {bool offered=false,completed=false;};Progress progress;bool writeOk=true;
Progress load(){return progress;}
bool markOffered(){++marks;if(writeOk)progress.offered=true;return writeOk;}
}}
namespace apps {namespace agent {bool home=true;bool isAtHomescreen(){return home;}}}
struct NavHandler {
 int levels=1;bool overlay=false;
 static NavHandler& instance(){static NavHandler nav;return nav;}
 int depth(){return levels;}bool systemScreenActive(){return overlay;}
 void setWorkflowPolicy(std::initializer_list<bool>){}
 void push(int*){++pushed;++levels;}
 void popToRoot(){++popped;levels=1;s_active=false;}
};
void load(){}
'''+source[start:end]+r'''
int main(){
 auto& nav=NavHandler::instance();
 serviceBootOffer();assert(pushed==1&&marks==1&&s_active);
 nav.levels=1;s_active=false;s_bootChecked=false;serviceBootOffer();assert(pushed==2&&marks==1); // Reboot resumes unfinished tutorial.
 services::onboarding_progress::progress.offered=false;
 services::onboarding_progress::progress.completed=true;
 s_bootChecked=false;serviceBootOffer();assert(pushed==2); // Completed legacy.
 s_active=true;nav.levels=3;hal::input::a=hal::input::b=true;
 now=100;serviceBootOffer();now=5099;serviceBootOffer();assert(popped==0);
 hal::input::b=false;serviceBootOffer();hal::input::b=true;now=5100;serviceBootOffer();
 now=10099;serviceBootOffer();assert(popped==0); // Releasing either button restarts the full interval.
 now=10100;serviceBootOffer();assert(popped==1&&!s_active&&saved==1&&suppressed==1);
 now=16000;serviceBootOffer();assert(popped==1); // Still-held gesture cannot repeat.
 s_active=true;s_flow.wifiOnly=true;serviceBootOffer();now+=6000;serviceBootOffer();assert(popped==1);
 s_flow.wifiOnly=false;now=0xfffffff0u;serviceBootOffer();now+=5000;serviceBootOffer();assert(popped==2); // Timer wrap.
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++14','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=10)
print('PASS first boot and unfinished reboot resume, completed migration, nested tutorial escape, 4999/5000 ms, interrupted hold and timer wrap')

# Execute the production policy, including explicit manual C-hold sleep admission.
policy_start=source.index('static core::health::RuntimePolicy runtimePolicy() {')
policy_end=source.index('\n}',policy_start)+2
policy=source[policy_start:policy_end]
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory)
    (p/'policy.cpp').write_text('#include "core/health_policy.h"\n#include <cassert>\nbool active; bool sessionActive(){return active;}\n'+policy+'\nint main(){active=true;auto p=runtimePolicy();assert(!p.allowPowerSleep&&!p.allowBatterySleep&&!p.allowIdleReturn);active=false;p=runtimePolicy();assert(p.allowPowerSleep&&p.allowBatterySleep);}')
    subprocess.run(['c++','-std=c++14','-I'+str(root/'include'),str(p/'policy.cpp'),'-o',str(p/'policy')],check=True)
    subprocess.run([str(p/'policy')],check=True)
print('PASS active tutorial blocks manual and idle sleep; inactive tutorial releases policy')
