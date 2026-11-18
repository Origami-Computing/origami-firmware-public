"""Execute the production boot/escape adapter with a clock, pin and navigation fixture."""
from pathlib import Path
import os, shlex, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
compiler=shlex.split(os.environ.get('CXX','c++'))
sanitizers=[] if os.name=='nt' else ['-fsanitize=address,undefined']
source=(root/'src/apps/tutorial_app.cpp').read_text()
def function(signature):
    start=source.index(signature)
    end=source.index('{',start)+1
    depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start:end]
code=r'''
#include <stdint.h>
#include <initializer_list>
#include <cassert>
bool s_active=false,s_talkPending=false,s_agentStarted=false,s_bootChecked=false,s_completed=false,s_audioSelected=false;
uint32_t now=0;
unsigned pushed=0,popped=0,saved=0,stopped=0,suppressed=0,marks=0;
int app;
#include "apps/onboarding_flow.h"
using apps::onboarding::Flow;
Flow s_flow;
namespace ui {namespace kami_motion {bool playing=false;bool active(unsigned){return playing;}}}
namespace services {namespace automation {bool capture=false;bool wantsInputCapture(){return capture;}}}
uint32_t millis(){return now;}void save(){++saved;}void stopSpeech(){++stopped;}
struct {void println(const char*){}}Serial;
namespace hal {namespace input {bool a=false,b=false;bool buttonADown(){return a;}bool buttonBDown(){return b;}void suppressUntilButtonsReleased(){++suppressed;}}}
namespace services {void notifyUserActivity(){} namespace onboarding_progress {
struct Progress {bool offered=false,completed=false;unsigned page=0,interest=0;bool account=false,joinWifi=false,setupPublished=false,connected=false;};Progress progress;bool writeOk=true;
Progress load(){return progress;}
bool save(unsigned page,bool,unsigned,bool,bool,bool,bool){if(!writeOk)return false;progress.page=page;return true;}
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
void load(){s_completed=services::onboarding_progress::load().completed;}
'''+function('void serviceBootOffer(){')+'\n'+function('void replay(){')+r'''
int main(){
 auto& nav=NavHandler::instance();
 serviceBootOffer();assert(pushed==1&&marks==1&&s_active);
 nav.levels=1;s_active=false;s_bootChecked=false;serviceBootOffer();assert(pushed==2&&marks==1); // Reboot resumes unfinished tutorial.
 // Holding both buttons during either the tutorial or a child must never unwind the stack.
 hal::input::a=hal::input::b=true;
 for(int depth:{2,3}){
  nav.levels=depth;now=100;serviceBootOffer();now+=6000;serviceBootOffer();
  assert(nav.levels==depth && popped==0 && s_active);
 }
 replay();assert(!s_completed && !s_flow.canExitReplay(s_completed));
 services::onboarding_progress::progress.completed=true;
 s_active=false;s_bootChecked=false;nav.levels=1;
 const auto before=pushed;serviceBootOffer();assert(pushed==before && !s_active);
 replay();assert(s_completed && s_flow.page==0 && s_flow.canExitReplay(s_completed));

 s_active=false;s_bootChecked=false;nav.levels=1;
 services::onboarding_progress::progress.completed=false;
 services::onboarding_progress::progress.page=Flow::kPowerResume;
 const auto wakeBefore=pushed;
 ui::kami_motion::playing=true;serviceBootOffer();assert(pushed==wakeBefore&&!s_bootChecked);
 ui::kami_motion::playing=false;services::automation::capture=true;serviceBootOffer();assert(pushed==wakeBefore);
 services::automation::capture=false;services::onboarding_progress::writeOk=false;serviceBootOffer();assert(pushed==wakeBefore);
 services::onboarding_progress::writeOk=true;serviceBootOffer();assert(pushed==wakeBefore+1&&services::onboarding_progress::progress.page==140);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.cpp').write_text(code)
    subprocess.run(compiler+['-std=c++14',*sanitizers,'-I'+str(root/'include'),str(p/'test.cpp'),'-o',str(p/'test.exe')],check=True)
    subprocess.run([str(p/'test.exe')],check=True,timeout=10)
print('PASS first boot and unfinished reboot resume, protected long holds and replay eligibility from persisted completion')

# Execute the production policy, including explicit manual C-hold sleep admission.
policy_start=source.index('static core::health::RuntimePolicy runtimePolicy() {')
policy_end=source.index('\n}',policy_start)+2
policy=source[policy_start:policy_end]
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory)
    (p/'policy.cpp').write_text('#include "core/health_policy.h"\n#include <cassert>\nbool active; bool sessionActive(){return active;}\n'+policy+'\nint main(){active=true;auto p=runtimePolicy();assert(!p.allowPowerSleep&&!p.allowBatterySleep&&!p.allowIdleReturn);active=false;p=runtimePolicy();assert(p.allowPowerSleep&&p.allowBatterySleep);}')
    subprocess.run(compiler+['-std=c++14','-I'+str(root/'include'),str(p/'policy.cpp'),'-o',str(p/'policy.exe')],check=True)
    subprocess.run([str(p/'policy.exe')],check=True)
print('PASS active tutorial blocks manual and idle sleep; inactive tutorial releases policy')
