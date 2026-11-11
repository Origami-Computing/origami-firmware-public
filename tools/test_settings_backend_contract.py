"""Characterize the actual Settings confirmation and serial backend failure gates.

Storage/refresh are fakes: this establishes caller/service policy, not NVS atomicity,
credential thread safety, or a successful network reconnect. No device changes.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, marker):
    source = (ROOT / path).read_text()
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


confirmation = function('src/apps/settings_menu_app.cpp', 'static void finishValueEdit()')
serial_source = (ROOT / 'src/tooling/dev_serial_input.cpp').read_text()
serial_start = serial_source.index('    const bool ok = services::agent_transport::setBackendUrl(url);')
serial_end = serial_source.index('    Serial.printf(', serial_start)
serial_gate = serial_source[serial_start:serial_end]
operations = '\n'.join(function('src/services/agent_transport.cpp', marker) for marker in
                       ('bool selectServerMode(core::secrets::ServerMode mode)', 'bool setBackendUrl(const char* url)'))
code = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
bool s_valueEditing=true,s_needRedraw=false;int s_valueDraft=0;
char s_otaChannelLabel[64],s_recipeLabel[64],s_serverLabel[64];
enum {SUB_MAIN, SUB_SLEEP_MENU, ROW_SERVER, ROW_OTA_CHANNEL, ROW_RECIPE_ALERTS};
int s_sub=SUB_MAIN,s_sel=ROW_SERVER;
int currentMode=0;bool saveOk=true;
int modeWrites=0,urlWrites=0,restarts=0,labels=0;
namespace core {
namespace sleep_timer {bool setEnabled(bool){assert(false);return false;}}
namespace secrets {
enum class ServerMode{Prod,Staging,Dev};
ServerMode serverMode(){return static_cast<ServerMode>(currentMode);}
bool setServerMode(ServerMode value){++modeWrites;if(saveOk)currentMode=int(value);return saveOk;}
bool setBackendUrl(const char*){++urlWrites;return saveOk;}
}}
namespace services {
namespace automation {bool setRecipeAlertsEnabled(bool){assert(false);return false;}}
namespace agent_transport {void reconfigure(){++restarts;}
''' + operations + r'''
}
namespace ota_client {
enum class ReleaseChannel {Staging,Production};
bool setReleaseChannel(ReleaseChannel){return saveOk;}
const char* lastError(){return "Set up OTA access key";}
void setDeveloperHold(bool){assert(false);}
}}
void updateSleepLabel(){assert(false);}
void updateRecipeLabel(){assert(false);}
void updateServerLabel(){++labels;}
void updateOtaChannelLabel(){strcpy(s_otaChannelLabel,"Updates: Production");}
''' + confirmation + '\nbool serialSet(const char* url){\n' + serial_gate + r'''return ok;}
int main(){
  for(int initial:{0,1,2})for(int draft:{0,1,2})for(bool succeeds:{false,true}){
    currentMode=initial;s_valueDraft=draft;saveOk=succeeds;
    modeWrites=restarts=labels=0;s_valueEditing=true;s_needRedraw=false;
    finishValueEdit();
    assert(modeWrites==int(initial!=draft));
    assert(restarts==int(initial!=draft&&succeeds)); // Rejected changes must not disconnect the live backend.
    assert(currentMode==((initial!=draft&&succeeds)?draft:initial));
    assert(labels==1&&!s_valueEditing&&s_needRedraw);
    finishValueEdit();assert(labels==1); // Repeated confirmation after editor closes is inert.
  }
  for(bool succeeds:{false,true}){
    saveOk=succeeds;urlWrites=restarts=0;assert(serialSet("http://synthetic.invalid")==succeeds);
    assert(urlWrites==1&&restarts==int(succeeds));
  }
  s_sel=ROW_OTA_CHANNEL;saveOk=false;s_valueEditing=true;
  finishValueEdit();
  assert(!strcmp(s_otaChannelLabel,"Updates: Set up OTA access key"));
  saveOk=true;s_valueEditing=true;finishValueEdit();
  assert(!strcmp(s_otaChannelLabel,"Updates: Production"));
  puts("PASS Settings changed/unchanged mode, success/failure, editor close/repeat, and serial success-only restart");
}
'''
code = '#include <initializer_list>\n' + code
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++11', '-fsanitize=address,undefined',
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
