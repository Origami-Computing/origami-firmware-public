#include "ui/eye_rig.h"
#include "hal/hal_display.h"

namespace ui { namespace eye_rig {
void draw(TFT_eSPI& target,int x,int y,int w,int h,const Pose& pose,float closure,uint16_t background,float nominalEyeWidth,float offsetX,float offsetY,bool composite,uint8_t opacity,int firstRow,int lastRow,float spacing,float aperture) {
  drawRaster(target,x,y,w,h,pose,closure,background,nominalEyeWidth,offsetX,offsetY,composite,opacity,firstRow,lastRow,spacing,aperture);
}
}}
