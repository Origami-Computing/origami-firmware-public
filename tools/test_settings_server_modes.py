"""Execute actual Settings server-wheel and About row composition for both roles."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/apps/settings_menu_app.cpp').read_text()
a=s.index('  if(s_valueEditing) {',s.index('void onInput('));b=s.index('\n  if (ev.type',a+10)
wheel=s[a:b]
a=s.index('      const auto account=',s.index('case SUB_ABOUT: {'));b=s.index('      s_aboutList.buildScene',a)
about=s[a:b]
code=r'''
#include "core/release_policy.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <initializer_list>
#define OTA_FIRMWARE_VERSION "test-version"
#define OTA_HARDWARE_ID "n16r8"
namespace services {namespace account_info {struct Snapshot{const char* name;};Snapshot snapshot(){return {"Test"};}}
namespace ota_client {const char* buildKindName(){return "test";}}}
namespace core {namespace secrets {struct Config {const char* apiKey;};Config backendConfig(){return {"fake"};}}}
struct List{int totalItems;}s_aboutList;
void applySettingsSubListLayout(List&){}
std::vector<std::string> rowsForAbout(){
'''+about+r'''
 std::vector<std::string> result;for(int i=0;i<s_aboutList.totalItems;++i)result.push_back(rows[i]);return result;
}
enum class ET{Scroll,Enter,Back};struct Event{ET type;int delta;};
namespace ui {namespace nav {bool isEnterTap(Event e){return e.type==ET::Enter;}bool isBackTap(Event e){return e.type==ET::Back;}}}
namespace sfx {int kSfxAlarmNavigate;void play(int){}}
enum {SUB_MAIN,SUB_SLEEP_MENU,ROW_SERVER,ROW_OTA_CHANNEL};
int s_sub=SUB_MAIN,s_sel=ROW_SERVER,s_valueDraft=1,commits=0;
bool s_valueEditing=true,s_needRedraw=false;
void finishValueEdit(){++commits;s_valueEditing=false;}
void input(Event ev){
'''+wheel+r'''
}
int main(){
 auto rows=rowsForAbout();assert(rows.size()==(core::release_policy::stagingBuild?7:6));
 bool found=false;for(auto& row:rows)if(row=="Firmware: Staging")found=true;
 assert(found==core::release_policy::stagingBuild);
 if(core::release_policy::stagingBuild){
  for(int start:{0,1,2})for(int delta:{-7,-3,-1,1,3,7}){
   s_valueDraft=start;input({ET::Scroll,delta});assert(s_valueDraft==(start+delta%3+3)%3);
  }
  s_valueDraft=1;input({ET::Scroll,1});assert(s_valueDraft==2);
  input({ET::Scroll,1});assert(s_valueDraft==0);
  input({ET::Scroll,1});assert(s_valueDraft==1);
 }
 // A separate production-entry regression ensures this editor cannot be opened.
 for(ET type:{ET::Enter,ET::Back}){s_valueEditing=true;int before=commits;input({type,0});assert(commits==before+1&&!s_valueEditing);}
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 for staging in (0,1):
  subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',f'-DORIGAMI_STAGING_BUILD={staging}',f'-DORIGAMI_PRODUCTION_BUILD={1-staging}','-I'+str(root/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
print('PASS actual server-wheel wrapping, confirmation, and About row presence/absence for staging/production')
