#pragma once
#include "apps/agent/agent_emotion_model.h"
namespace apps { namespace agent { namespace emotion {
void reset();
bool receive(JsonVariantConst event,bool active);
void bind(int segment);
Selection snapshot(int segment);
bool developmentSet(const char* name);
}}}
