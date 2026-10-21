#pragma once
#include "apps/agent/agent_state.h"
namespace apps { namespace agent { namespace motion {
// UI-thread only. Animation never sleeps or owns microphone/network state.
void beforeStage(Stage from, Stage to);
void prepareHome();
void startHome();
void startListening(uint32_t now, float x=0, float y=0);
void drawListening(AgentState& s, uint32_t now);
void startThinking();
void drawThinking(uint32_t now);
void startReply();
void completeReply(AgentState& s);
bool tick(AgentState& s, uint32_t now);
void cancel();
void status();
bool ownsStatusBackground();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool seek(int ms);
#endif
}}}
