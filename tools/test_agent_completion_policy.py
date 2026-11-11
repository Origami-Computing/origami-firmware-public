"""Production completion policy across every stage, result and allocation outcome."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
BASELINES=ROOT/'test/fixtures/agent_baselines'
def source(path,old=False):
 if old:
  name={'src/apps/agent/agent_flow.cpp':'completion.cpp','src/apps/agent/screen_reply.cpp':'reply.cpp'}[path]
  return (BASELINES/name).read_text()
 return (ROOT/path).read_text()
def fn(s,name):
 a=s.index(name);b=s.index('{',a)+1;depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[a:b]
code=r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include "apps/agent/agent_config.h"
std::vector<std::string> events;
enum class Stage {Eyes,Recording,Thinking,Reply,Error};
enum class CancelTarget {None,Eyes,Menu};
enum class SendOutcome {Ok,HttpError,NetworkError,Cancelled};
bool convOk,doneFlag;
struct Conversation {bool ensure(){events.push_back("ensure-conversation");return convOk;}};
struct AgentState {Stage stage;Conversation conv;char errBuf[128];uint32_t lastAnim=0,animFrame=0,miniBlinkMs=0;bool miniEyesOpen=false,returnHomeAfterReply=false;uint32_t returnHomeAtMs=0;} s_state;
namespace services {namespace agent_voice {void poll(){events.push_back("voice-poll");}}}
namespace transport {
 struct Result {SendOutcome outcome;uint8_t cancelTag;uint8_t pendingTasks;} reply;
 struct Status {bool done;};
 Status status(){events.push_back("status");return {doneFlag};}
 Result result(){events.push_back("result");return reply;}
 void clearDone(){events.push_back("clear-done");doneFlag=false;}
}
void restoreAudio(){events.push_back("restore-audio");}
int delegated=-1;
void awaitDelegatedReplies(uint8_t count){delegated=count;}
void requestExitToMenu(){events.push_back("request-menu");}
void goTo(Stage s){events.push_back("stage-"+std::to_string((int)s));s_state.stage=s;}
struct Logger {void printf(const char* fmt,const char* error){events.push_back(std::string(fmt)+error);}} Serial;
namespace before {
'''
code=code.replace('namespace before {','')
code+='namespace flow {\n'+fn(source('src/apps/agent/agent_flow.cpp'),'bool pollCompletion()')+'\n}\n'
code+=r'''
int main(){unsigned cases=0;
 for(unsigned ready=0;ready<2;++ready)for(unsigned stage=0;stage<5;++stage)
 for(unsigned outcome=0;outcome<4;++outcome)for(unsigned tag=0;tag<3;++tag)
 for(unsigned allocate=0;allocate<2;++allocate)for(unsigned pending=0;pending<2;++pending){
  events.clear();delegated=-1;s_state={};s_state.stage=(Stage)stage;
  strcpy(s_state.errBuf,"test failure");convOk=allocate;doneFlag=ready;
  transport::reply={(SendOutcome)outcome,(uint8_t)tag,(uint8_t)pending};
  assert(flow::pollCompletion()==bool(ready));
  assert(delegated==((ready&&outcome==0)?int(pending):-1));
  Stage expected=(Stage)stage;
  if(ready){
   if(outcome==3)expected=Stage::Eyes;
   else if(outcome==0)expected=(stage==unsigned(Stage::Reply)||allocate)?Stage::Reply:Stage::Error;
   else expected=Stage::Error;
  }
  assert(s_state.stage==expected&&!doneFlag);
  if(!ready)assert(events==std::vector<std::string>{"status"});
  if(ready&&outcome==0&&stage!=unsigned(Stage::Reply)&&!allocate)
   assert(!strcmp(s_state.errBuf,cfg::agent::kErrReplyUi));
  else assert(!strcmp(s_state.errBuf,"test failure"));
  ++cases;
 }
 assert(cases==480);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS 480 completion cases: all stages, outcomes, cancel targets, readiness, delegation forwarding and conversation allocation failure')
