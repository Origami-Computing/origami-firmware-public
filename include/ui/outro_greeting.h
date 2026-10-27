#pragma once
#include "ui/wake_greeting.h"
namespace ui { namespace outro_greeting {
constexpr uint32_t kDuration=2180;
// Exactly the wake expression/hand clock, except the initial closed-to-open lids.
// Start open, then use its tilted convex happy eyes during the wave.
inline wake_greeting::Frame sample(uint32_t age){
 auto f=wake_greeting::sample(age);
 if(age<300)f.face=0;
 return f;
}
}}
