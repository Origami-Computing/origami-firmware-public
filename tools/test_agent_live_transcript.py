"""Production transcript validation/callback and quiet repaint regressions."""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
def function(source,name):
    a=source.index('void '+name+'('); b=source.index('{',a)+1; depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]
flow=(ROOT/'src/apps/agent/agent_flow.cpp').read_text()
transport=(ROOT/'src/services/agent_transport.cpp').read_text()
a=transport.index('    const bool conversationState=');b=transport.index('    if (conversationState || inputEvent || documentPreview || endpoint || speech || document || documentPage || transcript ||',a)
validation=transport[a:b].replace('return;', 'return false;')
render=(ROOT/'src/apps/agent/agent_render.cpp').read_text()
code=r'''
#include <ArduinoJson.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdint>
enum class Stage {Reply,Recording};
uint32_t millis(){return 100;}
struct Conv {std::string text;void clear(){} bool pushMessage(const char*){return true;} void refreshDisplay(int){} const char* displayText(){return text.c_str();}};
struct AgentState {bool continuous=false;uint32_t agentActivityMs=0,typingCueMs=0;size_t twRevealLen=0;int msgIndex=0;enum class ConversationView { Listening, User, Thinking, Reply }; ConversationView conversationView=ConversationView::Listening;Conv conv;bool listeningReplyFollowTail=true;bool agentWorking=false,documentPreview=false,documentBeforePreviewVisible=false,documentPagePending=false;char* documentSavedText=nullptr;char documentSavedTitle[64]={},thinkStatus[256]={};int documentOffset=0,documentNextOffset=-1,previousScrollLines=0;uint32_t lastTranscriptMs=0;bool documentReceived=false;char* documentText=nullptr;char documentId[37]={},documentTitle[64]={};bool documentVisible=false,documentTruncated=false;int documentScroll=0;static constexpr size_t kTranscriptBytes=1536;static inline char kNoTranscript[1]={};char* transcript=kNoTranscript;uint32_t transcriptPainted=0,transcriptRevision=0,replyRevision=0,replyPainted=0,twLastMs=0;bool endpointRequested=false;bool transcriptFinal=false;Stage stage=Stage::Reply;} s_state;
namespace core {namespace memory {void* allocPsram(const char*,size_t n){return malloc(n);}void freePsram(void*& p){free(p);p=nullptr;}}}
namespace services {namespace agent_transport {void setDocumentId(const char*){}} namespace agent_speech {void receive(const char*){}} namespace agent_delivery {void bind(const char*,int){}} namespace agent_latency {int milestones=0;void milestone(const char*){++milestones;}}}
namespace emotion {void receive(JsonVariantConst,bool){}}
namespace services {namespace vibration {enum class Owner{Navigation};int pulses=0;bool pulse(Owner,uint32_t){++pulses;return true;}}
 namespace agent_speech {bool on=false;bool enabled(){return on;}bool busy(){return false;}} namespace agent_voice {bool isCapturing(){return false;}}}
namespace sfx {namespace product {enum class Sound{Typing};int typed=0;bool play(Sound){++typed;return true;}}}
namespace cfg {namespace agent {constexpr uint32_t kRevealCharMs=14,kRevealMaxLag=40,kTypingCueMs=56;}}
int paints=0;
void paintConversation(AgentState& s,uint32_t,bool){++paints;s.replyPainted=s.replyRevision;}
''' + function(flow,'transportEvent') + '\n' + render[render.index('static bool typingCueAllowed('):render.index('void serviceTypewriter(')] + function(render,'serviceTypewriter') + r'''
bool valid(JsonDocument& doc,uint32_t replyEpoch) {
    const char* type=doc["type"] | "";
''' + validation + r'''
    return true;
}
int main() {
    JsonDocument d;
    d["type"]="transcript";d["turnId"]=5;d["revision"]=1;d["text"]="hello";d["final"]=false;
    assert(valid(d,5)); assert(!valid(d,6));
    d["turnId"]="5";assert(!valid(d,5));d["turnId"]=5;
    d["final"]="false";assert(!valid(d,5));d["final"]=false;
    d["text"]=std::string(1536,'a');assert(!valid(d,5));
    d["text"]=std::string(1535,'a');assert(valid(d,5));
    std::string payload;serializeJson(d,payload);transportEvent(payload.c_str());
    assert(strlen(s_state.transcript)==1535);
    d["revision"]=2;d["text"]="corrected";d["final"]=true;payload.clear();serializeJson(d,payload);transportEvent(payload.c_str());
    assert(!strcmp(s_state.transcript,"corrected") && s_state.transcriptFinal);
    d["revision"]=1;d["text"]="stale";payload.clear();serializeJson(d,payload);transportEvent(payload.c_str());
    assert(!strcmp(s_state.transcript,"corrected"));
    assert(services::agent_latency::milestones==1);
    d.clear();d["type"]="conversation_state";d["turnId"]=5;d["view"]="thinking";
    assert(valid(d,5));assert(!valid(d,6));
    payload.clear();serializeJson(d,payload);transportEvent(payload.c_str());
    assert(s_state.conversationView==AgentState::ConversationView::Thinking);
    assert(s_state.transcriptPainted==UINT32_MAX);
    d["view"]="unknown";assert(!valid(d,5));
    // Agent mode: Kami taking the turn and listening again are felt (haptic).
    s_state.continuous=true;s_state.conversationView=AgentState::ConversationView::User;
    d.clear();d["type"]="conversation_state";d["turnId"]=5;d["view"]="thinking";payload.clear();serializeJson(d,payload);transportEvent(payload.c_str());
    assert(services::vibration::pulses==1);
    d["view"]="reply";payload.clear();serializeJson(d,payload);transportEvent(payload.c_str());assert(services::vibration::pulses==1);
    d["view"]="listening";payload.clear();serializeJson(d,payload);transportEvent(payload.c_str());assert(services::vibration::pulses==2);
    assert(s_state.agentActivityMs==100);
    s_state.continuous=false;
    // Chasing reveal: the first chunk paints one character at once, then the
    // reveal moves at the natural rate and never trails by more than 40 chars.
    s_state.conv.text="Hello there, this is a reply that keeps streaming in quickly from Kami.";
    const size_t n=s_state.conv.text.size();
    s_state.twLastMs=1000;serviceTypewriter(s_state,1001);
    assert(s_state.twRevealLen==n-39&&paints==1&&sfx::product::typed==1);   // within 40 chars at once
    serviceTypewriter(s_state,1010);assert(s_state.twRevealLen==n-39&&paints==2); // <14 ms: no step; painter dedups
    serviceTypewriter(s_state,1043);assert(s_state.twRevealLen==n-36);          // 42 ms -> 3 chars
    assert(sfx::product::typed==1);                                              // ticks stay >=56 ms apart
    serviceTypewriter(s_state,1060);assert(sfx::product::typed==2);
    for(uint32_t t=1060;t<3000;t+=16) serviceTypewriter(s_state,t);
    assert(s_state.twRevealLen==s_state.conv.text.size());
    s_state.twRevealLen=0;s_state.typingCueMs=0;services::agent_speech::on=true;const int quiet=sfx::product::typed;
    for(uint32_t t=4000;t<4400;t+=16) serviceTypewriter(s_state,t);
    assert(sfx::product::typed==quiet);                                              // Voice mode: no ticks
    services::agent_speech::on=false;s_state.stage=Stage::Recording;const int before=paints;serviceTypewriter(s_state,5000);assert(paints==before);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS production transcript epoch/schema/bounds, revisions, agent-mode turn haptics, chasing reveal with paced typing ticks')
