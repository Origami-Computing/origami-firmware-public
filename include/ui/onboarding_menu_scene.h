#pragma once
#include "ui/scene.h"
#include "ui/row_ink_alignment.h"
#include "ui/row_icon.h"
#include "ui/proposals/design_system.h"
namespace ui { namespace onboarding_menu {
// Render approved onboarding menu nodes identically to the Studio scene adapter.
// Other application menus retain their existing renderer and typography.
template<class C> void render(C& c,ui::scene::Scene& scene){
#ifndef ESP32
 scene.render(c);
#else
 unsigned order[ui::scene::Scene::kMaxNodes];for(unsigned i=0;i<scene.count();++i)order[i]=i;
 for(unsigned i=1;i<scene.count();++i){unsigned value=order[i],j=i;while(j&&scene.nodes()[order[j-1]].z>scene.nodes()[value].z){order[j]=order[j-1];--j;}order[j]=value;}
 for(unsigned i=0;i<scene.count();++i){const auto& n=scene.nodes()[order[i]];auto r=n.rect;if(!n.visible)continue;
  if(n.kind==ui::scene::NodeKind::Rect){
   for(int y=0;y<r.h;++y)for(int x=0;x<r.w;++x){
    int dx=x<n.radius?n.radius-x-1:x>=r.w-n.radius?x-(r.w-n.radius):0;
    int dy=y<n.radius?n.radius-y-1:y>=r.h-n.radius?y-(r.h-n.radius):0;
    if(dx*dx+dy*dy<=n.radius*n.radius)c.fillRect(r.x+x,r.y+y,1,1,n.fillColor);
   }
  }else if(n.kind==ui::scene::NodeKind::Label){
   const int size=n.textSize==2?18:12,w=ui::proportional::width(n.text,size),h=ui::proportional::height(n.text,size);
   const int x=r.x+(n.align!=ui::scene::TextAlign::TopLeft?(r.w-w)/2:0);int y=r.y+(n.align==ui::scene::TextAlign::MiddleCenter?(r.h-h)/2:0);
   if(strncmp(n.id,"list.row.",9)==0){char id[48];snprintf(id,sizeof(id),"%s",n.id);char* end=strrchr(id,'.');if(end){snprintf(end,sizeof(id)-(end-id),".bg");if(const auto* row=scene.find(id))y=ui::row_ink::textY(n.text,size,row->rect.y,row->rect.h);}}
   ui::design::text(c,n.text,x,y,size,n.fgColor,n.bgColor);
  }else if(n.kind==ui::scene::NodeKind::Bitmap){
   if(n.bitmapBpp==4&&strncmp(n.id,"list.row.",9)==0){char id[48];snprintf(id,sizeof(id),"%s",n.id);char* end=strrchr(id,'.');if(end){snprintf(end,sizeof(id)-(end-id),".bg");if(const auto* row=scene.find(id)){
     if(n.bitmapWidth==r.w && n.bitmapHeight==r.h){
      ui::row_icon::icon(c,n.bitmap,r.w,r.h,r.x,row->rect.y,row->rect.h,n.fgColor,n.bgColor);
      continue;
     }
     r.y=ui::row_ink::iconY(n.bitmap,n.bitmapWidth,n.bitmapHeight,row->rect.y,row->rect.h);
    }}}
   c.fillRect(r.x,r.y,r.w,r.h,n.bgColor);
   for(int y=0;y<r.h;++y)for(int x=0;x<r.w;++x){
    int sx=x*n.bitmapWidth/r.w,sy=y*n.bitmapHeight/r.h;
    if(n.bitmapBpp==4){unsigned pair=n.bitmap[sy*((n.bitmapWidth+1)/2)+sx/2],a=sx&1?pair&15:pair>>4;
     unsigned red=(((n.fgColor>>11)&31)*a+((n.bgColor>>11)&31)*(15-a)+7)/15;
     unsigned green=(((n.fgColor>>5)&63)*a+((n.bgColor>>5)&63)*(15-a)+7)/15;
     unsigned blue=((n.fgColor&31)*a+(n.bgColor&31)*(15-a)+7)/15;
     c.fillRect(r.x+x,r.y+y,1,1,(red<<11)|(green<<5)|blue);
    }else if(n.bitmap[sy*((n.bitmapWidth+7)/8)+sx/8]&(128>>(sx%8)))c.fillRect(r.x+x,r.y+y,1,1,n.fgColor);
   }
  }
 }
#endif
}
}}
