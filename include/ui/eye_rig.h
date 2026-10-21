#pragma once
#include "ui/eye_rig_math.h"
#include "ui/kami_expression.h"
class TFT_eSPI;
namespace ui { namespace eye_rig {
// Relative to the original web eye-center distance; eye size is unchanged.
constexpr float kDefaultSpacing = 1.f;
constexpr float kHomeSpacing = .9f;
inline float centerSeparation(float nominalEyeWidth,float spacing=kDefaultSpacing) {
  return nominalEyeWidth*1.2f*spacing;
}
// The supplied rectangle is a local backing surface. All pixels are restored,
// which makes this safe on white pages and the solid red area of the body.
void draw(TFT_eSPI& target,int x,int y,int w,int h,const Pose& pose,
          float closure=1,uint16_t background=0xffff,float nominalEyeWidth=0,
          float offsetX=0,float offsetY=0,bool composite=false,uint8_t opacity=255,
          int firstRow=0,int lastRow=-1,float spacing=kDefaultSpacing,float aperture=1);
void drawExpression(TFT_eSPI& target,int x,int y,int w,int h,const Pose& pose,
                    const kami_expression::Frame& frame,uint16_t background=0xffff,
                    float nominalEyeWidth=0,float spacing=kDefaultSpacing,
                    float offsetX=0,float offsetY=0);
void drawRingingEyes(TFT_eSPI& target,uint32_t ms,uint16_t background);
void drawWakeFace(TFT_eSPI& target,int x,int y,int w,int h,float amount);
void drawHomeMorph(TFT_eSPI& target,int x,int y,int w,int h,float squeeze,float blink,
                   float nominal,float offsetX,float offsetY,int firstRow=0,int lastRow=-1);
void drawExpressionMarks(TFT_eSPI& target,int x,int y,int w,int h,
                         float centerX,float centerY,float eyeWidth,
                         const kami_expression::Frame& frame,uint16_t background=0xffff);
}}
