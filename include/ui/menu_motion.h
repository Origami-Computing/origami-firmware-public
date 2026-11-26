#pragma once
#include <algorithm>
namespace ui { namespace menu_list {
constexpr int pitch=56, rowHeight=50, top=16;
constexpr unsigned duration=220;
inline float ease(float t){t=std::max(0.f,std::min(1.f,t));return t*t*(3-2*t);}
struct Pose {float top,bottom,scroll;};
struct Motion {
 Pose from{16,66,0},to{16,66,0};unsigned at=0;unsigned durationMs=duration;
 Pose sample(unsigned now)const{
  // Preserve the original curve ratios independently of the configured duration.
  float age=float(now-at)*360/std::max(1u,durationMs),lead=ease(age/240),trail=ease((age-90)/270);
  bool down=to.top>=from.top;
  return {from.top+(to.top-from.top)*(down?trail:lead),from.bottom+(to.bottom-from.bottom)*(down?lead:trail),from.scroll+(to.scroll-from.scroll)*ease(age/360)};
 }
 void move(int selection,int first,unsigned now){from=sample(now);to={float(top+selection*pitch),float(top+selection*pitch+rowHeight),float(first*pitch)};at=now;}
 void reset(int selection,int first,unsigned now){from=to={float(top+selection*pitch),float(top+selection*pitch+rowHeight),float(first*pitch)};at=now;}
};
}}
