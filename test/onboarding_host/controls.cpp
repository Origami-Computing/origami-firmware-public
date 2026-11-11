#include "apps/onboarding_controls.h"
#include <cassert>
#include <initializer_list>
#include <cstdint>
using apps::onboarding::Controls;
int main(){
 for(unsigned run=0;run<1000;++run){
  for(unsigned page:{3u,4u,5u}){
   Controls c;c.reset(page);unsigned b=page==3?0:page==4?1:2;
   assert(!c.button(b,false,0)&&!c.success);
   assert(!c.button((b+1)%3,true,0)&&!c.held);
   assert(!c.button(b,true,10)&&c.held&&!c.success);
   assert(!c.complete(10000));assert(!c.button(b,true,10000));
   assert(c.button(b,false,10001)==1&&c.success&&!c.held);
   assert(!c.complete(10000)); // UI tick sampled before the release event must not underflow.
   assert(!c.button(b,false,10002));assert(!c.complete(10550));assert(c.complete(10551));
   c.reset(page);assert(!c.held&&!c.success);
  }
  Controls c;c.reset(6);
  assert(c.turn(-32,0)==0&&c.angle==0);
  for(int i=1;i<=7;++i){assert(c.turn(1,i)==1);assert(c.angle==i*24&&!c.success);}
  assert(c.turn(1,8)==1&&c.angle==180&&c.success);
  assert(c.turn(1,9)==0&&c.successAt==8);
  assert(c.turn(-1,10)==1&&c.angle==156&&!c.success);
  assert(c.turn(-32,11)==7&&c.angle==0);
  c.turn(7,12);c.turn(1,13);c.turn(-1,14,true);assert(c.angle==168&&!c.success);
  c.reset(5);c.button(2,true,UINT32_MAX-300);c.button(2,false,UINT32_MAX-100);assert(!c.complete(100));assert(c.complete(450));
 }
}
