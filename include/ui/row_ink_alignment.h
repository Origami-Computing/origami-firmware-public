#pragma once
#include "ui/proportional_text.h"
namespace ui { namespace row_ink {
// design::text uses the "Ag" ascent. Convert the shared cap baseline to its y origin.
inline int textY(const char*,int size,int rowY,int rowH){
 return proportional::rowBaseline(rowY,rowH,size)+proportional::top("Ag",size);
}
inline int iconY(const uint8_t* mask,int w,int h,int rowY,int rowH){
 int first=h,last=-1;
 for(int y=0;y<h;++y)for(int x=0;x<w;++x){unsigned pair=mask[y*((w+1)/2)+x/2],a=x&1?pair&15:pair>>4;if(a){first=std::min(first,y);last=std::max(last,y);}}
 return last<first?rowY:rowY+(rowH-(last-first+1))/2-first;
}
}}
