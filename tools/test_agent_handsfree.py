"""Exercise the production A+B gesture, agent mode and its input without hardware."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
def function(file, signature):
    s = (root/file).read_text(); a = s.index(signature); b = s.index('{', a) + 1; depth = 1
    while depth: depth += (s[b] == '{') - (s[b] == '}'); b += 1
    return s[a:b]
code = r'''
#include <cstdint>
#include <cassert>
#include <initializer_list>
#include <vector>
#include "core/power_button.h"
enum class Stage {Eyes,Recording,Thinking,Reply,Error};
struct AgentState {
 enum class ConversationView { Listening, User, Thinking, Reply }; ConversationView conversationView=ConversationView::Listening;
 bool continuous=false,documentVisible=false,agentWorking=false;char documentId[37]={};
 bool handsFree=false,handsFreeReady=false,suppressHeldChord=false,recStartedByChord=false;
 Stage stage=Stage::Eyes;uint32_t transcriptPainted=0,chordHoldDeadlineMs=0,agentStartAtMs=0,agentActivityMs=0,pendingCueUntilMs=0;uint8_t pendingCue=0;uint32_t voiceEntryMs=0;bool gestureDeciding=false;
} s_state;
uint32_t clock_=1000;uint32_t millis(){return clock_;}
namespace cfg {namespace agent {constexpr uint32_t kTapHoldMs=250,kLockLeadMs=150,kAgentSilenceMs=60000,kCueWaitMs=800;}}
int starts=0,ends=0,homes=0,closed=0;
std::vector<int> played; bool speakerFree=true;
namespace sfx {namespace product {enum class Sound:uint8_t{WaterDrop,AddMenu,Typing,TalkStart,Send,Cancel,Lock,LibraryOpen,LibraryClose};
 bool play(Sound s){if(!speakerFree)return false;played.push_back((int)s);return true;}}}
using sfx::product::Sound;
namespace services {namespace agent_transport {void closeDocument(){++closed;}bool held=false,continuous=false,replyAudio=true;void holdStart(bool h){held=h;}void setContinuous(bool c){continuous=c;}void setReplyAudio(bool r){replyAudio=r;}}}
namespace transport=services::agent_transport;
namespace services {namespace agent_speech {bool allowed=false;void setEnabled(bool value){allowed=value;}bool active=false;bool busy(){return active;}}}
void startRecording(){++starts;s_state.stage=Stage::Recording;s_state.continuous=s_state.handsFree;s_state.voiceEntryMs=clock_;}
namespace services {namespace vibration {enum class Owner{Navigation};int pulses=0;bool pulse(Owner,uint32_t){++pulses;return true;}}}
namespace hal {namespace input {
enum class EventType {Scroll,ButtonA_Tap,ButtonB_Tap,ButtonC_Tap,ButtonA_Release,ButtonB_Release};
struct InputEvent {EventType type;int delta=0;};bool a=false,b=false;bool buttonADown(){return a;}bool buttonBDown(){return b;}uint32_t heldMs=0;bool chordDown(){return a&&b;}uint32_t chordHeldMs(){return heldMs;}
}}
namespace render {bool scrollListeningReply(AgentState&,int){return false;}}
void returnToHome(){++homes;s_state.handsFree=s_state.handsFreeReady=false;s_state.stage=Stage::Eyes;s_state.continuous=false;}
void finishRecording(){++ends;}
namespace flow {using ::returnToHome;using ::finishRecording;void leaveAgentMode();bool takeTapAsAgentMode(uint32_t);}
'''
F = 'src/apps/agent/agent_flow.cpp'
for sig in ['void startHandsFree()', 'bool continueHandsFree()', 'void cue(uint8_t productSound)', 'void serviceCue(uint32_t now)',
            'bool beginTurnGesture()', 'bool takeTapAsAgentMode(uint32_t now)', 'bool serviceTurnGesture(uint32_t now)', 'void leaveAgentMode()', 'void serviceAgentSilence(uint32_t now)']:
    code += function(F, sig) + '\n'
code += 'namespace flow { void leaveAgentMode(){ ::leaveAgentMode(); } bool takeTapAsAgentMode(uint32_t n){ return ::takeTapAsAgentMode(n); } }\n'
code += function('src/apps/agent/screen_recording.cpp', 'void onInput(AgentState& s, hal::input::InputEvent ev)') + '\n'
code += function('src/apps/agent/screen_recording.cpp', 'bool onChord(AgentState& s)') + '\n'
code += r'''
void press(bool down){hal::input::a=hal::input::b=down;}
int main(){
 using ET=hal::input::EventType;
 // A+B down: the mic opens at once, felt not heard; the session start is held.
 press(true);assert(beginTurnGesture());
 assert(starts==1&&s_state.recStartedByChord&&s_state.gestureDeciding&&transport::held&&services::vibration::pulses==1&&played.empty());
 // Seen held for 250 ms: the start is released as push-to-talk; both released sends.
 hal::input::heldMs=200;clock_+=200;assert(!serviceTurnGesture(clock_)&&transport::held);
 hal::input::heldMs=260;clock_+=60;serviceTurnGesture(clock_);assert(!transport::held&&!s_state.gestureDeciding&&!transport::continuous);
 hal::input::a=false;onInput(s_state,{ET::ButtonA_Release});assert(ends==0);
 hal::input::b=false;onInput(s_state,{ET::ButtonB_Release});assert(ends==1&&homes==0);
 assert(onChord(s_state)&&homes==0);                     // A+B during push-to-talk is a no-op
 // Released within 250 ms: a tap. The same mic and session switch to agent mode
 // before the start frame goes out — no restart, no Home screen in between.
 s_state=AgentState{};played.clear();starts=0;ends=0;clock_=5000;services::vibration::pulses=0;
 // The UI loop stalled for 600 ms (mic start, first paint): the tick runs late
 // and the release is processed late, but the buttons were seen down for only
 // 120 ms, so it is still a tap.
 press(true);assert(beginTurnGesture()&&starts==1&&transport::held);hal::input::heldMs=120;press(false);clock_+=600;
 assert(!serviceTurnGesture(clock_)&&transport::held);
 onInput(s_state,{ET::ButtonB_Release});
 assert(ends==0&&homes==0&&starts==1&&s_state.stage==Stage::Recording);
 assert(s_state.handsFree&&s_state.continuous&&transport::continuous&&!transport::held&&!transport::replyAudio&&!s_state.recStartedByChord);
 assert(services::vibration::pulses==2&&!services::agent_speech::allowed);
 // Agent mode input: C and releases do nothing; A closes a document first, then leaves.
 onInput(s_state,{ET::ButtonC_Tap});onInput(s_state,{ET::ButtonA_Release});assert(homes==0&&ends==0);
 s_state.documentVisible=true;s_state.documentId[0]='x';onInput(s_state,{ET::ButtonA_Tap});
 assert(closed==1&&!s_state.documentVisible&&homes==0);
 speakerFree=false;onInput(s_state,{ET::ButtonA_Tap});assert(homes==1&&s_state.stage==Stage::Eyes);
 assert(s_state.pendingCue==(uint8_t)Sound::Cancel+1);   // waits for the mic to close
 speakerFree=true;clock_+=10;serviceCue(clock_);assert(!s_state.pendingCue&&played.back()==(int)Sound::Cancel);
 // A+B tapped again in agent mode also leaves.
 s_state.stage=Stage::Recording;s_state.continuous=s_state.handsFree=true;assert(onChord(s_state)&&homes==2);
 // Silence: leaves after 60 s without activity, but not while Kami works or thinks.
 s_state=AgentState{};s_state.stage=Stage::Recording;s_state.continuous=true;s_state.agentActivityMs=clock_;
 serviceAgentSilence(clock_+59999);assert(homes==2);
 s_state.agentWorking=true;serviceAgentSilence(clock_+70000);assert(homes==2);s_state.agentWorking=false;
 s_state.conversationView=AgentState::ConversationView::Thinking;serviceAgentSilence(clock_+70000);assert(homes==2);
 s_state.conversationView=AgentState::ConversationView::Listening;serviceAgentSilence(clock_+60000);assert(homes==3);
 // Cues waiting too long expire instead of playing late.
 speakerFree=false;cue((uint8_t)Sound::Send);assert(s_state.pendingCue);speakerFree=true;
 clock_+=900;serviceCue(clock_);assert(!s_state.pendingCue&&played.back()!=(int)Sound::Send);
 // A gesture only starts from Home, a reply or an error; a held-chord guard blocks it.
 s_state=AgentState{};starts=0;s_state.stage=Stage::Thinking;assert(!beginTurnGesture()&&starts==0);
 s_state.stage=Stage::Reply;s_state.suppressHeldChord=true;assert(beginTurnGesture()&&starts==0);
 // Power button: a short tap is a click, a hold is sleep (unchanged contract).
 core::power_button::Gesture gesture;
 assert(gesture.update(true,100)==core::power_button::Action::None);
 assert(gesture.update(false,200)==core::power_button::Action::Click);
 assert(gesture.update(true,300)==core::power_button::Action::None);
 assert(gesture.update(true,300+core::power_button::kHoldMs)==core::power_button::Action::Sleep);
 // Re-arm after a reply waits for playback and the held-chord guard.
 s_state=AgentState{};s_state.stage=Stage::Reply;starts=0;assert(!continueHandsFree());s_state.handsFree=s_state.handsFreeReady=true;
 services::agent_speech::active=true;assert(!continueHandsFree());services::agent_speech::active=false;
 s_state.suppressHeldChord=true;assert(!continueHandsFree());s_state.suppressHeldChord=false;
 assert(continueHandsFree()&&starts==1&&!s_state.handsFreeReady);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory); (p/'test.cpp').write_text(code)
    subprocess.run(['c++','-I'+str(root/'include'),'-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS A+B gesture: mic opens on press with start held; hold releases push-to-talk, tap converts the same session to agent mode; agent-mode exit by A/A+B/silence; cues wait for the mic; guards')
