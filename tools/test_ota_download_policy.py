#!/usr/bin/env python3
"""Exercise real fetch/flash authorization before any network image effects."""
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[1];s=(r/'src/services/ota_client.cpp').read_text()
def prefix(name,marker):
 start=s.index(name);end=s.index(marker,start)
 return s[start:end]+' return true;\n}\n'
code=r'''
#include <atomic>
#include <cstdint>
#include <cassert>
#include <initializer_list>
#include "services/ota_client.h"
using namespace services::ota_client;
namespace ota_access {enum class Role{Unknown,Production,Developer};Role granted=Role::Unknown;Role role(){return granted;}}
bool authenticated=true;bool refreshAccess(){return authenticated && ota_access::role()!=ota_access::Role::Unknown;}
void setError(const char*){} void abortFlash(){} char s_err[100];
enum class DownloadState{Running,Idle};std::atomic<DownloadState>s_download{DownloadState::Idle};
'''+prefix('bool fetchLatestRelease(', '    if (!OTA_BASE_URL')+prefix('bool beginFlash(', '    char prefix[')+r'''
int main(){
 for(auto role:{ota_access::Role::Unknown,ota_access::Role::Production,ota_access::Role::Developer}){
  ota_access::granted=role;
  for(auto channel:{ReleaseChannel::Public,ReleaseChannel::Private,ReleaseChannel::Staging}){
   ReleaseInfo info{};info.channel=channel;
   const bool allowed=role==ota_access::Role::Developer||(role==ota_access::Role::Production&&channel==ReleaseChannel::Public);
   assert(fetchLatestRelease(channel,&info)==allowed);assert(beginFlash(info)==allowed);
   authenticated=false;assert(!fetchLatestRelease(channel,&info)&&!beginFlash(info));authenticated=true;
  }
 }
 ReleaseInfo invalid{};invalid.channel=static_cast<ReleaseChannel>(99);
 assert(!fetchLatestRelease(invalid.channel,&invalid)&&!beginFlash(invalid));
}
'''
# Definitions must be in their actual namespace to avoid ambiguous declarations.
code=code.replace('using namespace services::ota_client;', 'namespace services { namespace ota_client {')
code=code.replace('\nint main(){','\n}}\nusing namespace services::ota_client;\nint main(){')
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-DORIGAMI_PRODUCTION_BUILD=1','-I'+str(r/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS actual manifest/download entry points enforce the three-channel role matrix')
