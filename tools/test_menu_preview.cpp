#include "ui/menu_motion.h"
#include <cassert>
#include <cmath>
#include <cstdio>
int main(){
 using namespace ui::menu_list;
 static_assert(duration==220,"Firmware transition duration");
 Motion defaults;defaults.reset(0,0,0);defaults.move(1,0,0);
 assert(defaults.sample(219).top<72&&defaults.sample(220).top==72);
 for(unsigned speed:{100u,200u,220u,300u,360u,1000u}){
  Motion baseline,test;baseline.durationMs=360;test.durationMs=speed;
  baseline.reset(3,0,0);test.reset(3,0,0);
  baseline.move(4,1,0);test.move(4,1,0);
  for(unsigned quarter=0;quarter<=4;++quarter){
   auto a=baseline.sample(quarter*360/4),b=test.sample(quarter*speed/4);
   assert(std::abs(a.top-b.top)<0.001f&&std::abs(a.bottom-b.bottom)<0.001f&&std::abs(a.scroll-b.scroll)<0.001f);
  }
  auto before=test.sample(speed/2);test.move(2,0,speed/2);auto after=test.sample(speed/2);
  assert(before.top==after.top&&before.bottom==after.bottom&&before.scroll==after.scroll);
  auto end=test.sample(speed+speed/2);assert(end.top==128&&end.bottom==178&&end.scroll==0);
 }
 puts("PASS duration scaling preserves stretch, scrolling and interrupted-motion continuity");
}
