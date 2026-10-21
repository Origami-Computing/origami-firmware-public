#include "apps/agent/agent_emotion.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include "core/memory.h"
#include <new>
namespace apps { namespace agent { namespace emotion {
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
// The 16 reply poses belong in PSRAM, leaving internal memory for task stacks.
// Allocate once at turn setup, never while holding the lock or painting a frame.
static Turn* turn=nullptr;
static void ensure(){
  portENTER_CRITICAL(&mux);bool ready=turn!=nullptr;portEXIT_CRITICAL(&mux);
  if(ready)return;
  void* storage=core::memory::allocPsram("agent.emotions",sizeof(Turn));if(!storage)return;
  Turn* candidate=new(storage) Turn;
  portENTER_CRITICAL(&mux);
  if(!turn){turn=candidate;storage=nullptr;}
  portEXIT_CRITICAL(&mux);
  if(storage){candidate->~Turn();core::memory::freePsram(storage);}
}
void reset(){ensure();portENTER_CRITICAL(&mux);if(turn)turn->reset();portEXIT_CRITICAL(&mux);}
void bind(int segment){portENTER_CRITICAL(&mux);if(turn)turn->bind(segment);portEXIT_CRITICAL(&mux);}
Selection snapshot(int segment){portENTER_CRITICAL(&mux);Selection s=turn?turn->get(segment):Selection{};portEXIT_CRITICAL(&mux);return s;}
bool receive(JsonVariantConst event,bool active){
  const char* type=event["type"]|"";
  const bool isEmotion=!strcmp(type,"emotion");
  const bool toolStart=!strcmp(type,"tool_start"),toolEnd=!strcmp(type,"tool_end");
  if(!isEmotion&&!toolStart&&!toolEnd)return false;
  // Background text events belong to their request, not the foreground face.
  if(!active||!event["requestId"].isNull())return true;
  if(toolStart||toolEnd){
    const char* name=event["name"]|"";
    portENTER_CRITICAL(&mux);if(turn)turn->tool(name,toolStart);portEXIT_CRITICAL(&mux);return true;
  }
  Selection next;
  if(!parse(event["emotion"],next)){Serial.println("[agent-emotion] rejected invalid payload");return true;}
  portENTER_CRITICAL(&mux);bool available=turn!=nullptr;bool accepted=turn&&turn->accept(next);portEXIT_CRITICAL(&mux);
  Serial.printf("[agent-emotion] %s=%s\n",accepted?"received":available?"cleanup":"unavailable",next.name);return true;
}
bool developmentSet(const char* name){
  JsonDocument doc;doc["type"]="emotion";doc["emotion"]["preset"]=name;
  Selection check;if(!parse(doc["emotion"],check))return false;
  reset();receive(doc.as<JsonVariantConst>(),true);bind(0);return snapshot(0).received;
}
}}}
