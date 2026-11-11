"""Regression: a tick sampled before screen entry must not finish its animation."""
from pathlib import Path
import subprocess,sys
root=Path(__file__).resolve().parents[1]
source=(root/'src/apps/agent/agent_motion.cpp').read_text()
start=source.index('unsigned ageAt(')
end=source.index('\nui::eye_rig::Pose',start)
code=source[start:end]
out=root/'.cache/agent-motion-clock';out.mkdir(parents=True,exist_ok=True)
cpp=out/'clock.cpp';exe=cpp.with_suffix('.exe')
cpp.write_text('''#include <cstdint>
#include <cassert>
#include "ui/agent_transition_motion.h"
uint32_t started=120;
'''+code+'''
int main(){
 assert(ageAt(100)==0); // old UI-loop timestamp
 assert(ageAt(120)==0);
 assert(ageAt(700)==580);
 auto begin=ui::agent_motion::replyHome(0);
 assert(begin.avatar==1 && begin.offsetX==0);
 assert(ui::agent_motion::replyHome(220).avatar<.2f);
 bool overshot=false;
 for(unsigned t=0;t<520;++t){
  auto f=ui::agent_motion::replyHome(t);
  assert(f.avatar>=0 && f.avatar<=1);
  assert(f.offsetX>=0 && f.offsetX<3.1f && f.offsetY<2.3f);
  if(f.offsetX>1)overshot=true;
 }
 assert(overshot);
 auto end=ui::agent_motion::replyHome(ui::agent_motion::kReplyHomeDurationMs);
 assert(end.avatar==0 && end.offsetX==0 && end.offsetY==0 && end.bubble==0);
 started=0xfffffff0u;
 assert(ageAt(0x20u)==48); // millis() wrap remains valid
 for(unsigned age:{0u,1u,100u,180u,500u,1000u,2000u}){
  auto f=ui::agent_motion::homeListen(age);
  assert(f.glow==1 && f.listening==1 && f.avatar==0 && f.body==0); // acknowledge before decorative movement
 }
}
''')
subprocess.run(([sys.argv[1],'c++'] if len(sys.argv)>1 else ['c++'])+['-std=c++11','-I'+str(root/'include'),str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
print('PASS stale UI ticks and millis wrap cannot skip transition frames')
