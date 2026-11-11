#pragma once
#include <stdint.h>
namespace services { namespace camera_grid {
// One UI-task session. Captures a still through Camera Session, then submits it
// to the existing preview decode worker. No app callback is retained. close()
// joins decode before releasing its camera/radio reservation. No photo is saved.
constexpr unsigned width=11,height=7,count=width*height;
enum class State:uint8_t {Closed,Waiting,Decoding,Ready,Failed};
struct Image {uint16_t color[count];uint8_t luminance[count];};
bool begin(); // Schedules capture; startup uses the existing blocking still policy.
void poll();
void close();
State state();
const char* error();
bool copy(Image&); // Owned output copy, only when Ready.
}}
