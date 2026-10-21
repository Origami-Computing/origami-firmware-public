#include "core/device_agent_text.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <string.h>

namespace core { namespace device_agent_text {
namespace {
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
bool pending=false, completed=false, sent=false, authenticated=false, supported=false;
bool foreground=false;
uint32_t started=0;
char id[81]={}, prompt[768]={};
Result result;

void fail(Outcome outcome, const char* message, bool retryable=false) {
  result.outcome=outcome;
  result.retryable=retryable;
  strlcpy(result.error,message,sizeof(result.error));
  completed=true;
}
}

bool submit(const char* requestId, const char* text) {
  if (!requestId || !text || !*requestId || !*text || strlen(requestId)>=sizeof(id) || strlen(text)>=sizeof(prompt)) return false;
  portENTER_CRITICAL(&mux);
  if (pending) { portEXIT_CRITICAL(&mux); return false; }
  strlcpy(id,requestId,sizeof(id)); strlcpy(prompt,text,sizeof(prompt));
  result={}; pending=true; completed=false; sent=false; started=millis();
  portEXIT_CRITICAL(&mux);
  return true;
}
bool take(Result& out) {
  portENTER_CRITICAL(&mux);
  if (!pending || !completed) { portEXIT_CRITICAL(&mux); return false; }
  out=result; pending=false; completed=false;
  portEXIT_CRITICAL(&mux);
  return true;
}
bool hasPendingRequest() {
  portENTER_CRITICAL(&mux);
  const bool value = pending && !completed;
  portEXIT_CRITICAL(&mux);
  return value;
}
void cancel() {
  portENTER_CRITICAL(&mux);
  pending=false; completed=false; sent=false;
  portEXIT_CRITICAL(&mux);
}
void disconnected() {
  portENTER_CRITICAL(&mux);
  authenticated=false; supported=false; sent=false;
  if (!completed) result={};
  portEXIT_CRITICAL(&mux);
}
void ready(bool supportsText) {
  portENTER_CRITICAL(&mux);
  authenticated=true; supported=supportsText;
  portEXIT_CRITICAL(&mux);
}
bool receive(const JsonDocument& event) {
  // Correlated events must never reach the voice UI, even after cancellation.
  const char* requestId=event["requestId"] | "";
  if (!*requestId) return false;
  portENTER_CRITICAL(&mux);
  if (pending && sent && !completed && !foreground && strcmp(requestId,id)==0) {
    const char* type=event["type"] | "";
    if (!strcmp(type,"message")) {
      const char* text=event["text"] | "";
      size_t used=strlen(result.text), extra=strlen(text);
      if (used+extra+(used?1:0)>=sizeof(result.text)) fail(Outcome::Error,"Agent reply too long");
      else if (extra) {
        if (used) result.text[used++]=' ';
        memcpy(result.text+used,text,extra+1);
      }
    } else if (!strcmp(type,"error")) {
      fail(Outcome::Error,event["message"] | "Agent request failed",event["retryable"] | false);
    } else if (!strcmp(type,"done")) {
      if (!result.text[0]) fail(Outcome::Error,"Agent returned no text");
      else { result.outcome=Outcome::Ready; completed=true; }
    }
  }
  portEXIT_CRITICAL(&mux);
  return true;
}
void pump(uint32_t now, bool connected, bool voiceActive, bool (*send)(const char*)) {
  char requestId[81], text[768];
  bool transmit=false;
  portENTER_CRITICAL(&mux);
  foreground=voiceActive;
  if (pending && !completed) {
    if (voiceActive) { sent=false; result={}; started=now; }
    else if ((uint32_t)(now-started)>150000u) fail(Outcome::Error,"Agent request timed out",true);
    else if (connected && authenticated && !supported) fail(Outcome::Unsupported,"Server needs text-turn support");
    else if (connected && authenticated && supported && !sent) {
      strlcpy(requestId,id,sizeof(requestId)); strlcpy(text,prompt,sizeof(text));
      result={}; transmit=true; sent=true;
    }
  }
  portEXIT_CRITICAL(&mux);
  if (!transmit) return;
  JsonDocument doc;
  doc["type"]="text"; doc["requestId"]=requestId; doc["text"]=text;
  doc["sessionId"]="origami-device"; doc["internal"]=true;
  char wire[1200];
  const size_t count=serializeJson(doc,wire,sizeof(wire));
  if (count>=sizeof(wire) || !send(wire)) {
    portENTER_CRITICAL(&mux);
    if (strcmp(id,requestId)==0) sent=false;
    portEXIT_CRITICAL(&mux);
  } else Serial.printf("[agent-text] submitted id=%s\n",requestId);
}
} }
