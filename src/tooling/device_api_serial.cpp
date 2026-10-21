#include "tooling/device_api_serial.h"
#include "control/device_api.h"
#include <ArduinoJson.h>
#include <esp_system.h>
#include <string.h>
namespace tooling {
namespace {
char bootId[9]={};
struct Cached { char id[25]={}, input[320]={}; control::Result result{}; };
Cached cache[4];unsigned nextSlot=0;
void emit(JsonDocument& doc,Print& output) {
  // USB debug logs can interleave even within write(); checksum detects corruption.
  static char buffer[2048];
  const size_t size=measureJson(doc);
  if(size>sizeof(buffer)-17){output.println("[api] {\"status\":\"rejected\",\"error\":\"response_too_large\"}");return;}
  memcpy(buffer,"[api] ",6);size_t length=serializeJson(doc,buffer+6,sizeof(buffer)-16);
  uint32_t hash=2166136261u;
  for(size_t i=0;i<length;++i){hash^=(uint8_t)buffer[6+i];hash*=16777619u;}
  int trailer=snprintf(buffer+6+length,sizeof(buffer)-6-length,"\t%08lx\n",(unsigned long)hash);
  output.write((const uint8_t*)buffer,length+6+trailer);
}
void resultFields(JsonDocument& doc,const control::Result& result) {
  doc["status"]=result.error==control::Error::None?"completed":
    result.error==control::Error::StorageError?"failed":"rejected";
  if(result.error==control::Error::StorageError)doc["may_have_applied"]=true;
  if(result.error!=control::Error::None)doc["error"]=control::errorName(result.error);
  auto data=doc["result"].to<JsonObject>();
  data["app"]=result.app;data["sfx_volume"]=result.volume;
  auto timer=data["timer"].to<JsonObject>();
  const char* states[]={"idle","running","paused","done"};
  timer["state"]=states[(unsigned)result.timer.state];timer["duration_seconds"]=result.timer.duration;
  timer["remaining_seconds"]=result.timer.remaining;timer["revision"]=result.timerRevision;
}
}
bool handleDeviceApi(const char* line,Print& output) {
  if(strncmp(line,"api {",5))return false; // Preserve legacy api status/set provisioning commands.
  if(!bootId[0])snprintf(bootId,sizeof(bootId),"%08lx",(unsigned long)esp_random());
  JsonDocument input,response;response["api"]=1;response["boot_id"]=bootId;
  auto reject=[&](const char* error){response["status"]="rejected";response["error"]=error;emit(response,output);return true;};
  if(strlen(line)>=320 || deserializeJson(input,line+4,DeserializationOption::NestingLimit(4)) || !input.is<JsonObject>())return reject("invalid_request");
  for(JsonPair field:input.as<JsonObject>()) {
    const char* key=field.key().c_str();
    if(strcmp(key,"api")&&strcmp(key,"request_id")&&strcmp(key,"boot_id")&&strcmp(key,"operation")&&strcmp(key,"args")&&strcmp(key,"expected_revision"))return reject("unknown_field");
  }
  if(!input["api"].is<unsigned>() || input["api"].as<unsigned>()!=1)return reject("unsupported_version");
  const char* id=input["request_id"] | "";const char* name=input["operation"] | "";
  if(!id[0] || strlen(id)>24 || !name[0])return reject("invalid_request");
  response["request_id"]=id;
  if(!input["args"].isNull() && !input["args"].is<JsonObject>())return reject("invalid_argument");
  auto args=input["args"].as<JsonObject>();
  if(!strcmp(name,"capabilities.describe")) {
    if(args.size()!=1 || !args["operation"].is<const char*>() || !input["expected_revision"].isNull())return reject("invalid_argument");
    auto* item=control::find(args["operation"]);if(!item)return reject("unsupported");
    response["status"]="completed";auto description=response["result"].to<JsonObject>();
    description["operation"]=item->name;description["description"]=item->summary;
    description["mutates"]=item->mutates;description["calling_task"]="ui";
    description["completion"]="synchronous";
    description["lua_binding"]=item->luaBinding;
    if(item->argument){auto field=description["arguments"][item->argument].to<JsonObject>();
      field["type"]=item->type;field["required"]=true;
      if(item->choices){
        auto choices=field["choices"].to<JsonArray>();
        char values[64];strncpy(values,item->choices,sizeof(values)-1);values[sizeof(values)-1]=0;
        char* context=nullptr;
        for(char* value=strtok_r(values,",",&context);value;value=strtok_r(nullptr,",",&context))choices.add(value);
      }
      else {field["minimum"]=item->minimum;field["maximum"]=item->maximum;}}
    emit(response,output);return true;
  }
  if(!strcmp(name,"capabilities.list")) {
    if(args.size() || !input["expected_revision"].isNull())return reject("invalid_argument");
    response["status"]="completed";auto list=response["result"].to<JsonArray>();unsigned count;
    auto* entries=control::descriptors(count);
    for(unsigned i=0;i<count;++i){auto item=list.add<JsonObject>();item["operation"]=entries[i].name;
      item["description"]=entries[i].summary;item["mutates"]=entries[i].mutates;}
    emit(response,output);return true;
  }
  auto* descriptor=control::find(name);if(!descriptor)return reject("unsupported");
  control::Request request;request.operation=descriptor->operation;
  if(descriptor->mutates) {
    const char* supplied=input["boot_id"] | "";
    if(strcmp(supplied,bootId))return reject("stale_boot");
    for(const auto& entry:cache)if(!strcmp(entry.id,id)) {
      if(strcmp(entry.input,line))return reject("request_id_conflict");
      resultFields(response,entry.result);response["replayed"]=true;emit(response,output);return true;
    }
  }
  if(!input["expected_revision"].isNull()) {
    if(!input["expected_revision"].is<uint32_t>())return reject("invalid_argument");
    request.checkRevision=true;request.expectedRevision=input["expected_revision"];
  }
  using Op=control::Operation;
  if(request.operation==Op::TimerStart) {
    if(args.size()!=1 || !args["duration_seconds"].is<uint32_t>())return reject("invalid_argument");
    request.durationSeconds=args["duration_seconds"];
  } else if(request.operation==Op::VolumeSet) {
    if(args.size()!=1 || !args["value"].is<int>())return reject("invalid_argument");
    request.volume=args["value"];
  } else if(request.operation==Op::NavigationOpen) {
    if(args.size()!=1 || !args["route"].is<const char*>())return reject("invalid_argument");
    request.route=args["route"];
  } else if(args.size())return reject("invalid_argument");
  const auto result=control::execute(request);resultFields(response,result);
  if(descriptor->mutates) {
    auto& entry=cache[nextSlot++%4];strcpy(entry.id,id);strcpy(entry.input,line);entry.result=result;
  }
  emit(response,output);return true;
}
}
