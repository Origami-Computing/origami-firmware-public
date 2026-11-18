#include "apps/onboarding_controls.h"
#include "apps/onboarding_flow.h"
#include <cassert>
#include <initializer_list>
#include <cstdint>
using apps::onboarding::Controls;
int main(){
 apps::onboarding::Flow replay;
 for(unsigned page=0;page<=143;++page){
  replay.setPage(page);
  assert(!replay.canExitReplay(false));
 }
 for(unsigned page:{3u,4u,25u,122u,123u,124u}){
  replay.setPage(page);
  assert(!replay.canExitReplay(true));
 }
 for(unsigned page:{0u,5u,6u,18u,31u,36u,40u,140u}){
  replay.setPage(page);
  assert(replay.canExitReplay(true));
 }
 replay.beginWifiSetup();
 assert(!replay.canExitReplay(true));
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
  Controls c;
  for(int direction:{-1,1}){
   c.reset(6);assert(c.angle==0);
   for(int i=1;i<=3;++i){assert(c.turn(direction,i)==1);assert(c.angle==direction*i*24&&!c.success);}
   assert(c.turn(direction,4)==1&&c.angle==direction*90&&c.success);
   assert(c.turn(direction,5)==0&&c.successAt==4);
   assert(!c.complete(553)&&c.complete(554));
   assert(c.turn(-direction,6)==1&&c.angle==direction*66&&!c.success);
   c.turn(direction,7);c.turn(-direction,8,true);assert(c.angle==direction*66&&!c.success);
   assert(c.turn(-direction*32,9)==7&&c.angle==-direction*90&&c.success);
  }
  c.reset(5);c.button(2,true,UINT32_MAX-300);c.button(2,false,UINT32_MAX-100);assert(!c.complete(100));assert(c.complete(450));
 }
}
