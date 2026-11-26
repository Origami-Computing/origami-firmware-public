// The native menu model/painter, compiled for browser-local animation.
#define MENU_BROWSER 1
#include "menu_cleanup.h"
namespace {
menu_cleanup::Model model;
struct Surface {
 unsigned char rgba[280*240*4];
 void fillRect(int x,int y,int w,int h,uint16_t color){
  for(int yy=std::max(0,y);yy<std::min(240,y+h);++yy)
   for(int xx=std::max(0,x);xx<std::min(280,x+w);++xx){
    auto p=rgba+4*(yy*280+xx);p[0]=(color>>11)*255/31;
    p[1]=((color>>5)&63)*255/63;p[2]=(color&31)*255/31;p[3]=255;
   }
 }
} surface;
}
extern "C" {
void menu_init(int menu,int selection,unsigned duration,int topbar){
 model=menu_cleanup::Model{};
 model.menu=std::max(0,std::min(9,menu));
 model.selection=std::max(0,std::min(menu_cleanup_data::counts[model.menu]-1,selection));
 model.first=std::max(0,model.selection-3);
 model.motion.durationMs=std::max(50u,std::min(1000u,duration));
 model.topBar=topbar!=0;model.motion.reset(model.selection,model.first,0);
}
void menu_input(int kind,int value,unsigned now){model.input(char(kind),value,now);}
unsigned char* menu_render(unsigned now){menu_cleanup::render(surface,model,now);return surface.rgba;}
int menu_state(int field){switch(field){case 0:return model.menu;case 1:return model.selection;case 2:return model.first;case 3:return model.leaf;case 4:return model.home;case 5:return menu_cleanup_data::counts[model.menu];default:return 0;}}
}
