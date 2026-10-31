"""Check actual telemetry POST and file-sync JSON request credential lifetime.

Default mode requires stable request authentication and rejects stale token
invalidation. --expect-bug explicitly checks the current defect instead; its
success is a reproduction, never a firmware acceptance gate. No device/network
access. Synthetic credential storage and HTTP boundaries are test doubles.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--expect-bug', action='store_true')
parser.add_argument('--source-ref')
args = parser.parse_args()
def source(path):
    return (subprocess.check_output(['git', 'show', args.source_ref+':'+path], cwd=ROOT, text=True)
            if args.source_ref else (ROOT / path).read_text())
def function(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
poll_source = source('src/services/net_poll.cpp')
post = function(poll_source, 'int postJson(')
registration = function(poll_source, 'bool ensureRegistered(')
fs = source('src/services/device_fs.cpp')
adapter = ''
if 'template<class Transport>\nclass JsonClient' in fs:
    start = fs.index('template<class Transport>\nclass JsonClient')
    adapter = 'constexpr size_t kChunk=1024;\n' + fs[start:fs.index('// Operation batches', start)]
functions = registration + '\n' + post + '\nnamespace fs {\n' + adapter + '\n'.join(function(fs, marker) for marker in
    ['bool ok2xx(', 'void addAuth(', 'int httpGetJson(', 'int httpPostJson(']) + '\n}\n'
code = r'''
#include "core/secrets.h"
#include "../src/services/registration_retry.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <initializer_list>
struct String : std::string {
 using std::string::string;
 String(const std::string& value):std::string(value){}
 bool startsWith(const char* prefix) const {return find(prefix)==0;}
};
char liveUrl[192], liveKey[192], liveToken[192];
int mutation, responseCode, clears, posts, caller;
uint32_t revision=0;
bool beganTls, ended;
std::string requestUrl;
std::map<std::string,std::string> headers;
void replaceBackend(int boundary) {
 if (mutation!=boundary) return;
 ++revision;
 strcpy(liveUrl,"https://replacement.invalid");
 strcpy(liveKey,"replacement-key");
 strcpy(liveToken,"replacement-token");
}
bool clockSynced=true;
namespace core {namespace time {bool isSynced(){return clockSynced;}} namespace secrets {
const char* backendUrl(){return liveUrl;}
const char* apiKey(){return liveKey;}
const char* deviceToken(){return liveToken;}
BackendRequest backendRequest(){BackendRequest r{};strcpy(r.backend.url,liveUrl);strcpy(r.backend.apiKey,liveKey);strcpy(r.token,liveToken);r.revision=revision;return r;}
bool setDeviceTokenFor(const BackendRequest& r,const char* token){if(r.revision!=revision)return false;++clears;strcpy(liveToken,token);++revision;return true;}
bool setDeviceToken(const char* token){++clears;strcpy(liveToken,token);return true;}
}namespace device {const char* id(){return "synthetic-device";}const char* firmwareVersion(){return "synthetic";}}}
services::net_poll::RegistrationRetry s_registrationRetry;
bool s_onboardingRegistration=false, s_confirmOnboarding=false;
int s_healthMux=0;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
uint32_t retryAfterMs(const String&){return 0;}
namespace services {namespace agent_coordination {
bool isSessionActive(){return false;}
struct TransportPause {bool acquire(uint32_t){return true;}};
}}
struct WiFiClientSecure;
namespace services {namespace tls {void useRootBundle(WiFiClientSecure&);}}
struct WiFiClient {bool tls=false;virtual size_t write(const uint8_t*,size_t n){return n;}};
struct WiFiClientSecure:WiFiClient {
 WiFiClientSecure(){tls=true;}
 int lastError(char*,size_t){return 0;}
 void setInsecure(){} void setHandshakeTimeout(int seconds){assert(seconds==8);}
};
void services::tls::useRootBundle(WiFiClientSecure&){}
#define DEVICE_REGISTER_PATH "/register"
#define LOGW(...) ((void)0)
#define LOGI(...) ((void)0)
uint32_t millis(){return 20000;}
namespace core {namespace watchdog {void feed(){}}}
namespace services {namespace net_poll {bool stopRequested(){return false;}}}
bool due(uint32_t,uint32_t*,uint32_t interval){assert(interval==15000);return true;}
struct JsonDocument {struct Value {void operator=(const char*){} const char* operator|(const char*){return "issued-token";}}; Value operator[](const char*){return {};}};
void serializeJson(JsonDocument&,String& value){value="{}";}
int deserializeJson(JsonDocument&, const String&){return 0;}
struct HTTPClient {
 void setConnectTimeout(int value){assert(value==5000);}
 void setTimeout(int value){assert(value==(caller==0?5000:caller==1?6000:8000));}
 bool begin(WiFiClient& client,const String& url){
  beganTls=client.tls;requestUrl=url;replaceBackend(1);return true;
 }
 void collectHeaders(const char**,int){} String header(const char*){return "";}
 void addHeader(const char* name,const String& value){headers[name]=value;}
 int POST(String& body){assert(body=="{}");++posts;replaceBackend(2);return responseCode;}
 int GET(){++posts;replaceBackend(2);return responseCode;}
 String getString(){return String("{}");}
 void end(){ended=true;}
};
struct {template<class... T>void printf(const char*,T...) {}} Serial;
struct {unsigned getFreeHeap(){return 12345;}} ESP;
''' + functions + r'''
int main() {
 unsigned cases=0, mixed=0, staleClears=0;
 for(caller=0;caller<3;++caller)for(bool tls:{false,true})for(bool bearer:{false,true})
 for(int boundary:{0,1,2})for(int status:{200,401,503}) {
  strcpy(liveUrl,tls?"https://original.invalid":"http://original.invalid");
  strcpy(liveKey,"original-key");strcpy(liveToken,bearer?"original-token":"");
  mutation=boundary;responseCode=status;clears=posts=0;ended=false;headers.clear();
  JsonDocument output;
  const int result=caller==0?postJson("/synthetic",String("{}")):caller==1?fs::httpGetJson("/synthetic",output):fs::httpPostJson(String("/synthetic"),String("{}"));
  assert(result==status && ended && posts==1 && beganTls==tls);
  assert(requestUrl==std::string(tls?"https://":"http://")+"original.invalid/synthetic");
  if(caller!=1)assert(headers.at("Content-Type")=="application/json");
  assert(headers.at("X-Device-Id")=="synthetic-device");
  const bool stableAuth=bearer
   ? headers.count("Authorization") && headers.at("Authorization")=="Bearer original-token" && !headers.count("X-Api-Key")
   : caller==0 ? headers.count("X-Api-Key") && headers.at("X-Api-Key")=="original-key" && !headers.count("Authorization")
   : !headers.count("Authorization") && !headers.count("X-Api-Key");
  const bool staleClear=boundary!=0 && status==401 && liveToken[0]=='\0';
  mixed+=!stableAuth;staleClears+=staleClear;
  if(EXPECT_BUG) {
   assert(stableAuth==(boundary!=1));
   assert(clears==(status==401));
   assert(staleClear==(boundary!=0 && status==401));
  } else {
   assert(stableAuth && "request must retain authentication from its original backend");
   assert(!staleClear && "old response must not clear replacement backend token");
  }
  if(boundary==0) assert(std::string(liveToken)==(status==401?"":bearer?"original-token":""));
  else if(status!=401) assert(std::string(liveToken)=="replacement-token");
  ++cases;
 }
 for(bool tls:{false,true})for(int boundary:{0,1,2})for(int status:{200,201,401,503}) {
  caller=0;strcpy(liveUrl,tls?"https://original.invalid":"http://original.invalid");
  strcpy(liveKey,"original-key");liveToken[0]='\0';mutation=boundary;responseCode=status;
  clears=posts=0;ended=false;headers.clear();
  s_registrationRetry=services::net_poll::RegistrationRetry();
  s_onboardingRegistration=s_confirmOnboarding=false;
  const bool registered=ensureRegistered();
  const bool success=status==200||status==201;
  assert(registered==(success&&(EXPECT_BUG||boundary==0)));
  assert(requestUrl==std::string(tls?"https://":"http://")+"original.invalid/register");
  assert(headers.at("X-Api-Key")==((EXPECT_BUG&&boundary==1)?"replacement-key":"original-key"));
  assert(std::string(liveToken)==(success&&(EXPECT_BUG||boundary==0)?"issued-token":boundary?"replacement-token":""));
  assert(posts==1&&ended);++cases;
 }
 // A repeat scan must confirm last_seen despite an existing token; ordinary
 // periodic ticks must remain a no-op once the confirmation succeeds.
 caller=0;mutation=0;responseCode=200;posts=clears=0;
 strcpy(liveUrl,"https://original.invalid");strcpy(liveKey,"original-key");strcpy(liveToken,"existing-token");
 s_registrationRetry=services::net_poll::RegistrationRetry();s_confirmOnboarding=false;s_onboardingRegistration=false;
 assert(ensureRegistered() && posts==0);
 s_onboardingRegistration=true;clockSynced=false;
 assert(!ensureRegistered() && posts==0 && s_confirmOnboarding && s_registrationRetry.ready(millis()));
 clockSynced=true;
 assert(ensureRegistered() && posts==1 && !s_confirmOnboarding);
 assert(ensureRegistered() && posts==1);
 printf("%s: %u cases, %u mixed requests, %u stale token clears\n",
        EXPECT_BUG?"REPRODUCED (not acceptance)":"PASS",cases,mixed,staleClears);
}
'''
code = code.replace('EXPECT_BUG', 'true' if args.expect_bug else 'false')
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'probe.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++11', '-fsanitize=address,undefined', '-I'+str(ROOT/'include'),
                    str(path / 'probe.cpp'), '-o', str(path / 'probe')], check=True)
    subprocess.run([str(path / 'probe')], check=True)
