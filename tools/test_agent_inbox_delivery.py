"""Real inbox: leaving an app, receipt identity, replay cursor, and re-entry."""
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temp:
 d=Path(temp)
 for sub in ['freertos','core','widgets','services']:(d/sub).mkdir()
 (d/'Arduino.h').write_text('''#pragma once
#include <cstdio>
#include <cstdint>
#include <cstring>
inline size_t strlcpy(char* d,const char* s,size_t n){size_t z=strlen(s);if(n){strncpy(d,s,n-1);d[n-1]=0;}return z;}
struct Logger{template<class...T>void printf(const char*,T...){}void println(const char*){}};extern Logger Serial;
''')
 (d/'freertos/FreeRTOS.h').write_text('#pragma once\n')
 (d/'freertos/portmacro.h').write_text('#pragma once\n#include <mutex>\nusing portMUX_TYPE=std::mutex;\n#define portMUX_INITIALIZER_UNLOCKED {}\n#define portENTER_CRITICAL(p) (p)->lock()\n#define portEXIT_CRITICAL(p) (p)->unlock()\n')
 (d/'core/memory.h').write_text('#pragma once\n#include <cstdlib>\nnamespace core{namespace memory{inline void* allocPsram(const char*,size_t n){return malloc(n);}inline void freePsram(void*& p){free(p);p=nullptr;}}}\n')
 (d/'widgets/launch.h').write_text('#pragma once\nnamespace widgets{namespace launch{inline void notifyLoading(const char*){}inline void notifyReady(const char*,const char*){}inline void notifyError(const char*){}}}\n')
 (d/'services/agent_delivery.h').write_text('#pragma once\nnamespace services{namespace agent_delivery{void reject(const char*,const char*);}}\n')
 (d/'test.cpp').write_text('''#include <Arduino.h>
#include "services/agent_inbox.h"
#include <cassert>
#include <string>
#include <vector>
Logger Serial;std::vector<std::string> rejected;
namespace services{namespace agent_delivery{void reject(const char* id,const char*){rejected.push_back(id);}}}
using namespace services::agent_inbox;
int main(){
 assert(!offerReply(1,"late","push:1"));assert(lastReplyEventId()==1&&!hasReply());
 setAccepting(true);assert(!offerReply(1,"replay","push:1"));assert(!hasReply());
 assert(offerReply(2,"answer","push:2"));assert(hasReply());
 assert(takeReply([](const char* t,const char* id){return !strcmp(t,"answer")&&!strcmp(id,"push:2");}));assert(!hasReply());
 assert(offerReply(3,"queued","push:3"));setAccepting(false);assert(!hasReply()&&rejected.back()=="push:3");
 assert(!offerReply(4,"after cover","push:4"));assert(lastReplyEventId()==4);
 setAccepting(true);assert(!offerReply(4,"stale after reconnect","push:4"));assert(offerReply(5,"fresh","push:5"));
 setAccepting(false);assert(!hasReply());
 puts("PASS inbox receipt identity, app cover cleanup, rejected cursor advancement and no stale replay after re-entry");
}
''')
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(d),'-I'+str(root/'include'),str(root/'src/services/agent_inbox.cpp'),str(d/'test.cpp'),'-o',str(d/'test')],check=True)
 subprocess.run([str(d/'test')],check=True)
