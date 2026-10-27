#pragma once
#include "ui/raster.h"
#include "ui/pixel_text.h"
#include "assets/kami_dialogue.h"
#include "ui/proposals/design_system.h"
namespace camera_photographer {
template<class C>void photographer(C& c,unsigned phase){
 int stage=phase/1000,age=phase%1000;
 int lift=stage==1?((int)age>=35?9:(int)age*9/35):(stage>=2&&stage<=4)?9:stage==5?((int)age>=140?0:9-(int)age*9/140):0;
 const int x=24,y=188;
 // Original device bitmap, native scale: body 24x26; eyes 16x10.
 ui::raster::maskOver(c,assets::kami_dialogue::kBody,x,y,24,26,0xF945);
 ui::raster::maskOver(c,assets::kami_dialogue::kEyes,x+4,y+7,16,10,0x1082);
 int cy=y+17-lift;
 c.fillRect(x+5,cy-2,5,2,0x1082);c.fillRect(x+3,cy,18,11,0x1082);
 c.fillRect(x+5,cy+2,14,7,0x4208);c.fillRect(x+10,cy+2,6,7,0xFFFF);
 c.fillRect(x+9,cy+3,8,5,0xFFFF);c.fillRect(x+11,cy+3,4,5,0x1082);
 c.fillRect(x+1,cy+4,5,5,0xF945);c.fillRect(x+19,cy+4,5,5,0xF945);
}
template<class C>void overlays(C& c,unsigned phase,bool tutorial=false){
 const uint16_t ink=0xFFFF;
 // Near the physical corners, inside the measured rounded bezel. The upper
 // right is reserved for B/C instead of squeezing a bracket behind the icons.
 for(int side=0;side<2;side++)for(int bottom=0;bottom<2;bottom++){
  if(side&&!bottom)continue;
  if(tutorial&&bottom)continue;
  int ox=side?262:16,oy=bottom?222:20,sx=side?-1:1,sy=bottom?-1:1;
  for(int i=6;i<20;i++){c.fillRect(ox+sx*i,oy,2,2,ink);c.fillRect(ox,oy+sy*i,2,2,ink);}
  c.fillRect(ox+sx*3,oy+sy,3,2,ink);
  c.fillRect(ox+sx,oy+sy*3,2,3,ink);
  c.fillRect(ox+sx*2,oy+sy*2,2,2,ink);
 }
 // White two-pixel strokes, no backing tiles. B is a simple camera silhouette
 // with open lens; C is a compact gear.
 c.fillRect(245,14,8,2,ink);
 c.fillRect(241,16,16,2,ink);
 c.fillRect(239,18,2,10,ink);c.fillRect(257,18,2,10,ink);
 c.fillRect(241,28,16,2,ink);
 c.fillRect(247,19,4,2,ink);c.fillRect(247,25,4,2,ink);
 c.fillRect(245,21,2,4,ink);c.fillRect(251,21,2,4,ink);
 const uint16_t gear[]={0x03c0,0x03c0,0x1bd8,0x3ffc,0x3ffc,0x1e78,0x0c30,0x1c38,0x1c38,0x0c30,0x1e78,0x3ffc,0x3ffc,0x1bd8,0x03c0,0x03c0};
 for(int y=0;y<16;y++)for(int x=0;x<16;x++)if(gear[y]&(1<<(15-x)))c.fillRect(241+x,53+y,1,1,ink);
 if(tutorial){
  struct Move{C& c;uint16_t at(int x,int y)const{return c.at(x,y-30);}void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x,y-30,w,h,ink);}} moved{c};
  photographer(moved,phase);
 }else photographer(c,phase);
 int stage=phase/1000;
 const char* label=stage==1?"CAPTURE":stage==2?"CAPTURE":stage==3||stage==4?"SAVING":stage==5?"SAVED":stage==6?"SAVE FAILED":"";
 if(*label)ui::pixel_text::text(c,label,56,tutorial?172:202,1,ink);
 if(tutorial)ui::design::instruction(c,"Press B to take photo");
}

}
