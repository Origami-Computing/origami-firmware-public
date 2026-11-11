"""Compile the actual delivery receipt owner with fake clock/network and sanitizer checks."""
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temp:
 d=Path(temp);(d/'freertos').mkdir()
 (d/'Arduino.h').write_text('''#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
extern uint32_t now;
inline uint32_t millis(){return now;}
inline size_t strlcpy(char* d,const char* s,size_t n){size_t z=strlen(s);if(n){strncpy(d,s,n-1);d[n-1]=0;}return z;}
struct Logger {template<class...T>void printf(const char*,T...){}};extern Logger Serial;
''')
 (d/'freertos/FreeRTOS.h').write_text('#pragma once\n')
 (d/'freertos/portmacro.h').write_text('#pragma once\n#include <mutex>\nusing portMUX_TYPE=std::mutex;\n#define portMUX_INITIALIZER_UNLOCKED {}\n#define portENTER_CRITICAL(p) (p)->lock()\n#define portEXIT_CRITICAL(p) (p)->unlock()\n')
 (d/'test.cpp').write_text('''#include <Arduino.h>
#include "services/agent_delivery.h"
#include <cassert>
#include <string>
#include <vector>
using namespace services::agent_delivery;
uint32_t now=100;Logger Serial;
std::vector<std::string> wire;
bool send(const char* s){wire.push_back(s);return true;}
int main(){
 received("reply-1");pump(send);assert(wire.size()==1&&wire.back().find("received")!=std::string::npos);
 pump(send);assert(wire.size()==1);now+=2001;pump(send);assert(wire.size()==2);
 confirmed("reply-1","received");bind("reply-1",0);pump(send);assert(wire.size()==2);
 displayed(0);pump(send);assert(wire.size()==3&&wire.back().find("displayed")!=std::string::npos);
 confirmed("reply-1","displayed");displayed(0);dismiss();now+=2001;pump(send);assert(wire.size()==3);
 received("reply-2");bind("reply-2",1);confirmed("reply-2","received");dismiss();displayed(1);pump(send);
 assert(wire.back().find("rejected")!=std::string::npos&&wire.back().find("left_app")!=std::string::npos);
 confirmed("reply-2","rejected");received("commit");bind("commit",-2);dismiss();pump(send);assert(wire.back().find("received")!=std::string::npos);
 confirmed("commit","received");auto count=wire.size();now+=2001;pump(send);assert(wire.size()==count);
 received("old");bind("old",0);confirmed("old","received");received("new");confirmed("new","received");replaceView();bind("new",0);displayed(0);pump(send);
 assert(wire.back().find("new")!=std::string::npos&&wire.back().find("displayed")!=std::string::npos);
 puts("PASS real receipt owner: receipt/display separation, retry, server confirmation, cancel, commit and view replacement");
}
''')
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(d),'-I'+str(root/'include'),str(root/'src/services/agent_delivery.cpp'),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
 source=(root/'src/apps/agent/agent_flow.cpp').read_text()
 timeout=source[source.index('void pollDelegation('):source.index('void acknowledgeDisplayed(')]
 (d/'timeout.cpp').write_text('#include <cstdint>\n#include <cassert>\nuint8_t s_pendingDelegations=1;uint32_t s_delegationStarted=1000;int cancels=0;void cancelDelegation(){++cancels;}\n'+timeout+'''int main(){
 pollDelegation(999);assert(cancels==0);pollDelegation(1000+900000);assert(cancels==0);
 pollDelegation(1000+900001);assert(cancels==1);
 s_delegationStarted=0xfffffff0u;pollDelegation(10);assert(cancels==1);
 }''')
 subprocess.run(['c++','-std=c++17',str(d/'timeout.cpp'),'-o',str(d/'timeout')],check=True)
 subprocess.run([str(d/'timeout')],check=True)
 print('PASS delegation deadline: newer start timestamp, timeout boundary and millis wrap')
with tempfile.TemporaryDirectory() as temp:
 d=Path(temp)
 source=(root/'src/apps/agent_app.cpp').read_text()
 policy=source[source.index('static core::health::RuntimePolicy runtimePolicy()'):source.index('static void prepareForSleep()')]
 (d/'policy.cpp').write_text('''#include "core/health_policy.h"
#include <cassert>
bool home=false,waiting=false;namespace flow{bool waitingForDelegation(){return waiting;}}
bool isRecording(){return false;}bool isSending(){return false;}bool isOnReplyScreen(){return !home;}bool isShowingError(){return false;}bool isAtHomescreen(){return home;}
'''+policy+'''int main(){
 for(int i=0;i<2;++i){home=i;waiting=true;auto p=runtimePolicy();assert(!p.allowIdleReturn&&!p.allowBatterySleep&&!p.allowBackgroundUploads);
 waiting=false;p=runtimePolicy();assert(p.allowIdleReturn&&p.allowBackgroundUploads);if(home)assert(p.allowBatterySleep);}
}''')
 subprocess.run(['c++','-std=c++17','-I'+str(root/'include'),str(d/'policy.cpp'),'-o',str(d/'policy')],check=True)
 subprocess.run([str(d/'policy')],check=True)
 print('PASS delegated reply policy blocks idle return and battery sleep, releases on cancellation')
