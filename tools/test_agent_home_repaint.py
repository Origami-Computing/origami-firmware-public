"""Exercise the production home-entry function with a dirty-row display fake."""
import pathlib,subprocess,sys
root=pathlib.Path(__file__).resolve().parents[1]
source=(root/'src/apps/agent/agent_render.cpp').read_text()
function=source[source.index('void paintEyesHomeStatic('):source.index('void serviceEyesShellTick(')]
out=root/'.cache/agent-repaint-test';out.mkdir(parents=True,exist_ok=True)
prefix=r'''
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstdio>
constexpr int kSystemStatusBarH=38,HOME_SCREEN_NUM_FRAMES=3;
constexpr uint16_t kUiBg=0xffff,TFT_WHITE=0xffff;
uint32_t micros(){return 0;}
struct {template<class... T>void printf(const char*,T...) {}} Serial;
struct AgentState{float eyeFloatX,eyeFloatY;};
bool session=true;bool dirty[240]={};std::vector<uint16_t> canvas(280*240),panel(280*240);
struct Display{void fillRect(int x,int y,int w,int h,uint16_t ink){for(int row=y;row<y+h;++row)for(int col=x;col<x+w;++col)canvas[row*280+col]=ink;}} display;
Display& T(){return display;}int scrW(){return 280;}int scrH(){return 240;}
void releaseChatSprites(AgentState&){}void publishEyesRuntimeScene(AgentState&){}
namespace hal {namespace display {
void releaseSpiForSd(){}
void clearContentArea(uint16_t ink){T().fillRect(0,38,280,202,ink);for(int y=38;y<240;++y)dirty[y]=true;}
}}
namespace ui {namespace home_eyes {
void setEyeOffset(float,float){}bool isLoaded(){return true;}bool isProcedural(){return false;}
void endSession(){}bool beginSession(Display&){return session;}
void setEmotion(const char*,bool){}int numFrames(){return 3;}
void upgradeToFullSession(Display&){}void resetProceduralPaintState(int){}void resetShellBlink(){}
void drawFrame(Display&,int,int){T().fillRect(45,68,190,116,0x18c3);for(int y=68;y<184;++y)dirty[y]=true;}
}}
'''
suffix=r'''
int main(){
 for(bool ready:{true,false})for(uint16_t old:{uint16_t(0x1234),uint16_t(0xf945)}){
  session=ready;std::fill(canvas.begin(),canvas.end(),old);panel=canvas;std::fill(dirty,dirty+240,false);
  AgentState s{};paintEyesHomeStatic(s);
  for(int y=0;y<240;++y)if(dirty[y])std::copy(canvas.begin()+y*280,canvas.begin()+(y+1)*280,panel.begin()+y*280);
  for(int y=0;y<240;++y)for(int x=0;x<280;++x){
   uint16_t expected=y<38?old:ready&&y>=68&&y<184&&x>=45&&x<235?0x18c3:0xffff;
   if(panel[y*280+x]!=expected)return 3;
  }
 }
 puts("PASS home entry flushes every prior-scene pixel, preserves chrome, and clears even if eyes cannot load");return 0;
}
'''
compiler_name=sys.argv[1] if len(sys.argv)>1 else 'c++'
compiler=[compiler_name]+(['c++'] if pathlib.Path(compiler_name).stem=='zig' else [])
for mutation in [False,True]:
    body=function if not mutation else function.replace('hal::display::clearContentArea(kUiBg);','T().fillRect(0,38,280,202,kUiBg);')
    cpp=out/('fault.cpp' if mutation else 'test.cpp');exe=cpp.with_suffix('.exe');cpp.write_text(prefix+body+suffix)
    subprocess.run(compiler+['-std=c++11',str(cpp),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)])
    assert result.returncode==(3 if mutation else 0)
print('PASS regression test detects missing display invalidation')
