"""Exercise the production conversation composition and pacing with stubbed painters."""
from pathlib import Path
import subprocess, tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'src/apps/agent/agent_render.cpp').read_text()
a = source.index('namespace view = ui::agent_conversation;')
b = source.index('void paintEyesHomeStatic', a)
code = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
enum class Stage { Eyes, Recording, Thinking, Reply, Error };
uint32_t nowMs=0; uint32_t millis(){return nowMs;}
struct Conversation {
 std::string value; std::vector<std::string> more; int shown=0; char scratch[512]={};
 void refreshDisplay(int i){shown=i;} const char* displayText(){return shown<=0?value.c_str():more[shown-1].c_str();}
 bool anyNonEmpty(){return !value.empty();} int segmentCount(){return value.empty()?0:1+(int)more.size();}
 char* wrapScratch(){return scratch;} size_t wrapScratchSize(){return sizeof(scratch);}
};
struct TFT_eSprite;
struct AgentState {
 enum class ConversationView { Listening, User, Thinking, Reply }; ConversationView conversationView=ConversationView::Listening;
 Stage stage=Stage::Recording; Conversation conv;
 bool continuous=false,handsFree=false,transcriptFinal=false,followReply=false,listeningReplyFollowTail=false,miniEyesOpen=true;
 bool documentVisible=false,documentTruncated=false,documentPreview=false,documentPagePending=false,spritesReady=false;
 const char* documentText=nullptr; const char* documentTitle="Note"; int documentScroll=0,documentNextOffset=-1,documentOffset=0;
 char transcript[200]={}; int msgIndex=0,scrollLines=0,previousScrollLines=0;
 uint32_t transcriptRevision=0,transcriptPainted=0,replyRevision=0,replyPainted=0;
 size_t twRevealLen=0;
 int agentTextX,agentTextY,agentTextW,agentTextH,agentMaxLines,agentLineH;
 void* sprMiniEye=nullptr; void* sprDialogue=nullptr; int sprDialogueW=0,sprDialogueH=0;
 uint32_t miniEmotionRevision=0; int miniEmotionSegment=0;
 enum class RecDot{Off,Grey,RedBlink}; RecDot recDotMode=RecDot::Off; uint8_t thinkDotPhase=0;
 bool viewInit=false; uint8_t viewMode=0; float viewFromT=0,levelSmooth=0;
 uint32_t viewSinceMs=0,viewPaintMs=0,viewSignature=0,viewContentSig=0;uint8_t viewLayer=0;bool eyesUp=false;
};
namespace core{namespace memory{void freeSprite(void*&p){p=nullptr;}}}
namespace motion{void cancel(){}}
void serviceMicCaptureEarly(){}
namespace hal{namespace display{int dirtyH=0;void markDirty(int,int h){dirtyH=h;}void setFlushService(void(*)()){}}}
namespace services{namespace agent_latency{int painted_=0;void painted(int,int){++painted_;}} namespace agent_transport{int requested=-1;void requestDocumentWindow(int n){requested=n;}}}
namespace flow{uint8_t level=0;uint8_t micLevel(){return level;}}
namespace ui{namespace eye_rig{struct Pose{int id=0;};struct Preset{Pose pose;};const Preset kPresets[1]={};}}
namespace emotion{struct Selection{ui::eye_rig::Pose pose;uint32_t revision=0;};Selection snapshot(int){return {};}}
struct Canvas{}; Canvas canvas; Canvas& T(){return canvas;}
constexpr uint16_t kUiBg=0xffff;
namespace ui{namespace home_eyes{bool loaded=false;int conv=0,think=0,travel=0,invalid=0,incremental=0;float lastTravel=-1;
 bool isLoaded(){return loaded;}float sharedBlink(uint32_t){return 1;}void invalidatePaint(){++invalid;}
 bool drawThinkingEyes(Canvas&,float,uint32_t,bool only){++think;incremental+=only;return true;}
 bool drawConversationEyes(Canvas&,float,float,uint32_t,bool only){++conv;incremental+=only;return true;}
 bool drawTravelEyes(Canvas&,float a,float,float){++travel;lastTravel=a;return true;}}
 namespace eye_rig{template<class T> void drawRaster(T&,int,int,int,int,const Pose&,float,uint16_t,float,float,float,bool,int,int,int,float,float){}}}

// Recording painter: what the last frame showed.
struct Frame{int paints=0;float t=-1,offX=0,offY=0,open=1;std::string line,hint,reply,docFooter,answer;int answerPx=0,page=-1,pages=0;bool sub=false;bool interim=false,dim=false,dot=false,document=false;int scroll=-1,rows=0;} f;
namespace ui{namespace agent_conversation{
 constexpr int x=24,width=232,replyY=84,pitch=24,replyRows=5;
 struct EyeFrame{float t=0,open=1,offsetX=0,offsetY=0;};
 int count(const char* s){if(!*s)return 0;return std::count(s,s+strlen(s),'\n')+1;}
 void clear(Canvas&){f=Frame{f.paints+1};}
 void eyes(Canvas&,const eye_rig::Pose&,const EyeFrame& e){f.t=e.t;f.offX=e.offsetX;f.offY=e.offsetY;f.open=e.open;}
 void reply(Canvas&,const char* t,int scroll,int rows){f.reply=t;f.scroll=scroll;f.rows=rows;}
 void line(Canvas&,const char* t,bool interim,bool dim){f.line=t;f.interim=interim;f.dim=dim;}
 void hint(Canvas&,const char* t){f.hint=t;}
 constexpr int lineBaseline=216;
 int answerPx(const char* t){return (t&&*t&&!strchr(t,'\n')&&strlen(t)<=24)?24:0;}
 void answer(Canvas&,const char* t,int px){f.answer=t;f.answerPx=px;}
 void liveDot(Canvas&,int=0){f.dot=true;}
 constexpr int subBaseline=236;
 void subLine(Canvas&,const char* t){f.line=t?t:"";f.sub=true;}
 void pageMarker(Canvas&,int p,int n){f.page=p;f.pages=n;}
 void documentEyes(Canvas&,const eye_rig::Pose&,float){f=Frame{f.paints+1};f.document=true;}
 void document(Canvas&,const char*,const char*,int n,bool,const char* footer=nullptr,bool=false){f.scroll=n;f.docFooter=footer?footer:"";}
}}
''' + source[a:b] + r'''
int main(){
 AgentState s;
 // Push-to-talk listening: hint until words arrive, eyes at listening size.
 nowMs=1000; beginRecordingView(s,nowMs);
 assert(f.paints==1&&f.t==0&&f.hint=="Listening..."&&f.line.empty()&&!f.dot);
 // Unchanged state does not repaint; a new transcript within 33 ms waits.
 nowMs=1005; paintRecordingBorder(s,nowMs,false); assert(f.paints==1);
 strcpy(s.transcript,"hello"); ++s.transcriptRevision; nowMs=1010; paintRecordingBorder(s,nowMs,false); assert(f.paints==1);
 nowMs=1080; paintRecordingBorder(s,nowMs,false); assert(f.paints==2&&f.line=="hello"&&f.interim&&!f.dim&&f.hint.empty());
 s.transcriptFinal=true;++s.transcriptRevision; nowMs=1120; paintRecordingBorder(s,nowMs,false); assert(!f.interim);
 // Mic level bobs the eyes; quantized so tiny changes do not repaint.
 flow::level=160; nowMs=1160; paintRecordingBorder(s,nowMs,false); assert(f.offY<0);   // the mic level lifts the eyes
 // Thinking: your line dims; eyes glance up and right over 300 ms.
 s.stage=Stage::Thinking; nowMs=1200; paintThinkingShell(s,0); assert(f.line=="hello"&&f.dim&&f.offX==0);
 nowMs=1500; paintThinkingShell(s,0); assert(f.offX>5.9f&&f.offY<-5.9f&&f.t==0);
 // Reply: eyes ease to reply size over 380 ms; only the revealed prefix shows.
 s.stage=Stage::Reply; s.conv.value="first line\nsecond\nthird\nfourth\nfifth\nsixth"; s.twRevealLen=5; ++s.replyRevision;
 nowMs=1600; paintReplyText(s); assert(f.t<0.05f&&f.reply=="first"&&f.rows==5&&f.line.empty());
 assert(services::agent_latency::painted_>0);
 nowMs=1990; paintConversation(s,nowMs,false); assert(f.t==1.f);
 s.twRevealLen=s.conv.value.size(); nowMs=2030; paintConversation(s,nowMs,false); assert(f.reply==s.conv.value&&f.scroll==0);
 // Opening lines stay put unless following was chosen by scrolling to the end.
 s.followReply=true; ++s.replyRevision; nowMs=2070; paintConversation(s,nowMs,false); assert(f.scroll==1);
 // Agent mode: reply above, "Listening..." and the live marker below, four rows.
 AgentState c; c.stage=Stage::Recording; c.continuous=c.handsFree=true; nowMs=3000; beginRecordingView(c,nowMs);
 assert(f.dot&&f.hint=="Listening...");
 c.conversationView=AgentState::ConversationView::Reply; c.conv.value="a\nb\nc\nd\ne\nf"; ++c.replyRevision;
 nowMs=3040; paintRecordingBorder(c,nowMs,false); assert(f.reply==c.conv.value&&f.rows==4&&f.hint=="Listening..."&&f.dot&&f.scroll==0);
 assert(!scrollListeningReply(c,-1)); assert(scrollListeningReply(c,99)&&c.listeningReplyFollowTail&&c.previousScrollLines==2);
 c.conv.value+="\ng"; ++c.replyRevision; nowMs=3080; paintRecordingBorder(c,nowMs,false); assert(f.scroll==3);
 assert(scrollListeningReply(c,-1)&&!c.listeningReplyFollowTail);
 // A stray noise transcript or a rejected turn never hides the answer: your
 // words show in the bottom line and the answer stays until a turn starts.
 c.conversationView=AgentState::ConversationView::User; strcpy(c.transcript,"thanks"); ++c.transcriptRevision;
 nowMs=3500; paintRecordingBorder(c,nowMs,false); assert(f.line=="thanks"&&f.reply==c.conv.value&&f.t==1.f);
 c.conversationView=AgentState::ConversationView::Listening; nowMs=3540; paintRecordingBorder(c,nowMs,false);
 assert(f.reply==c.conv.value&&f.hint=="Listening..."&&f.line.empty());
 c.conversationView=AgentState::ConversationView::Thinking; nowMs=3580; paintRecordingBorder(c,nowMs,false);
 assert(f.dim&&f.line=="thanks"&&f.dot&&f.reply.empty());           // real words + Kami working: thinking view
 c.transcript[0]=0;++c.transcriptRevision;nowMs=3620; paintRecordingBorder(c,nowMs,false); assert(f.reply==c.conv.value); // no words: keep answer
 // The server clears the answer when the next turn begins; the eyes stay up
 // for the rest of the conversation instead of jumping back down.
 c.conv.value.clear();++c.replyRevision;c.conversationView=AgentState::ConversationView::User;strcpy(c.transcript,"next one");++c.transcriptRevision;
 nowMs=3700; paintRecordingBorder(c,nowMs,false); assert(f.line=="next one"&&f.reply.empty());
 nowMs=4100; paintRecordingBorder(c,nowMs,false); assert(f.t==1.f&&c.eyesUp);
 // With the eyes up, a one-line answer is ordinary reply text under them.
 c.conversationView=AgentState::ConversationView::Reply;c.conv.value="Sure, done.";++c.replyRevision;c.transcript[0]=0;++c.transcriptRevision;
 nowMs=4150; paintRecordingBorder(c,nowMs,false); assert(f.reply=="Sure, done."&&f.answer.empty()&&f.hint=="Listening..."&&f.t==1.f);
 // A fresh conversation (after Home) starts with the eyes big again.
 { AgentState d; d.stage=Stage::Recording; d.continuous=true; d.conversationView=AgentState::ConversationView::Reply; d.conv.value="Sure, done."; ++d.replyRevision;
   nowMs=4160; beginRecordingView(d,nowMs); assert(f.answer=="Sure, done."&&f.sub&&f.line=="Listening..."&&f.t==0.f&&!d.eyesUp); }
 c.conv.value="a\nb\nc\nd";
 // Documents keep their sheet with a live footer in agent mode.
 c.documentVisible=true; c.documentText="a\nb\nc\nd"; c.documentNextOffset=4095; c.conversationView=AgentState::ConversationView::Reply; ++c.replyRevision;
 nowMs=3900; paintRecordingBorder(c,nowMs,false); assert(f.document&&f.docFooter.find("Kami:")==0);
 assert(scrollListeningReply(c,1)&&scrollListeningReply(c,1)); assert(scrollListeningReply(c,1)&&services::agent_transport::requested==4095);
 c.documentVisible=false; ++c.replyRevision; nowMs=3950; paintRecordingBorder(c,nowMs,false); assert(!f.document);
 computeDialogueLayout(s); assert(s.agentMaxLines==5&&s.agentTextY==84);
 // With the Home renderer loaded every eye view is the Home eyes: listening and
 // thinking use it in place, size changes use its cached travel frames.
 {
  namespace he=ui::home_eyes; he::loaded=true;
  AgentState h; h.stage=Stage::Recording; nowMs=20000; beginRecordingView(h,nowMs);
  assert(he::conv==1&&he::invalid==1&&f.hint=="Listening...");
  const int paintsBefore=f.paints;
  nowMs=20040; paintRecordingBorder(h,nowMs,false);
  assert(he::conv==2&&he::incremental==1&&f.paints==paintsBefore);     // eyes only; text untouched
  h.stage=Stage::Thinking; nowMs=20100; paintThinkingShell(h,0); assert(he::think==1&&he::invalid==2);
  h.stage=Stage::Reply; h.conv.value="a\nb\nc\nd\ne\nf\ng"; h.twRevealLen=h.conv.value.size(); ++h.replyRevision;
  nowMs=20200; paintReplyText(h); assert(he::travel==1&&he::lastTravel<0.05f);   // first frame already travels
  nowMs=20400; paintConversation(h,nowMs,false); assert(he::travel==2&&he::lastTravel>0.3f&&he::lastTravel<0.75f);
  const int travelled=he::travel; nowMs=20700; paintConversation(h,nowMs,false);
  assert(he::travel==travelled&&f.reply==h.conv.value);                  // resting small eyes
  he::loaded=false;
 }
 // A one-line answer keeps the big eyes and shows large; it moves to the
 // paragraph view (eyes shrinking) once more text makes it outgrow the line.
 AgentState a; a.stage=Stage::Reply; a.conv.value="It's 72 and sunny."; a.twRevealLen=a.conv.value.size(); ++a.replyRevision;
 nowMs=5000; paintReplyText(a); assert(f.answer=="It's 72 and sunny."&&f.answerPx==24&&f.reply.empty()&&f.t==0.f);
 a.conv.value+=" Tomorrow looks a bit cooler with rain later."; a.twRevealLen=a.conv.value.size(); ++a.replyRevision;
 nowMs=5040; paintConversation(a,nowMs,false); assert(f.answer.empty()&&f.reply==a.conv.value&&f.t<0.2f);
 nowMs=5450; paintConversation(a,nowMs,false); assert(f.t==1.f);
 // Several messages: one per screen with its position; each can stay big.
 AgentState m; m.stage=Stage::Reply; m.conv.value="First, the weather is mild today and tomorrow it will rain hard."; m.conv.more={"Want a reminder?","Third."};
 m.twRevealLen=100; m.msgIndex=0; ++m.replyRevision; nowMs=6000; paintReplyText(m);
 assert(f.pages==3&&f.page==0&&!f.reply.empty()&&f.answer.empty());
 m.msgIndex=1; m.twRevealLen=100; ++m.replyRevision; nowMs=6040; paintReplyText(m);
 // The first message raised the eyes, so this short one stays as reply text.
 assert(f.pages==3&&f.page==1&&f.reply=="Want a reminder?"&&f.answer.empty()&&m.eyesUp);
 // A short message first, in a fresh conversation, stays big.
 { AgentState q; q.stage=Stage::Reply; q.conv.value="Done."; q.conv.more={"Anything else?"}; q.twRevealLen=10; ++q.replyRevision;
   nowMs=6050; paintReplyText(q); assert(f.answer=="Done."&&f.pages==2&&!q.eyesUp); }
 // Agent mode shows the newest message and its position.
 AgentState n; n.stage=Stage::Recording; n.continuous=true; n.conversationView=AgentState::ConversationView::Reply;
 n.conv.value="Okay."; n.conv.more={"Anything else?"}; ++n.replyRevision; nowMs=6100; beginRecordingView(n,nowMs);
 assert(f.answer=="Anything else?"&&f.page==1&&f.pages==2);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory); (p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS conversation composition: hint/line/dim/reply per state, eye size tween and glance, mic bob, repaint pacing, agent-mode scroll and takeover, documents')
