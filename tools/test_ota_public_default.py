"""Exercise production Public-default migration, persistence faults and explicit opt-in."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/services/ota_client.cpp').read_text()
a=s.index('static bool s_publicDefaultAttempted');b=s.index('const char* releaseChannelName',a)
code=r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include <initializer_list>
enum class ReleaseChannel { Public, Staging, Private };
const char* kSettingsNamespace="settings";const char* kChannelKey="ota_chan";
int saved=2,marker=0,writes=0,failAt=0;bool auth=true,network=true;
namespace core {namespace store {
uint8_t getU8(const char*,const char*k,int){return !strcmp(k,"ota_chan")?saved:marker;}
bool putU8(const char*,const char*k,int v){if(++writes==failAt)return false;if(!strcmp(k,"ota_chan"))saved=v;else marker=v;return true;}
}}
namespace ota_access{enum class Role{Unknown,Production,Developer};Role grant=Role::Developer;Role role(){return grant;}}
namespace firmware_update{struct Session{enum class Result{Ready,Busy};Result reserveNetwork(){return network?Result::Ready:Result::Busy;}};}
bool hasAuthToken(){return auth;}bool refreshAccess(){return auth;}void setError(const char*){}char s_err[100];
'''+s[a:b]+r'''
void boot(){s_publicDefaultAttempted=s_publicDefaultReady=false;}
int main(){
 for(int old:{0,1,2}){saved=old;marker=writes=failAt=0;boot();assert(releaseChannel()==ReleaseChannel::Public&&saved==0&&marker==1&&writes==2);
  for(int i=0;i<100;i++)assert(releaseChannel()==ReleaseChannel::Public);assert(writes==2);
  assert(setReleaseChannel(ReleaseChannel::Private));boot();assert(releaseChannel()==ReleaseChannel::Private);
  assert(setReleaseChannel(ReleaseChannel::Staging));boot();assert(releaseChannel()==ReleaseChannel::Staging);
  ota_access::grant=ota_access::Role::Production;assert(!setReleaseChannel(ReleaseChannel::Private));assert(!setReleaseChannel(ReleaseChannel::Staging));assert(setReleaseChannel(ReleaseChannel::Public));ota_access::grant=ota_access::Role::Developer;
 }
 for(int failure:{1,2}){saved=2;marker=writes=0;failAt=failure;boot();assert(releaseChannel()==ReleaseChannel::Public);int attempts=writes;assert(releaseChannel()==ReleaseChannel::Public&&writes==attempts);assert(!setReleaseChannel(ReleaseChannel::Private));failAt=0;boot();assert(releaseChannel()==ReleaseChannel::Public&&marker==1&&saved==0);}
 saved=1;marker=1;auth=false;boot();assert(releaseChannel()==ReleaseChannel::Public);assert(!setReleaseChannel(ReleaseChannel::Private));
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code);subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
print('PASS Public default once, preserved explicit developer choices, role gates, partial writes and bounded persistence attempts')
