"""Verify actual Agent teardown owns cancellation before releasing resources."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/apps/agent_app.cpp').read_text()
def function(signature):
    start=s.index(signature+' {');end=s.index('{',start)+1;depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]
code=r'''
#include <cassert>
#include <vector>
#include <initializer_list>
std::vector<int> events;
int feedbackStops=0,delegationCancels=0;
void stopEyesFeedback(){++feedbackStops;}
namespace motion {void cancel(){}}
namespace ui {namespace listening_style {void releaseCache(){}}}
struct App {};namespace apps {namespace menu {App app;}}
struct NavHandler {
 int depth_=1;App* pushed=nullptr;int pops=0;
 static NavHandler& instance(){static NavHandler nav;return nav;}
 void runtimePolicyChanged();
 int depth(){return depth_;}void push(App* app){pushed=app;}void pop(){++pops;}
};
enum class Stage {Eyes,Recording,Thinking,Reply,Error};
enum class CancelTarget {None,Eyes};
struct Conv {void release(){events.push_back(8);}};
struct PreviousReply {void release(){events.push_back(10);}};
struct AgentState {Stage stage;CancelTarget cancelResult;bool recStartedByChord;int chordHoldDeadlineMs,pcmN;Conv conv;PreviousReply previousReply;bool hasPreviousReply;} state;
AgentState& S(){return state;}
void NavHandler::runtimePolicyChanged(){assert(state.stage==Stage::Eyes);events.push_back(9);}
bool isRecording(){return state.stage==Stage::Recording;}
bool isSending(){return state.stage==Stage::Thinking;}
namespace services {namespace agent_voice {
void releaseResources(){events.push_back(1);}
void finish(){events.push_back(2);}
int samples(){return 123;}
void restorePlayback(){events.push_back(5);}
void releaseBuffers(){events.push_back(7);}
}}
namespace flow {void cancelDelegation(){++delegationCancels;}void cancelSend(CancelTarget){events.push_back(0);}}
namespace sfx {void typewriterTeardown(){events.push_back(4);}}
namespace render {void releaseChatSprites(AgentState&){events.push_back(6);}}
'''
code+=function('void requestExitToMenu()')+'\n'+function('void requestAbortSendingToEyes()')+'\n'+function('void exitApp()')+'\n'+function('static void prepareForSleep()')
code+=r'''
int main(){
for(int depth:{0,1,2,8}){
 auto& nav=NavHandler::instance();nav.depth_=depth;nav.pushed=nullptr;nav.pops=0;
 requestExitToMenu();
 assert(nav.pushed==(depth<=1?&apps::menu::app:nullptr));
 assert(nav.pops==(depth>1?1:0));
}
for(Stage stage:{Stage::Eyes,Stage::Recording,Stage::Thinking,Stage::Reply,Stage::Error}){
 state.stage=stage;state.hasPreviousReply=true;events.clear();feedbackStops=delegationCancels=0;exitApp();assert(feedbackStops==1&&delegationCancels==1);assert(state.stage==Stage::Eyes&&!state.hasPreviousReply);
 if(stage==Stage::Thinking)assert((events==std::vector<int>{0,1,4,5,9,6,7,8,10}));
 else if(stage==Stage::Recording)assert((events==std::vector<int>{1,2,0,4,5,9,6,7,8,10}));
 else assert((events==std::vector<int>{1,4,5,9,6,7,8,10}));
 state.stage=stage;events.clear();feedbackStops=delegationCancels=0;prepareForSleep();
 assert(feedbackStops==((stage==Stage::Thinking||stage==Stage::Recording)?2:1));
 assert(delegationCancels==((stage==Stage::Thinking||stage==Stage::Recording)?1:0));
 if(stage==Stage::Thinking)assert((events==std::vector<int>{0,0,1,4,5,9,6,7,8,10}));
 else if(stage==Stage::Recording)assert((events==std::vector<int>{1,2,0,4,5,9,6,7,8,10}));
 else assert(events.empty()&&state.stage==stage);
}
}
'''
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);(root/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined',str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
print('PASS Agent menu root/popup semantics and teardown cancels Thinking, finishes/abandons Recording, and releases resources in order for all stages')
