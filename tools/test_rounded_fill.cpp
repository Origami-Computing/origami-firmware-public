#include "ui/rounded_fill.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace ui::rounded_fill;
struct Canvas {
    std::vector<uint16_t> pixels=std::vector<uint16_t>(280*240,0xffff);
    Clip clip{0,0,280,240};int calls=0;
    void fillRect(int x,int y,int w,int h,uint16_t color){
        assert(x>=clip.x&&y>=clip.y&&x+w<=clip.x+clip.w&&y+h<=clip.y+clip.h);
        assert(w>0&&h>0);++calls;
        for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)pixels[yy*280+xx]=color;
    }
};
// Independent area-coverage reference, with 32x32 subpixel samples per edge pixel.
static float reference(Shape s,int x,int y){
    int inside=0;const float r=std::min(s.radius,std::min(s.w,s.h)/2);
    for(int a=0;a<32;++a)for(int b=0;b<32;++b){
        const float px=x+(a+.5f)/32,py=y+(b+.5f)/32;
        if(px<s.x||px>s.x+s.w||py<s.y||py>s.y+s.h)continue;
        const float cx=std::max(s.x+r,std::min(s.x+s.w-r,px));
        const float cy=std::max(s.y+r,std::min(s.y+s.h-r,py));
        if((px-cx)*(px-cx)+(py-cy)*(py-cy)<=r*r)++inside;
    }
    return inside*255.f/1024;
}
int main(){
    assert(blend(0x1234,0xabcd,0)==0xabcd&&blend(0x1234,0xabcd,255)==0x1234);
    for(float offset: {0.f,.25f,.5f,.75f})for(float height:{35.f,50.f,78.f,106.f}){
        const Shape s{12,16+offset,256,height,25};Canvas c;
        draw(c,s,0,0xffff,{0,0,280,240});int fractional=0;
        for(int y=15;y<int(18+height);++y)for(int x=11;x<269;++x){
            unsigned a=coverage(s,x,y);
            if(a&&a<255){++fractional;assert(std::abs(reference(s,x,y)-a)<35);}
            assert(a==coverage(s,279-x,y));
        }
        assert(fractional>20);
        assert(c.pixels[(int(s.y)+int(height/2))*280+140]==0);
        assert(c.pixels[16*280+12]==0xffff);
        assert(c.calls<700); // Filled interiors remain horizontal spans.
    }
    Canvas clipped;clipped.clip={0,16,280,218};
    draw(clipped,{12,-20.25f,256,106,25},0,0xffff,clipped.clip);
    for(int y=0;y<16;++y)for(int x=0;x<280;++x)assert(clipped.pixels[y*280+x]==0xffff);
    for(int y=234;y<240;++y)for(int x=0;x<280;++x)assert(clipped.pixels[y*280+x]==0xffff);
    Canvas empty;draw(empty,{0,0,0,50,25},0,0xffff,{0,0,280,240});assert(!empty.calls);
    // Repainting the same flat-background shape is stable, without edge darkening.
    auto before=clipped.pixels;draw(clipped,{12,-20.25f,256,106,25},0,0xffff,clipped.clip);assert(before==clipped.pixels);
    puts("PASS native antialias coverage, symmetry, clipping, span budget and repaint stability");
}
