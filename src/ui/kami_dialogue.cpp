#include "ui/kami_dialogue.h"
#include "assets/kami_dialogue.h"
#include "config/ui_theme.h"
#include "ui/eye_rig.h"
namespace ui { namespace kami_dialogue {
static uint16_t blend(uint16_t fg,uint16_t bg,int a){
  const int r=(((fg>>11)&31)*a+((bg>>11)&31)*(15-a)+7)/15;
  const int g=(((fg>>5)&63)*a+((bg>>5)&63)*(15-a)+7)/15;
  const int b=((fg&31)*a+(bg&31)*(15-a)+7)/15;
  return (r<<11)|(g<<5)|b;
}
static void mask(TFT_eSPI& t,const uint8_t* data,int x,int y,int w,int h,uint16_t fg,uint16_t bg){
  uint16_t palette[16];for(int a=0;a<16;++a)palette[a]=blend(fg,bg,a);
  for(int py=0;py<h;++py)for(int px=0;px<w;++px){
    const int i=py*w+px;const uint8_t v=pgm_read_byte(data+i/2);
    t.drawPixel(x+px,y+py,palette[(v>>(i%2?0:4))&15]);
  }
}
void drawAvatar(TFT_eSPI& t,int x,int y,bool open,const char* emotion){
  const auto* preset=eye_rig::preset(emotion);
  kami_expression::Frame frame;bool named=kami_expression::namedShape(emotion,frame.shape);
  drawAvatarPose(t,x,y,open,preset?preset->pose:eye_rig::kPresets[0].pose,eye_rig::kDefaultSpacing,named?&frame:nullptr);
}
void drawAvatarPose(TFT_eSPI& t,int x,int y,bool open,const eye_rig::Pose& pose,float spacing,const kami_expression::Frame* expression){
  constexpr uint16_t red=0xF945;
  mask(t,assets::kami_dialogue::kBody,x,y,24,26,red,kUiBg);
  kami_expression::Frame frame;if(!open)frame.shape=kami_expression::Shape::Squeeze;
  if(expression)frame=*expression;
  eye_rig::drawExpression(t,x+4,y+7,16,10,pose,frame,red,0,spacing);
  eye_rig::drawExpressionMarks(t,x,y,24,26,x+12,y+12,6.8f,frame,red);
}
}}
