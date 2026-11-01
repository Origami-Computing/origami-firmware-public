#!/usr/bin/env python3
"""Run actual OTA authorization/start code for enrolled and legacy-only devices."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/services/ota_client.cpp').read_text()
a=s[s.index('static bool authorizeRelease('):s.index('bool fetchLatestRelease(')]
b=s[s.index('bool beginFlash('):s.index('bool pumpFlash(')]
code=r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "services/ota_client.h"
using namespace services::ota_client;
#define OTA_BASE_URL "https://fixture.invalid"
#define OTA_HARDWARE_ID "n16r8-rev4"
#define OTA_LAYOUT_ID "n16r8-ota-v1"
bool validKey=false,network=false,spawnOk=true;int authCalls=0,spawns=0;
char s_err[96]={},s_downloadKey[65]="old-developer-bearer";
void setError(const char*s){snprintf(s_err,sizeof(s_err),"%s",s);}
namespace ota_access {enum class Role{Unknown,Production,Developer};Role granted=Role::Unknown;Role role(){return granted;}bool read(char(&key)[65]){if(!validKey)return false;strcpy(key,"synthetic-key");return true;}}
bool refreshAccess(){++authCalls;if(!validKey||!network){setError("access failed");return false;}return true;}
struct Partition{size_t size=6000000;}part;const Partition*s_candidate=nullptr;
const Partition*esp_ota_get_next_update_partition(void*){return &part;}
ReleaseInfo s_release{};
enum class DownloadState{Idle,Running,Failed};std::atomic<DownloadState>s_download{DownloadState::Idle};std::atomic<int>s_written{0};std::atomic<bool>s_cancel{false};
const char*channelPath(ReleaseChannel c){return c==ReleaseChannel::Public?"public":c==ReleaseChannel::Private?"private":"staging";}
void downloadTask(void*){}
namespace hal{namespace tasks{enum class Id{OtaDownload};bool spawn(Id,void(*)(void*)){++spawns;return spawnOk;}}}
namespace services {namespace ota_client {void abortFlash(){s_download=DownloadState::Idle;}}}
'''+a+'\nnamespace services {namespace ota_client {\n'+b+'\n}}\n'+r'''
ReleaseInfo release(ReleaseChannel c){ReleaseInfo r{};r.channel=c;r.size=100;snprintf(r.url,sizeof(r.url),OTA_BASE_URL "/releases/%s/" OTA_HARDWARE_ID "/" OTA_LAYOUT_ID "/v1.0.5/hash/firmware.bin",channelPath(c));return r;}
int main(){
 auto pub=release(ReleaseChannel::Public),priv=release(ReleaseChannel::Private),stage=release(ReleaseChannel::Staging);
 assert(authorizeRelease(ReleaseChannel::Public)&&authCalls==0);
 assert(beginFlash(pub)&&spawns==1&&authCalls==0&&s_downloadKey[0]==0);
 assert(!beginFlash(priv)&&spawns==1);assert(!beginFlash(stage)&&spawns==1);
 validKey=network=true;ota_access::granted=ota_access::Role::Production;
 assert(!beginFlash(priv)&&spawns==1);assert(!beginFlash(stage)&&spawns==1);
 ota_access::granted=ota_access::Role::Developer;
 assert(beginFlash(priv)&&spawns==2&&s_downloadKey[0]);
 assert(beginFlash(stage)&&spawns==3);
 validKey=network=false;int before=authCalls;
 assert(beginFlash(pub)&&spawns==4&&authCalls==before&&s_downloadKey[0]==0);
 auto bad=pub;strcpy(bad.url,"https://other.invalid/firmware.bin");assert(!beginFlash(bad)&&spawns==4);
 bad=pub;bad.size=6000001;assert(!beginFlash(bad)&&spawns==4);
 bad=pub;bad.channel=static_cast<ReleaseChannel>(42);assert(!beginFlash(bad)&&spawns==4);
 spawnOk=false;assert(!beginFlash(pub)&&s_download==DownloadState::Failed);
 puts("PASS Public without enrollment; developer-only Private/Staging; no stale bearer; origin, size and admission guards");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(root/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
