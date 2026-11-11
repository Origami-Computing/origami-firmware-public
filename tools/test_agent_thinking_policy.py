"""Differential event traces for real old/new Thinking ticks and response policy."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def function(source,signature):
 start=source.index(signature);brace=source.index('{',start);level=1;i=brace+1
 while level:
  level+=(source[i]=='{')-(source[i]=='}');i+=1
 return source[start:i]
old=(ROOT/'test/fixtures/agent_baselines/thinking.cpp').read_text()
new=(ROOT/'src/apps/agent/screen_thinking.cpp').read_text()
policy=(ROOT/'src/apps/agent/agent_flow.cpp').read_text()
before=function(old,'void onTick(');after=function(new,'void onTick(');poll=function(policy,'void pollThinking(')
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)/'check.cpp'
 p.write_text('''
#include <stdint.h>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <vector>
#include <string>
#include "apps/agent/agent_config.h"
struct AgentState {uint8_t thinkDotPhase=255;char thinkStatus[64]={};};
uint32_t sThinkingEnteredMs=0,clockMs=0;
unsigned flags=0,commitReads=0;
std::vector<unsigned long> events;
uint32_t millis(){events.push_back(1);return clockMs;}
struct SerialFake {void printf(const char*,int committed,unsigned long elapsed){events.push_back(2);events.push_back(committed);events.push_back(elapsed);}} Serial;
namespace services {namespace agent_latency {void fail(){}}}
namespace motion {bool tick(AgentState&,uint32_t){return false;}}
namespace render {void paintThinkingShell(AgentState&s,uint8_t phase){events.push_back(100+phase);s.thinkDotPhase=phase;}}
namespace flow {
bool isCommitSent(){events.push_back(3);return (flags>>(commitReads++%3))&1;}
bool isReplyReady(){events.push_back(4);return flags&8;}
bool isSendDone(){events.push_back(5);return flags&16;}
void showReplyIfReady(){events.push_back(6);}
void finalizeSend(){events.push_back(7);}
bool pollCompletion(){if(!isSendDone())return false;finalizeSend();return true;}
void returnToHome(){events.push_back(8);}
namespace transport {struct Status {bool replyReady;};Status status(){return {isReplyReady()};}}
''' + poll + '''
}
namespace before {
''' + before + '''
}
namespace after {
''' + after + '''
}
int main(){
 unsigned cases=0;
 const uint32_t nowValues[]={0,1,999,2000,cfg::agent::kConnectGraceMs+2000,cfg::agent::kConnectGraceMs+2001,cfg::agent::kResponseTimeoutMs+2000,cfg::agent::kResponseTimeoutMs+2001,0xffffffffu};
 for(unsigned f=0;f<32;++f)for(uint32_t entered:{0u,1u,1234u,0xfffffff0u})for(uint32_t now:nowValues)for(uint32_t tick:{0u,301u,601u,0xffffffffu})for(bool painted:{false,true}) {
  flags=f;sThinkingEnteredMs=entered;clockMs=now;
  AgentState a,b;
  if(painted){a.thinkDotPhase=b.thinkDotPhase=1;strcpy(a.thinkStatus,"Thinking");strcpy(b.thinkStatus,"Thinking");}
  events.clear();commitReads=0;before::onTick(a,tick);const auto expected=events;
  events.clear();commitReads=0;after::onTick(b,tick);
  assert(events==expected);assert(a.thinkDotPhase==b.thinkDotPhase);assert(strcmp(a.thinkStatus,b.thinkStatus)==0);++cases;
 }
 printf("PASS %u old/new event traces: paint order, changing commit snapshots, reply/done precedence, watchdog boundaries and clock rollover\\n",cases);
}
''')
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p),'-o',str(p.with_suffix(''))],check=True)
 subprocess.run([str(p.with_suffix(''))],check=True)
