#pragma once
#include "ui/kami_expression.h"
namespace ui { namespace kami_motion {
void trigger(kami_expression::Action action,uint32_t now);
void poll(uint32_t now,bool enabled,bool homePhysics=false);
// UI-core cache: consumers never initiate I2C reads. Expires after 100ms.
bool acceleration(uint32_t now,int32_t xyz[3]);
void printStats(uint32_t now);
kami_expression::Frame frame(uint32_t now);
bool active(uint32_t now);
void reset();
void hold(kami_expression::Action action,uint32_t age);
void inject(uint32_t now,const int32_t xyz[3]);
void resetDetector();
const char* currentName(uint32_t now);
}}
