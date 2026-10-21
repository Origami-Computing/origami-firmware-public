#include "services/agent_delivery.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <cstring>
namespace services { namespace agent_delivery {
namespace {
struct Receipt { char id[48]{}; int segment=-1; bool received=false,displayed=false,rejected=false; uint8_t confirmed=0; uint32_t last=0; char reason[24]{}; };
Receipt slots[32];
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
Receipt* find(const char* id) { for(auto& r:slots) if(id&&id[0]&&!strcmp(r.id,id))return &r;return nullptr; }
}
void received(const char* id) {
 if(!id||!id[0]||strlen(id)>=sizeof(slots[0].id))return;
 portENTER_CRITICAL(&mux);
 auto* r=find(id);
 if(!r)for(auto& slot:slots)if(!slot.id[0] || (slot.confirmed&6)){r=&slot;*r={};strlcpy(r->id,id,sizeof(r->id));break;}
 if(r)r->received=true;
 portEXIT_CRITICAL(&mux);
 Serial.printf("[agent-delivery] id=%s stage=received tracked=%u\n",id,r!=nullptr);
}
void disconnected() {
 portENTER_CRITICAL(&mux);for(auto& r:slots)r={};portEXIT_CRITICAL(&mux);
}
void bind(const char* id,int segment) {portENTER_CRITICAL(&mux);if(auto* r=find(id))r->segment=segment;portEXIT_CRITICAL(&mux);}
void displayed(int segment) {portENTER_CRITICAL(&mux);for(auto& r:slots)if(r.id[0]&&r.segment==segment&&!r.rejected&&!r.displayed){r.displayed=true;r.last=0;}portEXIT_CRITICAL(&mux);}
void reject(const char* id,const char* reason) {portENTER_CRITICAL(&mux);if(auto* r=find(id)){if(!r->displayed){r->rejected=true;strlcpy(r->reason,reason,sizeof(r->reason));r->last=0;}}portEXIT_CRITICAL(&mux);}
void replaceView() {
 portENTER_CRITICAL(&mux);
 for(auto& r:slots)if(r.id[0]&&r.segment>=0){if(!r.displayed){r.rejected=true;strlcpy(r.reason,"cancelled",sizeof(r.reason));r.last=0;}r.segment=-1;}
 portEXIT_CRITICAL(&mux);
}
void dismiss() {portENTER_CRITICAL(&mux);for(auto& r:slots)if(r.id[0]&&r.segment!=-2){if(!r.displayed){r.rejected=true;strlcpy(r.reason,"left_app",sizeof(r.reason));r.last=0;}r.segment=-1;}portEXIT_CRITICAL(&mux);}
void confirmed(const char* id,const char* stage) {
 portENTER_CRITICAL(&mux);if(auto* r=find(id)){if(!strcmp(stage,"received"))r->confirmed|=1;else if(!strcmp(stage,"displayed"))r->confirmed|=2;else if(!strcmp(stage,"rejected"))r->confirmed|=4;
 if(r->segment==-2&&(r->confirmed&1))*r={};}portEXIT_CRITICAL(&mux);
 Serial.printf("[agent-delivery] id=%s confirmed=%s\n",id,stage);
}
void pump(bool (*send)(const char*)) {
 for(auto& slot:slots){
  Receipt r;const uint32_t now=millis();uint8_t stage=0;
  portENTER_CRITICAL(&mux);
  if(slot.id[0]&&(!slot.last||uint32_t(now-slot.last)>=2000)){
   if(!(slot.confirmed&1))stage=1;
   else if(slot.displayed&&!(slot.confirmed&2))stage=2;
   else if(slot.rejected&&!(slot.confirmed&4))stage=4;
   if(stage){r=slot;slot.last=now;}
  }
  portEXIT_CRITICAL(&mux);
  if(!stage)continue;
  char wire[192];const char* name=stage==1?"received":stage==2?"displayed":"rejected";
  snprintf(wire,sizeof(wire),"{\"type\":\"delivery_ack\",\"receiptId\":\"%s\",\"stage\":\"%s\",\"reason\":\"%s\"}",r.id,name,r.reason);
  if(send(wire))Serial.printf("[agent-delivery] id=%s sent=%s\n",r.id,name);
 }
}
} }
