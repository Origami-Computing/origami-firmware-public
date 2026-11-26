"""Exercise the production conversation painter on an instrumented 280x240 surface."""
from pathlib import Path
import subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include "ui/agent_conversation.h"
struct Surface {
  uint16_t pixels[280*240]; bool outside=false; bool swap=false;
  Surface(){ for(auto& p:pixels) p=0x1234; }
  void put(int x,int y,uint16_t c){ if(x<0||y<0||x>=280||y>=240){outside=true;return;} pixels[y*280+x]=c; }
  void fillRect(int x,int y,int w,int h,uint16_t c){ for(int j=y;j<y+h;++j)for(int i=x;i<x+w;++i)put(i,j,c); }
  void drawRect(int x,int y,int w,int h,uint16_t c){ fillRect(x,y,w,1,c);fillRect(x,y+h-1,w,1,c);fillRect(x,y,1,h,c);fillRect(x+w-1,y,1,h,c); }
  void fillCircle(int cx,int cy,int r,uint16_t c){ for(int j=-r;j<=r;++j)for(int i=-r;i<=r;++i) if(i*i+j*j<=r*r) put(cx+i,cy+j,c); }
  void pushImage(int x,int y,int w,int h,const uint16_t* d){ for(int j=0;j<h;++j)for(int i=0;i<w;++i){uint16_t v=d[j*w+i]; put(x+i,y+j,(uint16_t)((v>>8)|(v<<8)));} }
  void drawPixel(int x,int y,uint16_t c){ put(x,y,c); }
  uint16_t readPixel(int x,int y) const { return (x<0||y<0||x>=280||y>=240)?0xffff:pixels[y*280+x]; }
  bool getSwapBytes() const { return swap; } void setSwapBytes(bool v){ swap=v; }
  int width() const { return 280; } int height() const { return 240; }
  uint16_t at(int x,int y) const { return pixels[y*280+x]; }
  int inkIn(int x0,int y0,int x1,int y1) const { int n=0; for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x) if(at(x,y)!=0xffff) ++n; return n; }
  void rowsInk(int& top,int& bottom) const { top=240;bottom=-1; for(int y=0;y<240;++y)for(int x=0;x<280;++x) if(at(x,y)!=0xffff){ if(y<top)top=y; bottom=y; } }
};
namespace v=ui::agent_conversation;
int main(){
  const auto pose=ui::eye_rig::kPresets[0].pose;
  // Listening eyes are large and centred above the line; reply eyes are small, near the top.
  { Surface s; v::clear(s); v::EyeFrame e; v::eyes(s,pose,e); assert(!s.outside);
    int top,bottom; s.rowsInk(top,bottom); assert(top>=30 && bottom<=160 && bottom-top>=70);
    Surface r; v::clear(r); e.t=1; v::eyes(r,pose,e); int rt,rb; r.rowsInk(rt,rb); assert(rt>=10 && rb<=70 && rb-rt<=45); }
  // Your words: centred when short, nothing near the left edge.
  { Surface s; v::clear(s); v::line(s,"what's on my calendar",false,false); assert(!s.outside);
    int left=280,right=0; for(int y=180;y<230;++y)for(int x=0;x<280;++x) if(s.at(x,y)!=0xffff){left=std::min(left,x);right=std::max(right,x);}
    assert(left>24 && right<256 && std::abs((left+right)/2-140)<=3); assert(s.inkIn(0,0,280,180)==0); }
  // Long words: anchored right, faded to nothing at the left edge, still one line.
  { Surface s; v::clear(s); v::line(s,"what's on my calendar tomorrow and can you move the dentist to Friday please",false,false);
    assert(!s.outside); assert(s.inkIn(0,180,v::fadeFrom+1,240)==0); assert(s.inkIn(200,180,257,240)>0);
    assert(s.inkIn(257,180,280,240)==0); assert(s.inkIn(0,0,280,185)==0); }
  // Interim: the last word is grey; dim: everything grey.
  { Surface s; v::clear(s); v::line(s,"hello there",true,false);
    bool black=false,grey=false; for(int y=190;y<230;++y)for(int x=0;x<280;++x){ uint16_t p=s.at(x,y); if(p==0x0000)black=true; if(p==v::kInterim)grey=true; }
    assert(black&&grey);
    Surface d; v::clear(d); v::line(d,"hello there",false,true); bool anyBlack=false; for(int i=0;i<280*240;++i) if(d.pixels[i]==0x0000) anyBlack=true; assert(!anyBlack); }
  // Hints and the agent-mode marker stay in the line band.
  { Surface s; v::clear(s); v::hint(s,"Listening..."); v::liveDot(s); assert(!s.outside); assert(s.inkIn(0,0,280,180)==0); assert(s.at(14,v::lineBaseline-6)==v::kLive); }
  // Reply: measured wrapping, five rows, scrollbar only when it overflows, no overflow outside.
  for(auto text:{"", "short", "A deliberately long sentence with measured proportional words and lines that keeps going for a while so it needs more than five rows of text on this little screen, really."}) {
    const int lines=v::count(text); assert(*v::skip(text,lines)==0);
    for(int scroll=0;scroll<=lines;++scroll){ Surface s; v::clear(s); v::reply(s,text,scroll,v::replyRows); assert(!s.outside);
      assert(s.inkIn(0,0,280,v::replyY-8)==0); assert(s.inkIn(0,v::replyY+v::replyRows*v::pitch,280,240)==0);
      assert((s.inkIn(266,0,269,240)>0)==(lines>v::replyRows)); }
  }
  // The message marker sits in the top-right corner and only for several messages.
  { Surface s; v::clear(s); v::pageMarker(s,0,1); assert(s.inkIn(0,0,280,240)==0);
    v::pageMarker(s,1,12); assert(!s.outside&&s.inkIn(200,4,258,28)>0&&s.inkIn(0,30,280,240)==0&&s.inkIn(258,0,280,240)==0); }
  // Documents keep their sheet and stay in bounds.
  { Surface s; v::clear(s); v::documentEyes(s,pose); v::document(s,"A long note title that should be clipped safely","One\nTwo\nThree\nFour\nFive",0,false,"Kami: hello"); assert(!s.outside); }
}
'''
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory); (p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS conversation painter: eye sizes, centred and faded one-line words, interim/dim ink, reply rows and scrollbar, bounds')
