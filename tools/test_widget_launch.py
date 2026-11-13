"""Run the production widget event bridge and loading tick under failure/re-entry."""
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
code = r'''#include "widgets/launch.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
uint32_t clockMs=100,s_loadingStartMs=0,s_loadingErrMs=0,s_launchJob=0;
uint32_t millis(){return clockMs;}
char s_loadingTitle[80]={},s_loadingErr[80]={},s_remoteId[40]={},s_remoteSlug[40]={};
bool s_loadRequested=false,s_externalSelected=false;
const int kScreenPicker=1,kScreenLua=2;int screen=0;
namespace custom {enum class Result {Ok,Io};struct Identity{};const char* resultName(Result){return "io";}}
custom::Identity s_selected;
struct Package {void reset(){}} s_readyPackage;
namespace services {void notifyUserActivity(){}}
namespace hal {namespace display {void markAllDirty(){}}}
namespace storage {
enum class Operation {Load,Sync};bool busy=false,complete=false;custom::Result outcome=custom::Result::Ok;int requests=0;
uint32_t request(Operation,custom::Identity,const void* =nullptr,unsigned=0,const char* =nullptr,const char* =nullptr){++requests;return busy?0:42;}
bool take(uint32_t id,custom::Result& result,Package*,void*,custom::Identity*){assert(id==42);if(!complete)return false;complete=false;result=outcome;return true;}
}
void showWidgetScreen(const int* next){screen=*next;}
'''
code += fn('loadingEnter') + '\n' + fn('loadingTick')
code += r'''
int main(){
 using namespace widgets::launch;
 notifyLoading("old");takeLoadingRequest();notifyReady("old-id","old");
 notifyLoading("new");takeLoadingRequest();loadingEnter();loadingTick(101);
 assert(storage::requests==0&&screen==0);assert(!strcmp(s_loadingTitle,"new"));
 notifyReady("id","slug");storage::busy=true;loadingTick(102);assert(!s_launchJob&&s_remoteId[0]);
 storage::busy=false;loadingTick(103);assert(s_launchJob==42&&!s_remoteId[0]);
 loadingTick(104);assert(screen==0);storage::complete=true;loadingTick(105);assert(screen==kScreenLua&&s_externalSelected&&!s_launchJob);
 screen=0;s_loadRequested=true;storage::busy=true;loadingTick(106);assert(s_loadRequested&&!s_launchJob);
 storage::busy=false;loadingTick(107);assert(!s_loadRequested&&s_launchJob==42);
 storage::outcome=custom::Result::Io;storage::complete=true;loadingTick(108);assert(s_loadingErr[0]&&screen==0);
 loadingTick(2307);assert(screen==0);loadingTick(2308);assert(screen==kScreenPicker);
 screen=0;clockMs=0xfffffff0u;loadingEnter();notifyError("server failed");loadingTick(clockMs);assert(s_loadingErr[0]);
 loadingTick(uint32_t(clockMs+2200u));assert(screen==kScreenPicker);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),str(ROOT/'src/widgets/launch.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS widget launch: stale events, worker admission retry, completion, I/O failure and error clock wrap')
