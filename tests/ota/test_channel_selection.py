"""Exercise the actual channel setter: failures preserve detail and preference."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class ChannelSelectionTests(unittest.TestCase):
    def test_failures_and_roles(self):
        source = (ROOT/'src/services/ota_client.cpp').read_text()
        start = source.index('bool setReleaseChannel(')
        end = source.index('\nconst char* releaseChannelName', start)
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include "core/release_policy.h"
enum class ReleaseChannel {Public=0,Production=Public,Staging=1,Private=2};
char s_err[96];
void setError(const char* s){strcpy(s_err,s);}
const char* kSettingsNamespace="settings";
const char* kChannelKey="channel";
bool network=true,auth=true,writeOk=true,key=true;
bool hasAuthToken(){return key;}
int saved=0,writes=0;
namespace core {namespace store {
bool putU8(const char*,const char*,int v){++writes;if(writeOk)saved=v;return writeOk;}
}}
namespace firmware_update {
struct Session {enum class Result{Ready,Busy};Result reserveNetwork(){return network?Result::Ready:Result::Busy;}};
}
namespace ota_access {
enum class Role{Unknown,Production,Developer};Role granted=Role::Developer;
Role role(){return granted;}
}
bool refreshAccess(){if(!auth)setError("Set up OTA access key");return auth;}
''' + source[start:end] + r'''
int main(){
 const auto staging=ReleaseChannel::Staging,production=ReleaseChannel::Production;
 key=false;network=false;assert(!setReleaseChannel(staging));
 assert(!strcmp(s_err,"Set up OTA access key")&&writes==0);
 key=network=true;
 auth=false;assert(!setReleaseChannel(staging));
 assert(!strcmp(s_err,"Set up OTA access key")&&saved==0&&writes==0);
 auth=true;network=false;assert(!setReleaseChannel(staging));
 assert(!strcmp(s_err,"OTA network busy")&&writes==0);
 network=true;ota_access::granted=ota_access::Role::Production;
 assert(!setReleaseChannel(staging)&&writes==0);
 assert(!setReleaseChannel(ReleaseChannel::Private)&&writes==0);
 assert(!strcmp(s_err,"Developer OTA access required"));
 ota_access::granted=ota_access::Role::Developer;
 writeOk=false;assert(!setReleaseChannel(staging)&&saved==0);
 assert(!strcmp(s_err,"Update channel save failed"));
 writeOk=true;assert(setReleaseChannel(staging)&&saved==1&&!s_err[0]);
 assert(setReleaseChannel(ReleaseChannel::Private)&&saved==2);
 auth=network=false;assert(setReleaseChannel(production)&&saved==0);
 assert(!setReleaseChannel(static_cast<ReleaseChannel>(42))&&saved==0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',
                            '-DORIGAMI_PRODUCTION_BUILD=1', '-I'+str(ROOT/'include'),
                            str(path/'test.cpp'),'-o',str(path/'test')],check=True)
            subprocess.run([str(path/'test')],check=True)
