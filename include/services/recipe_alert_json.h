#pragma once
#include "services/recipe_alerts.h"
#include <ArduinoJson.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

namespace services { namespace recipe_alerts {
inline bool parseEntry(JsonObjectConst object, Entry& entry) {
  entry = {};
  if (object.isNull()) return false;
  const char* id=object["id"].as<const char*>();
  if(id) {
    if(!*id || strlen(id)>=sizeof(entry.id)) return false;
    strcpy(entry.id,id);
  } else if(object["id"].is<uint32_t>() && object["id"].as<uint32_t>()!=0) {
    snprintf(entry.id,sizeof(entry.id),"%lu",(unsigned long)object["id"].as<uint32_t>());
  } else return false;
  const char* type=object["type"] | object["Type"];
  if(!type) return false;
  if(!strcmp(type,"yellow")) entry.type=automation::Type::Yellow;
  else if(!strcmp(type,"red")) entry.type=automation::Type::Red;
  else if(!strcmp(type,"green")) entry.type=automation::Type::Green;
  else return false;
  JsonVariantConst value=object["time_unix"];
  if(value.isNull()) value=object["time"];
  if(value.isNull()) value=object["fire_at"];
  int64_t seconds=0;
  if(value.is<int64_t>()) seconds=value.as<int64_t>();
  else if(value.is<const char*>()) {
    const char* text=value.as<const char*>(); char* end=nullptr;
    seconds=strtoll(text,&end,10);
    if(end==text || *end) return false;
  } else return false;
  // ESP32 Arduino time_t is signed 32-bit on this toolchain.
  if(seconds<=1000000000LL || seconds>INT32_MAX) return false;
  entry.fire=(time_t)seconds;
  const char* message=object["message"].as<const char*>();
  if(!message || strlen(message)>=sizeof(entry.message)) return false;
  strcpy(entry.message,message);
  return true;
}
inline bool parseSnapshot(JsonVariantConst root, Snapshot& snapshot) {
  // No partial reconciliation: malformed, oversized and legacy responses are
  // failures, not empty lists. Only a successful authoritative feed may delete.
  if(!root["authoritative"].is<bool>() || !root["authoritative"].as<bool>() ||
     !root["items"].is<JsonArrayConst>()) return false;
  auto items=root["items"].as<JsonArrayConst>();
  if(items.size()>kCapacity) return false;
  Snapshot parsed;
  for(JsonVariantConst item:items) {
    Entry& entry=parsed.items[parsed.count];
    if(!parseEntry(item.as<JsonObjectConst>(),entry)) return false;
    for(unsigned i=0;i<parsed.count;++i) if(!strcmp(parsed.items[i].id,entry.id)) return false;
    ++parsed.count;
  }
  snapshot=parsed; return true;
}
} }
