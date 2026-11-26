#pragma once
// Host routing adapter; geometry, motion and rasterization are shared with firmware.
#include "ui/proportional_text.h"
#include "ui/menu_list.h"
#include "menu_cleanup_generated.h"
#ifndef MENU_BROWSER
#include <sstream>
#endif
#include "assets/battery_icon.h"
#include "assets/wifi_icons.h"
#include "ui/battery_indicator.h"
namespace menu_cleanup {
using namespace ui::menu_list;
struct Model {
 bool topBar=false;int menu=0,selection=0,first=0;bool leaf=false,home=false;const char* label="";
 struct Parent{int menu,selection,first;};Parent stack[8];int depth=0;Motion motion;
 void input(char kind,int value,unsigned now){
  if(kind=='r'&&!leaf&&!home){int next=std::max(0,std::min(menu_cleanup_data::counts[menu]-1,selection+value));if(next==selection)return;selection=next;first=std::max(0,std::min(first,selection));if(selection>=first+4)first=selection-3;motion.move(selection,first,now);}
  if(kind=='c'&&!leaf&&!home){const auto& e=menu_cleanup_data::pages[menu][selection];int child=!strcmp(e.label,"Settings")?4:e.child;if(child>=0&&depth<8){stack[depth++]={menu,selection,first};menu=child;selection=first=0;motion.reset(0,0,now);}else{leaf=true;label=e.label;}}
  if(kind=='a'){if(leaf)leaf=false;else if(depth){auto p=stack[--depth];menu=p.menu;selection=p.selection;first=p.first;motion.reset(selection,first,now);}else home=true;}
  if(kind=='r'&&home){home=false;motion.reset(selection,first,now);}
 }
};
// Representative idle status values; native firmware icon masks, no live services.
template<class C> void statusBar(C& c){
 for(int y=0;y<22;++y)for(int x=0;x<22;++x){unsigned v=assets::wifi_icons::kStrong[y*22+x];unsigned a=(v&15)*17;if(a)c.fillRect(24+x,10+y,1,1,ui::rounded_fill::blend((v&16)?0x9492:0,0xffff,a));}
 for(int y=0;y<26;++y)for(int x=0;x<42;++x){int bit=y*42+x;unsigned a=((assets::battery_icon::kAlpha4[bit/2]>>(bit%2?0:4))&15)*17;if(a)c.fillRect(214+x,8+y,1,1,ui::rounded_fill::blend(0,0xffff,a));}
 c.fillRect(219,13,ui::battery::fillWidth(81),16,ui::battery::color(81));
 ui::proportional::text(c,"81",225,14,12,0,ui::battery::color(81));
 ui::proportional::centered(c,"13:15",12,20);
}
template<class C> void render(C& c,const Model& model,unsigned now){
 if(model.home||model.leaf){c.fillRect(0,0,280,240,0xffff);ui::proportional::centered(c,model.home?"Home":model.label,84,24);ui::proportional::centered(c,model.home?"Turn C to open menu":"A / A+B to return",162,16);return;}
 auto item=[&](int i){const auto& e=menu_cleanup_data::pages[model.menu][i];ui::ListItem row;row.label=e.label;row.icon=e.icon;row.iconSize=e.size;if(i==model.selection&&e.motion){row.icon=e.motion+((now-model.motion.at)/75%48)*392;row.iconSize=28;row.iconColor=e.color;}row.opensScreen=e.opens;row.fg=0;return row;};
 auto pose=model.motion.sample(now);ui::menu_list::Layout layout;
 if(model.topBar){layout.top=42;layout.pitch=48;layout.height=42;layout.bottom=234;
  pose={42+(pose.top-16)*48/56,42+(pose.bottom-66)*48/56+42,pose.scroll*48/56};
 }
 ui::menu_list::paint(c,pose,menu_cleanup_data::counts[model.menu],model.selection,item,now-model.motion.at,layout);
 if(model.topBar)statusBar(c);

}
#ifndef MENU_BROWSER
template<class C> void run(C& c,const char* events,unsigned now,int menu=0,int selection=0,bool metadata=false,unsigned speed=360,bool topBar=false){
 Model m;m.motion.durationMs=speed;m.topBar=topBar;m.menu=std::max(0,std::min(9,menu));m.selection=std::max(0,std::min(menu_cleanup_data::counts[m.menu]-1,selection));m.first=std::max(0,m.selection-3);m.motion.reset(m.selection,m.first,0);
 std::stringstream stream(events);std::string event;while(std::getline(stream,event,';')){unsigned at=0;char kind=0;int value=0;sscanf(event.c_str(),"%u,%c,%d",&at,&kind,&value);m.input(kind,value,at);}
 if(metadata){auto p=m.motion.sample(now);printf("{\"menu\":%d,\"selection\":%d,\"first\":%d,\"leaf\":%s,\"home\":%s,\"depth\":%d,\"top\":%.3f,\"bottom\":%.3f,\"scroll\":%.3f,\"pixels\":[",m.menu,m.selection,m.first,m.leaf?"true":"false",m.home?"true":"false",m.depth,p.top,p.bottom,p.scroll);}
 render(c,m,now);
 if(metadata)printf("]}");
}
#endif
}
