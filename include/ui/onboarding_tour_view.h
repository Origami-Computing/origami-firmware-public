#pragma once
#include "ui/recorder_view.h"
#include "ui/camera_photographer_view.h"
#include "ui/recording_wave.h"
#include "ui/scene.h"
namespace ui {namespace onboarding_tour {
// Existing preview scene, retained as an illustrative camera snapshot. These are
// screen examples, never live microphone/camera/network sessions.
template<class C>void camera(C& c){
 c.fillRect(0,0,280,240,0xBDF7);c.fillRect(0,0,280,150,0xAD95);c.fillRect(0,150,280,90,0xC4CC);
 c.fillRect(28,26,106,113,0xffff);c.fillRect(34,32,94,101,0x9DDD);
 c.fillRect(78,32,6,101,0xffff);c.fillRect(34,78,94,6,0xffff);
 c.fillRect(186,101,7,67,0x43E9);c.fillRect(165,85,26,18,0x43E9);c.fillRect(189,108,25,16,0x53C8);c.fillRect(173,156,34,34,0xB266);c.fillRect(53,175,70,15,0xE73C);
 struct Read{C& c;uint16_t at(int x,int y)const{return c.readPixel(x,y);}void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x,y,w,h,ink);}} read{c};
 camera_photographer::overlays(read,0,true);
}
template<class C>void conversation(C& c){
 c.fillRect(0,0,280,240,0xffff);
 raster::mask(c,assets::kami_dialogue::kBody,26,40,24,26,0xf945,0xffff);
 raster::mask(c,assets::kami_dialogue::kEyes,30,47,16,10,0,0xf945);
 uint8_t wave[kRecordingWaveBars];for(int i=0;i<kRecordingWaveBars;++i)wave[i]=int(220*std::abs(std::sin(i*.7f+2500*.009f)*std::sin(i*.19f)));
 auto& sc=scene::scratchScene();sc.clear();recordingWave(sc,280,wave);sc.render(c);sc.clear();
}
}}
