#pragma once
#include "ui/pixel_text.h"
#include "ui/onboarding_dialogue_view.h"
#include "assets/wifi_icons.h"
namespace onboarding_wifi {
template<class C> void disconnectedWifi(C& c){
 // Use the same final-size, antialiased offline mask as the device status bar.
 for(unsigned y=0;y<assets::wifi_icons::kHeight;++y)for(unsigned x=0;x<assets::wifi_icons::kWidth;++x){
  const unsigned mask=assets::wifi_icons::kOffline[y*assets::wifi_icons::kWidth+x];
  const unsigned a=(mask&15)*(mask&16?3:10),denom=150;
  const unsigned r=(31*(denom-a)+denom/2)/denom,g=(63*(denom-a)+denom/2)/denom;
  c.fillRect(24+x,12+y,1,1,(r<<11)|(g<<5)|r);
 }
}
template<class C> void scannerHeader(C& c){
 c.fillRect(0,0,280,60,kUiBg);
 disconnectedWifi(c);arrows(c,false,true);
 ui::pixel_text::text(c,"B: skip",180,35,2,kUiFg);
}
}
