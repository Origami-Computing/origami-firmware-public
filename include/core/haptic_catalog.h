#pragma once
#include "core/haptic_effect.h"

// Exact saved Studio profiles; source: tools/haptic-studio/firmware-profiles.json.
namespace core { namespace haptic { namespace catalog {
static const Effect scroll = [] { Effect e; e.count=1; e.pulses[0]={0,50,65}; return e; }();
static const Effect select = [] { Effect e; e.count=2; e.pulses[0]={0,25,70}; e.pulses[1]={150,25,100}; return e; }();
static const Effect back = [] { Effect e; e.count=2; e.pulses[0]={0,25,100}; e.pulses[1]={150,25,60}; return e; }();
static const Effect dizzy = [] { Effect e; e.count=1; e.pulses[0]={0,2000,20}; return e; }();
} } }
