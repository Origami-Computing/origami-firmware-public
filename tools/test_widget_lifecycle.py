"""Exercise actual Lua app transitions with a faultable VM and screen lifecycle."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
s = (ROOT / 'src/widgets/app.cpp').read_text()
def fn(name):
    start = s.index('void ' + name + '(')
    end = s.index('{', start) + 1
    depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}')
        end += 1
    return s[start:end]
code = r'''
#include "app.h"
#include <cassert>
#include <cstddef>
namespace scene { enum class ScreenTransition{Automatic,Forward,Back,Silent}; struct Screen {void(*onEnter)();};
void switchTo(App* a,const Screen* s,ScreenTransition=ScreenTransition::Automatic){a->currentScreen=s;if(s->onEnter)s->onEnter();}}
struct NavHandler {static NavHandler& instance(){static NavHandler n;return n;}void runtimePolicyChanged(){}};
int opens,loads,starts,stops,closes,dirty;
bool failOpen,failLoad,s_running;
struct Damage {void release(){}} s_frameDamage;
constexpr size_t kMemCeilingBytes=128*1024;
struct Vm {
 bool opened=false,fault=false;
 bool isOpen(){return opened;} bool faulted(){return fault;}
 bool open(size_t cap){assert(cap==kMemCeilingBytes);++opens;fault=failOpen;return opened=!failOpen;}
 void close(){++closes;opened=false;fault=false;}
 void callStart(){if(!fault)++starts;}
 void callStop(){++stops;} const char* error(){return "failure";}
} s_vm;
struct Log {void printf(const char*,const char*){}} Serial;
namespace hal {namespace display {void markAllDirty(){++dirty;}}}
void loadWidgetScript(){++loads;s_vm.fault=failLoad;}
struct Reservation {bool held=false;void release(){held=false;}} s_loadingNetwork;
extern App app;extern const scene::Screen kScreenLua,kScreenPicker,kScreenLoading;
'''
for name in ['startWidget','stopWidget','exitWorkflow','showWidgetScreen','resumeWorkflow','runnerEnter']:
    code += fn(name) + '\n'
code += r'''
const scene::Screen kScreenLua={runnerEnter},kScreenPicker={nullptr},kScreenLoading={nullptr};
const scene::Screen* const kScreens[]={&kScreenPicker,&kScreenLua,&kScreenLoading};
'''
a=s.index('App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS(')
start=s.index('static core::health::RuntimePolicy runtimePolicy()'); code += s[start:s.index('\nApp app =',start)]+'\n'
code+=s[a:s.index(';',a)+1]+'\n'
code+=r'''
int main(){
// Direct presentation entry never allocates/reloads a VM.
runnerEnter();runnerEnter();assert(opens==0&&loads==0&&dirty==2);
showWidgetScreen(&kScreenLua);assert(s_running&&s_vm.opened&&opens==1&&starts==1);
runnerEnter();assert(opens==1&&starts==1);
// Overlay resume preserves the established reload in the same VM.
app.onResume();runnerEnter();assert(opens==1&&loads==2&&starts==2&&stops==0);
s_loadingNetwork.held=true;showWidgetScreen(&kScreenLoading);assert(s_loadingNetwork.held);assert(!app.runtimePolicy().allowBackgroundUploads && !app.runtimePolicy().allowAutomaticUpdate);assert(!s_running&&!s_vm.opened&&stops==1);
showWidgetScreen(&kScreenLua);assert(!s_loadingNetwork.held);assert(opens==2&&starts==3);
showWidgetScreen(&kScreenPicker);assert(app.runtimePolicy().allowBackgroundUploads && app.runtimePolicy().allowAutomaticUpdate);assert(!s_running&&!s_vm.opened&&stops==2);
// Uncover resets navigation to picker: no orphan VM remains.
showWidgetScreen(&kScreenLua);app.currentScreen=&kScreenPicker;app.onResume();
assert(!s_running&&!s_vm.opened&&stops==3);
showWidgetScreen(&kScreenLua);app.onExit();int stopped=stops;app.onExit();assert(stops==stopped&&!s_running);
// Loading ownership survives a system overlay, but not uncover or terminal pop.
showWidgetScreen(&kScreenLoading);s_loadingNetwork.held=true;app.onResume();assert(s_loadingNetwork.held);
app.currentScreen=&kScreenPicker;app.onResume();assert(!s_loadingNetwork.held);
showWidgetScreen(&kScreenLoading);s_loadingNetwork.held=true;app.onExit();assert(!s_loadingNetwork.held);
// Open and script failures remain visible until explicit back/retry.
failOpen=true;showWidgetScreen(&kScreenLua);assert(s_running&&!s_vm.opened&&s_vm.fault);
int attempts=opens;runnerEnter();assert(opens==attempts);
showWidgetScreen(&kScreenPicker);failOpen=false;failLoad=true;
int started=starts;showWidgetScreen(&kScreenLua);assert(s_vm.fault&&starts==started);
showWidgetScreen(&kScreenPicker);failLoad=false;showWidgetScreen(&kScreenLua);
assert(s_running&&s_vm.opened&&!s_vm.fault&&starts==started+1);app.onExit();
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS Lua app transitions: view-only entry, overlay reload, back/loading/uncover/pop cleanup, failed startup and explicit retry')
