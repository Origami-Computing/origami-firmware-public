"""Compare actual transport preparation with the pre-extraction operation order."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
BASELINES=ROOT/'test/fixtures/agent_baselines'
def function(source,signature):
    start=source.index(signature+' {');end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
old=(BASELINES/'transport.cpp').read_text()
join=(BASELINES/'home_join.cpp').read_text()
code=r'''
#include <cassert>
#include <vector>
#include <string>
#include <atomic>
struct VoiceCallbacks { int value; } callbacks{1}, previous{2};
std::vector<std::string> events;
bool online,spawnOk,connectOk;
void* s_sendHandle;
void kamiSendTask(void*){}
struct Callbacks {
 const VoiceCallbacks* value;
 void store(const VoiceCallbacks* v){events.push_back("bind");value=v;}
} s_callbacks;
struct Logger {void println(const char* s){events.push_back(s);}} Serial;
namespace hal {namespace wifi {
 bool connected(){events.push_back("connected");return online;}
 bool connect(){events.push_back("connect");return connectOk;}
} namespace tasks {
 enum class Id{KamiSend};
 void* spawn(Id,void(*)(void*)){events.push_back("spawn");return spawnOk?(void*)&callbacks:nullptr;}
}}
namespace services {namespace agent_coordination {
 void workerStarted(){events.push_back("worker-started");}
}}
namespace before {
'''
code+=function(old,'bool ensureStarted()')+'\n'
code+=function(old,'void bindVoiceCallbacks(const VoiceCallbacks& callbacks)')+'\n'
code+='bool prepare(const VoiceCallbacks& callbacks,bool reconnectIfOffline) {\nif(reconnectIfOffline){\n'+join+'}\nbindVoiceCallbacks(callbacks);return ensureStarted();\n}\n}\nnamespace after {\n'
code+=function((ROOT/'src/services/agent_transport.cpp').read_text(),'bool prepare(const VoiceCallbacks& callbacks, bool reconnectIfOffline)')+'\n}\n'
code+=r'''
int main(){
 unsigned cases=0;
 for(unsigned mask=0;mask<64;++mask){
  online=mask&1;spawnOk=mask&2;connectOk=mask&4;
  const bool existing=mask&8,reconnect=mask&16,bound=mask&32;
  auto reset=[&](){events.clear();s_sendHandle=existing?(void*)&previous:nullptr;s_callbacks.value=bound?&previous:nullptr;};
  reset();bool expected=before::prepare(callbacks,reconnect);
  auto expectedEvents=events;auto expectedHandle=s_sendHandle;auto expectedCallback=s_callbacks.value;
  reset();bool actual=after::prepare(callbacks,reconnect);
  assert(actual==expected && events==expectedEvents && s_sendHandle==expectedHandle && s_callbacks.value==expectedCallback);
  assert(actual==(existing||spawnOk));assert(s_callbacks.value==&callbacks);
  ++cases;
 }
 assert(cases==64);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS 64 differential startup cases: join policy/results, callback replacement, existing/missing worker, spawn failure and exact event/log order')
