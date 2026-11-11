#include <cassert>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include "Arduino.h"
#include "services/automation.h"
#include "services/vibration.h"
#include "core/alert_vibration.h"
#include "services/recipe_alert_json.h"
static time_t now=1800000000;
static bool storageFails=false;
extern "C" time_t time(time_t* out)
#ifndef __APPLE__
noexcept
#endif
{ if(out)*out=now; return now; }
namespace core { namespace store {
std::map<std::string,std::vector<unsigned char>> storage;
bool putBlob(const char*,const char* key,const void* value,size_t size) {
  if(storageFails)return false;
  auto p=(const unsigned char*)value; storage[key]={p,p+size};return true;
}
bool getBlob(const char*,const char* key,void* value,size_t size) {
  auto it=storage.find(key);if(it==storage.end() || it->second.size()!=size)return false;
  memcpy(value,it->second.data(),size);return true;
}
} namespace alert_vibration {
uint8_t validId(uint8_t id){return id;}
} }
namespace services { namespace vibration { void drive(Owner,uint8_t,uint32_t){} void release(Owner){} } namespace automation { void transport_poll_service(uint32_t){} void beginRecipeTransport(){} } }
using namespace services::automation;
using namespace services::recipe_alerts;
static void tick(){service(1000);}
static bool feed(const std::string& payload){
  JsonDocument doc;if(deserializeJson(doc,payload))return false;
  Snapshot snapshot;if(!parseSnapshot(doc.as<JsonVariantConst>(),snapshot))return false;
  applyRecipeSnapshot(snapshot);return true;
}
static std::string item(std::string id,time_t fire,std::string message="Reminder") {
  return "{\"id\":\""+id+"\",\"type\":\"yellow\",\"message\":\""+message+"\",\"time_unix\":"+std::to_string(fire)+"}";
}
static std::string snapshot(std::string items){return "{\"authoritative\":true,\"items\":["+items+"]}";}
int main(){
  begin();assert(!recipeAlertsEnabled());
  std::string a="22222222-2222-2222-2222-222222222222",b="abcdef01-2222-2222-2222-222222222222";
  auto pair=snapshot(item(a,now+10)+","+item(b,now+20));
  assert(feed(pair));assert(pendingRecipeAlertsCount()==0);
  assert(setRecipeAlertsEnabled(true));
  for(int i=0;i<100;++i)assert(feed(pair));
  assert(pendingRecipeAlertsCount()==2);
  assert(feed(snapshot(item(a,now+30,"Updated"))));assert(pendingRecipeAlertsCount()==1);
  now+=11;tick();assert(!isAlertActive());
  auto bad=snapshot(item(a,now+30)+",{\"id\":\"broken\"}");
  assert(!feed(bad));assert(pendingRecipeAlertsCount()==1);
  assert(!feed("{\"authoritative\":true,\"items\":null}"));
  assert(!feed("{\"items\":[]}"));assert(!feed("{\"authoritative\":\"true\",\"items\":[]}"));
  assert(!feed(snapshot(item(a,now)+","+item(a,now))));
  std::string tooMany;for(int i=0;i<13;i++){if(i)tooMany+=",";tooMany+=item(std::to_string(i),now+90);}
  assert(!feed(snapshot(tooMany)));assert(pendingRecipeAlertsCount()==1);
  assert(!feed(snapshot(item(std::string(65,'x'),now))));
  now+=19;tick();assert(isAlertActive() && activeAlertIsRecipe());assert(std::string(activeAlertMsg())=="Updated");
  assert(feed(snapshot(item(a,now))));assert(pendingRecipeAlertsCount()==0);
  dismissActiveAlert();tick();assert(!isAlertActive());
  assert(feed(snapshot(item(a,now))));tick();assert(!isAlertActive());
  Queue rebooted;assert(core::store::getBlob("recipe_alerts","receipts",&rebooted.receipts,sizeof(rebooted.receipts)));
  assert(rebooted.handled(a.c_str()));
  assert(feed(snapshot(item(b,now-61))));tick();assert(!isAlertActive());assert(pendingRecipeAlertsCount()==0);
  assert(feed(snapshot(item(b,now))));tick();assert(activeAlertIsRecipe());
  snoozeActiveAlert();assert(showingSnooze());
  assert(feed(snapshot(item(b,now))));assert(pendingRecipeAlertsCount()==0);
  now+=60;tick();assert(activeAlertIsRecipe());dismissActiveAlert();
  assert(ingestScheduled(Type::Red,now+100,"local",123));
  assert(feed(snapshot(item("next",now+30))));assert(feed(snapshot("")));
  assert(pendingRecipeAlertsCount()==0 && hasPendingAutomationWithServerId(123));
  assert(feed(snapshot(item("disable",now))));tick();assert(activeAlertIsRecipe());
  assert(setRecipeAlertsEnabled(false));assert(!isAlertActive());assert(hasPendingAutomationWithServerId(123));
  now+=100;tick();assert(isAlertActive() && !activeAlertIsRecipe());dismissActiveAlert();
  assert(setRecipeAlertsEnabled(true));
  assert(feed(snapshot(item("store-failure",now))));storageFails=true;tick();assert(!isAlertActive());
  assert(!setRecipeAlertsEnabled(false));assert(recipeAlertsEnabled());storageFails=false;
  // Remote queue capacity cannot consume local alarm slots.
  tooMany.clear();for(int i=0;i<12;i++){if(i)tooMany+=",";tooMany+=item("capacity"+std::to_string(i),now+90);}
  assert(feed(snapshot(tooMany)));assert(pendingRecipeAlertsCount()==12);
  for(int i=0;i<12;i++)assert(ingestScheduled(Type::Red,now+90,"local",100+i));
  assert(pendingAutomationsCount()==12);
  assert(setRecipeAlertsEnabled(false));assert(pendingRecipeAlertsCount()==0 && pendingAutomationsCount()==12);
  std::cout<<"PASS: default off; UUID dedup; reschedule/cancel; malformed/overflow atomicity; active/dismissed/reboot receipts; overdue skip; snooze; opt-out; local alarm isolation; storage failures\n";
}
