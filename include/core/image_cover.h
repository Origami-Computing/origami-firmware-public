#pragma once
#include <stdint.h>
namespace core { namespace images {
// One rational scale for both axes, with a centered source crop. Pixel centers
// are mapped directly, so small and large JPEGs cover the viewport without stretch.
struct Cover {
 int sw,sh,w,h,num,den;
 Cover(int sourceW,int sourceH,int width,int height):sw(sourceW),sh(sourceH),w(width),h(height){
  if(int64_t(w)*sh>=int64_t(h)*sw){num=w;den=sw;}else{num=h;den=sh;}
 }
 int source(int p,int size,int extent)const{
  int v=int(((int64_t(2)*p+1)*den+int64_t(size)*num-int64_t(extent)*den)/(int64_t(2)*num));
  return v<0?0:v>=size?size-1:v;
 }
 int x(int p)const{return source(p,sw,w);}int y(int p)const{return source(p,sh,h);}
 // First destination pixel sampling at or after a source boundary.
 int boundary(int s,bool horizontal)const{
  int lo=0,hi=horizontal?w:h;
  while(lo<hi){int mid=(lo+hi)/2;if((horizontal?x(mid):y(mid))<s)lo=mid+1;else hi=mid;}
  return lo;
 }
};
}}
