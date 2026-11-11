"""Exercise the actual alarm builder with Instrument font metrics and a scene sink."""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def function(source,signature):
    a=source.index(signature);b=source.index('{',a)+1;depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]
header=(ROOT/'include/ui/scene.h').read_text()
decl=header[header.index('class Builder {'):header.index('\n};',header.index('class Builder {'))+3]
source=(ROOT/'src/ui/scene.cpp').read_text()
code=r'''
#include "ui/layout_metrics.h"
#include "ui/clock_layout.h"
#include "ui/toggle_row_layout.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
constexpr uint16_t kUiBg=0xffff,kUiFg=0,kUiMuted=0x7777,kUiSelBg=0,kUiSelFg=0xffff,kUiEdit=0x8888;
constexpr int kSystemStatusBarH=38;
namespace ui {
struct Rect {int16_t x,y,w,h;};
namespace scene {
enum class TextAlign {TopLeft,MiddleCenter};
struct Node {std::string id;Rect rect;bool label=false;char text[128]{};int textSize=0,fgColor=0,bgColor=0,z=0;};
struct Scene {
 std::deque<Node> nodes;
 void clear(){nodes.clear();}
 Node* find(const char* id){for(auto& n:nodes)if(n.id==id)return &n;return nullptr;}
 Node* addRect(const char* id,Rect r,unsigned bg,int radius=0,int z=0){
  nodes.emplace_back();auto& n=nodes.back();n.id=id;n.rect=r;n.bgColor=bg;n.z=z;return &n;
 }
 Node* addLabel(const char* id,Rect r,const char* text,unsigned fg,unsigned bg,int size,TextAlign align=TextAlign::TopLeft,int z=0){
  auto* n=addRect(id,r,bg,0,z);n->label=true;n->fgColor=fg;n->textSize=size;snprintf(n->text,sizeof(n->text),"%s",text?text:"");return n;
 }
};
void applySourceLayout(Scene&,const char*,const char*){}
'''+decl+'\n'
for signature in ['Builder::Builder(','void Builder::clearContent(','void Builder::spacer(','void Builder::clockDisplay(','void Builder::settingRow(','void Builder::onOffRow(']:
    code+=function(source,signature)+'\n'
code+=r'''
}}
enum class EditField {Hours,Minutes,Sound,Vibration,Enabled,Save};
EditField s_editField=EditField::Hours;bool s_adjustingField=false;
struct {int hour=0,minute=0,sound=0,vibration=0;bool enabled=true;}s_editAlarm;
const char* s_saveError=nullptr;bool fieldHighlighted(EditField f){return s_editField==f;}
const char* sound="Daybreak";const char* vibration="Classic";
namespace sfx {namespace alarms {struct Preset{const char* name;};Preset preset(uint8_t){return {sound};}}}
namespace core {namespace alert_vibration {const char* name(int){return vibration;}}}
'''+function((ROOT/'src/apps/alarm_clock_app.cpp').read_text(),'static void buildAlarmEditScene(')+r'''
void check(){
 ui::scene::Scene s;buildAlarmEditScene(s,280,240);assert(!s.find("alarm.title")&&!s.find("alarm.vibration.test"));
 for(auto& n:s.nodes)if(n.label){
  assert(n.textSize>=2);
  const auto fitted=ui::safe_area::fitBox({n.rect.x,n.rect.y,n.rect.w,n.rect.h});
  if(ui::default_font::width(n.text,n.textSize)>fitted.w || ui::default_font::height(n.textSize)>fitted.h){
   fprintf(stderr,"Clipped %s text=%s size=%d rect=%d,%d,%d,%d\n",n.id.c_str(),n.text,n.textSize,fitted.x,fitted.y,fitted.w,fitted.h);abort();
  }
  assert(n.rect.y>=38&&n.rect.y+n.rect.h<=228);
 }
 assert(s.find("alarm.save.label")->rect.y+s.find("alarm.save.label")->rect.h<=228);
 const auto colon=s.find("alarm.clock.colon")->rect;
 const auto hourBg=s.find("alarm.clock.hour.bg")->rect;
 const auto minuteBg=s.find("alarm.clock.minute.bg")->rect;
 assert(hourBg.x+hourBg.w<=colon.x && colon.x+colon.w<=minuteBg.x);
 for(const char* id:{"alarm.clock.hour","alarm.clock.minute"}){
  auto* n=s.find(id);assert(strlen(n->text)==2);
 }
}
int main(){
 for(int hour=0;hour<24;++hour)for(int minute=0;minute<60;++minute){s_editAlarm.hour=hour;s_editAlarm.minute=minute;check();}
 for(const char* name:{"Daybreak","Sunrise","Morning Steps","Gentle Bells"}){sound=name;check();}
 for(const char* name:{"Classic","Soft reminder","Double tap","Long wave","Breathing","Three steps","Heartbeat","Patient nudge","Long-short","Dawn pulses","Custom 1","Off"}){
  vibration=name;for(int f=0;f<6;++f){s_editField=EditField(f);for(bool editing:{false,true}){s_adjustingField=editing;check();}}
 }
 s_saveError="Couldn't save. Try again.";check();
 puts("PASS actual alarm builder: all 1440 times, all preset labels, every field/edit state and save failure, standard font metrics, no clipping/scrolling");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-DORIGAMI_INSTRUMENT_FONT','-I'+str(ROOT/'include'),'-I'+str(ROOT/'lib/TB_TFT_eSPI'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
