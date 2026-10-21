#include "ui/eye_rig.h"
#include "hal/hal_display.h"

namespace ui { namespace eye_rig {
void draw(TFT_eSPI& target,int x,int y,int w,int h,const Pose& pose,float closure,uint16_t background,float nominalEyeWidth,float offsetX,float offsetY,bool composite,uint8_t opacity,int firstRow,int lastRow,float spacing,float aperture) {
  if(w<=0 || h<=0 || w>280 || !opacity)return;
  // Keep web proportions. A 195px pair is approximately 89px per eye.
  const float nominal=nominalEyeWidth>0?nominalEyeWidth:fminf(w/2.2f,h*67.f/91.f);
  const float scaleX=1+(pose.scale-1)*.4f;
  float ew=nominal*scaleX,eh=nominal*91.f/67.f*pose.scale;
  // Caller reserves expression headroom in the backing rect. If it doesn't,
  // contain the entire pose rather than silently clipping surprised eyes.
  const float distance=centerSeparation(nominal,spacing);
  float fit=fminf(1.f,fminf(w/(distance+ew),h/fmaxf(1,eh*fmaxf(1,closure))));
  ew*=fit;eh*=fit;
  const float separation=distance*fit;
  const float center=x+w*.5f+offsetX,cy=y+h*.5f+offsetY;
  int32_t coverage[280];uint16_t pixels[280],palette[256];
  constexpr uint16_t ink=0x18c3; // RGB565 of web #1a1a1a.
  auto blend=[](int alpha,uint16_t bg){
    int r=(((ink>>11)&31)*alpha+((bg>>11)&31)*(255-alpha)+127)/255;
    int g=(((ink>>5)&63)*alpha+((bg>>5)&63)*(255-alpha)+127)/255;
    int b=((ink&31)*alpha+(bg&31)*(255-alpha)+127)/255;
    uint16_t color=(r<<11)|(g<<5)|b;
    return (uint16_t)((color>>8)|(color<<8));
  };
  if(!composite)for(int alpha=0;alpha<256;++alpha)palette[alpha]=blend(alpha,background);
  // Sprite buffers use wire byte order. Pre-swap once in the palette so the
  // sprite push is a bulk memcpy, not two byte writes per pixel in slow PSRAM.
  const bool swap=target.getSwapBytes();target.setSwapBytes(false);
  if(firstRow<0)firstRow=0;
  if(lastRow<0||lastRow>h)lastRow=h;
  for(int row=firstRow;row<lastRow;++row) {
    memset(coverage,0,w*sizeof(int32_t));
    eyeRow(coverage,w,x,y+row,center-separation*.5f,cy,ew,eh*closure,pose.left,aperture);
    eyeRow(coverage,w,x,y+row,center+separation*.5f,cy,ew,eh*closure,pose.right,aperture);
    int32_t sum=0;
    for(int col=0;col<w;++col) {
      sum+=coverage[col];
      const int32_t bounded=sum<0?0:sum>65536?65536:sum;
      int alpha=(bounded*opacity+32768)>>16;
      pixels[col]=composite?blend(alpha,target.readPixel(x+col,y+row)):palette[alpha];
    }
    target.pushImage(x,y+row,w,1,pixels);
  }
  target.setSwapBytes(swap);
}
}}
