"""Execute the real OTA app transitions: metadata never authorizes a download."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/apps/ota_update_app.cpp').read_text()
def function(marker):
 a=source.index(marker);b=source.index('{',a)+1;depth=1
 while depth:
  depth+=(source[b]=='{')-(source[b]=='}');b+=1
 return source[a:b]
code=r'''
#include <cassert>
#include <cstdint>
enum State{ST_CONFIRM,ST_CHECKING,ST_FOUND,ST_UP_TO_DATE,ST_NO_WIFI,ST_DOWNLOADING,ST_FLASHING,ST_SUCCESS,ST_ERROR};
State s_state=ST_CONFIRM;
bool s_appActive=false,s_flashBegun=false,s_needRedraw=false;
int s_written=0,s_total=0,s_checkRetries=0;
uint32_t s_retryAfterMs=0,s_progressRedrawMs=0,s_successMs=0;
const char* s_resourceError=nullptr;
bool wifi=true,cached=false,busy=false,newer=true,fetchOk=true,running=true;
int fetches=0,downloads=0,activations=0,cancels=0,pops=0;
uint32_t clockMs=100;uint32_t millis(){return clockMs;}
void delay(int){}struct Esp{void restart(){assert(false);}}ESP;
struct Release{char tag[32];}s_release;
struct Session{void cancel(){++cancels;}}s_session;
bool reserveNetwork(){return true;}
void setState(State state){s_state=state;}
namespace services {
namespace firmware_update{bool noticeBusy(){return busy;}bool settingsUpdateAvailable(){return cached;}
void copySettingsRelease(Release*){}}
namespace ota_client{
enum class ReleaseChannel{Production,Staging};
ReleaseChannel releaseChannel(){return ReleaseChannel::Production;}
void abortFlash(){++cancels;}
bool fetchLatestRelease(ReleaseChannel,Release*){++fetches;return fetchOk;}
bool isReleaseNewer(ReleaseChannel,const char*){return newer;}
bool beginFlash(const Release&){++downloads;return true;}
bool pumpFlash(int*,int*){return running;}
const char* lastError(){return "";}
bool finishFlash(){++activations;return true;}
}}
services::ota_client::ReleaseChannel s_channel=services::ota_client::ReleaseChannel::Production;
namespace hal {
namespace display{void markAllDirty(){}}
namespace wifi{bool connected(){return ::wifi;}}
namespace input{enum class EventType{ButtonA_Tap,ButtonC_Tap,Other};struct InputEvent{EventType type;};}}
namespace ui{namespace nav{
bool isEnterTap(hal::input::InputEvent e){return e.type==hal::input::EventType::ButtonC_Tap;}
bool isBackTap(hal::input::InputEvent e){return e.type==hal::input::EventType::ButtonA_Tap;}
}}
struct NavHandler{static NavHandler& instance(){static NavHandler n;return n;}void pop(){++pops;}};
'''+function('void enter()')+function('void tick()')+function('void onInput(')+r'''
int main(){
 using E=hal::input::EventType;
 enter();assert(s_state==ST_CONFIRM);for(int i=0;i<100;++i)tick();assert(!fetches&&!downloads);
 onInput({E::ButtonC_Tap});tick();assert(fetches==1&&s_state==ST_FOUND&&!downloads);
 for(int i=0;i<100;++i)tick();onInput({E::Other});assert(!downloads&&!activations);
 onInput({E::ButtonA_Tap});assert(pops==1&&!downloads);
 cached=true;enter();assert(s_state==ST_FOUND);for(int i=0;i<100;++i)tick();assert(!downloads);
 onInput({E::ButtonC_Tap});assert(s_state==ST_DOWNLOADING&&!downloads);
 tick();tick();assert(downloads==1&&!activations);
 running=false;tick();assert(s_state==ST_FLASHING&&!activations);
 tick();assert(activations==1&&s_state==ST_SUCCESS);
 cached=false;newer=false;enter();onInput({E::ButtonC_Tap});tick();assert(s_state==ST_UP_TO_DATE);
 onInput({E::ButtonC_Tap});tick();assert(downloads==1&&activations==1);
 busy=true;enter();tick();assert(s_state==ST_CHECKING&&fetches==2);
 busy=false;newer=true;tick();assert(s_state==ST_FOUND&&downloads==1);
 wifi=false;enter();onInput({E::ButtonC_Tap});tick();assert(s_state==ST_NO_WIFI&&downloads==1);
}
'''
with tempfile.TemporaryDirectory() as folder:
 path=Path(folder);(path/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
 subprocess.run([str(path/'test')],check=True)
print('PASS actual OTA app: explicit check and separate download confirmation; cached/idle/back/current/offline cases never download')
