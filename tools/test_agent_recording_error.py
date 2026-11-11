"""Production session completion must stop capture before releasing its ring."""
from pathlib import Path
import subprocess, sys
root=Path(__file__).resolve().parents[1]
source=(root/'src/apps/agent/agent_flow.cpp').read_text()
def function(name):
    start=source.index('void '+name+'(')
    brace=source.index('{',start);depth=1;i=brace+1
    while depth:
        depth+=(source[i]=='{')-(source[i]=='}');i+=1
    return source[start:i]
out=root/'.cache/recording-error';out.mkdir(parents=True,exist_ok=True)
cpp=out/'test.cpp';exe=out/'test.exe'
prefix=r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#define LOGD(...) ((void)0)
enum class Stage { Eyes, Recording, Thinking, Reply, Error };
enum class SendOutcome { Ok, Cancelled, NetworkError };
enum class CancelTarget { None, Menu };
struct Conv {bool nonempty=true,allocate=true;void release(){nonempty=false;}void buildLog(){}bool anyNonEmpty(){return nonempty;}int segmentCount(){return nonempty;}bool ensure(){return allocate;} };
struct AgentState {Conv conv;Stage stage=Stage::Recording;CancelTarget cancelResult=CancelTarget::None;char errBuf[128]={};bool sendJustFinished=false,recStartedByChord=true,returnHomeAfterReply=false;uint32_t returnHomeAtMs=0;unsigned pcmN=0;} s_state;
int releases=0,commits=0,stops=0;bool capturing=true,releasePending=false;
namespace services {namespace agent_latency {void submit(){}void milestone(const char*){}} namespace agent_voice {
bool isCapturing(){return capturing;}
unsigned finish(){assert(capturing);capturing=false;++stops;return 123;}
unsigned samples(){return 123;}
void requestRelease(){assert(!capturing);releasePending=true;}
void poll(){assert(!capturing);if(releasePending){++releases;releasePending=false;}}
} namespace agent_transport {const char* outcomeName(SendOutcome){return "test";}}}
namespace transport {
struct Status {bool done;};bool done=false;
struct Result {SendOutcome outcome;uint8_t cancelTag;uint8_t pendingTasks;} completed{};
Status status(){return {done};}Result result(){return completed;}void clearDone(){done=false;}
}
namespace cfg {namespace agent {const char* kErrEmptyReply="empty";const char* kErrReplyUi="ui";}}
struct Logger {template<class... T> void printf(const char*,T...){} } Serial;
namespace sfx {void typewriterTeardown(){}}
void restoreAudio(){}void requestExitToMenu(){}void goTo(Stage s){s_state.stage=s;}
void awaitDelegatedReplies(uint8_t count){assert(count==0);}
void startSend(){++commits;}
'''
def extract(signature):
    start=source.index(signature);brace=source.index('{',start);depth=1;i=brace+1
    while depth:
        depth+=(source[i]=='{')-(source[i]=='}');i+=1
    return source[start:i]
tests=r'''
void complete(SendOutcome outcome,const char* error=""){
 transport::completed={finishTransport(outcome,error,12),0};transport::done=true;
}
int main(){
 complete(SendOutcome::NetworkError,"Sign-in failed");
 assert(!capturing&&stops==1&&releases==0);
 finishRecording(); // Releasing buttons after rejection must not commit or replace the diagnosis.
 assert(releases==1&&commits==0&&s_state.stage==Stage::Error);
 assert(!strcmp(s_state.errBuf,"Sign-in failed"));assert(!pollCompletion());
 capturing=true;s_state.stage=Stage::Recording;
 complete(SendOutcome::Cancelled);assert(pollCompletion());
 assert(s_state.stage==Stage::Eyes&&releases==2&&stops==2);
 s_state.stage=Stage::Thinking;s_state.conv.nonempty=true;
 complete(SendOutcome::Ok);assert(pollCompletion());
 assert(s_state.stage==Stage::Reply&&releases==3&&stops==2);
 s_state.stage=Stage::Thinking;s_state.conv.nonempty=false;
 complete(SendOutcome::Ok);pollCompletion();assert(s_state.stage==Stage::Error&&!strcmp(s_state.errBuf,"empty"));
 s_state.stage=Stage::Thinking;s_state.conv.nonempty=true;s_state.conv.allocate=false;
 complete(SendOutcome::Ok);pollCompletion();assert(s_state.stage==Stage::Error&&!strcmp(s_state.errBuf,"ui"));
 capturing=true;s_state.stage=Stage::Recording;s_state.conv.allocate=true;
 finishRecording();assert(!capturing&&commits==1&&s_state.stage==Stage::Thinking);
}
'''
cpp.write_text(prefix+extract('SendOutcome finishTransport(')+'\n'+extract('bool pollCompletion()')+'\n'+function('finishRecording')+tests)
subprocess.run(([sys.argv[1],'c++'] if len(sys.argv)>1 else ['c++'])+['-std=c++11','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
print('PASS recording rejection/release race, cancellation, successful/empty replies, UI allocation failure and normal commit')
