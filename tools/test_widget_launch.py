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
code = r'''
#include "widgets/launch.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
uint32_t clockMs=0,s_loadingStartMs=0,s_loadingErrMs=0;
uint32_t millis(){return clockMs;}
char s_loadingErr[80]={},s_chosenSlug[40]={};
constexpr size_t kSlugLen=40;
const int kScreenPicker=1,kScreenLua=2;
int screen=0,fetches=0;bool failFetch=false;
namespace services {void notifyUserActivity(){}}
namespace hal {namespace display {void markAllDirty(){}}}
namespace widgets {namespace sync {int syncOne(const char*,const char*,char* err,size_t n){++fetches;if(failFetch){snprintf(err,n,"fetch failed");return -1;}return 0;}}}
void showWidgetScreen(const int* next){screen=*next;}
'''
code += fn('loadingEnter') + '\n' + fn('loadingTick')
code += r'''
void reset(uint32_t now=100){s_loadingStartMs=now;s_loadingErr[0]=0;s_loadingErrMs=0;screen=0;}
int main(){
 using namespace widgets::launch;
 // A cancelled previous build must not launch its stale ready in the next build.
 notifyLoading("old");takeLoadingRequest();notifyReady("old-id","old");
 notifyLoading("new");takeLoadingRequest();reset();loadingTick(101);
 assert(fetches==0 && screen==0);
 loadingTick(99);assert(!s_loadingErr[0]&&screen==0);
 // Old failure must not poison the next operation either.
 notifyError("old failure");notifyLoading("next");takeLoadingRequest();reset();loadingTick(102);
 assert(!s_loadingErr[0]);
 notifyReady("new-id","new");loadingTick(103);assert(screen==kScreenLua&&fetches==1);
 notifyLoading("failed download");takeLoadingRequest();reset();failFetch=true;
 clockMs=4104;notifyReady("id","slug");loadingTick(104);assert(s_loadingErr[0]&&screen==0);
 loadingTick(4105);assert(screen==0);loadingTick(6304);assert(screen==kScreenPicker);failFetch=false;
 // A lost server completion has a bounded, visible outcome, including millis wrap.
 notifyLoading("lost");takeLoadingRequest();reset(0xfffffff0u);
 loadingEnter();assert(s_loadingStartMs==0xfffffff0u);
 loadingTick(uint32_t(s_loadingStartMs+180000u));assert(s_loadingErr[0]&&screen==0);
 loadingEnter();assert(s_loadingErr[0]);
 loadingTick(uint32_t(s_loadingStartMs+182200u));assert(screen==kScreenPicker);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),str(ROOT/'src/widgets/launch.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS widget launch: stale events, ready, download failure, lost completion, clock wrap')
