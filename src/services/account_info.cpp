#include "services/account_info.h"
#include "core/secrets.h"
#include "services/tls_trust.h"
#include "hal/hal_wifi.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
namespace services { namespace account_info {
namespace {
portMUX_TYPE s_mux=portMUX_INITIALIZER_UNLOCKED;
Snapshot s_value{};
uint32_t s_revision=0, s_lastAttempt=0, s_attemptRevision=0;
bool s_valid=false, s_attempted=false;
// HTTPClient decodes chunked transfers into this sink. Bound actual bytes as
// well as Content-Length; reverse proxies may omit that header.
class ProfileBody : public Stream {
public:
 String text;
 size_t write(uint8_t byte) override { return write(&byte,1); }
 size_t write(const uint8_t* bytes,size_t size) override {
  if(size>8192u-text.length() || !text.concat(reinterpret_cast<const char*>(bytes),size))return 0;
  return size;
 }
 int available() override {return 0;}
 int read() override {return -1;}
 int peek() override {return -1;}
 void flush() override {}
};
}
Snapshot snapshot() {
 const auto request=core::secrets::backendRequest();
 Snapshot value{};
 portENTER_CRITICAL(&s_mux);
 if(s_valid && request.revision==s_revision)value=s_value;
 portEXIT_CRITICAL(&s_mux);
 if(!request.backend.apiKey[0])strlcpy(value.name,"Not linked",sizeof(value.name));
 else if(!value.name[0])strlcpy(value.name,hal::wifi::connected()?"Checking...":"Offline",sizeof(value.name));
 return value;
}
void poll() {
 const auto request=core::secrets::backendRequest();
 if(!request.backend.apiKey[0])return;
 if(s_attempted && s_attemptRevision==request.revision && uint32_t(millis()-s_lastAttempt)<60000u)return;
 s_attempted=true;s_attemptRevision=request.revision;s_lastAttempt=millis();
 WiFiClientSecure secure;WiFiClient plain;
 String url=String(request.backend.url)+"/api/profile";
 const bool https=url.startsWith("https://");
 if(https)tls::useRootBundle(secure);
 HTTPClient http;http.setConnectTimeout(5000);http.setTimeout(5000);
 if(!http.begin(https?static_cast<WiFiClient&>(secure):plain,url))return;
 http.addHeader("X-Api-Key",request.backend.apiKey);
 const int code=http.GET();
 Serial.printf("[account] refresh HTTP %d content_length=%d\n",code,http.getSize());
 Snapshot next{};
 if(code==200 && http.getSize()<=8192){
  JsonDocument filter,doc;filter["name"]=true;filter["is_anonymous"]=true;filter["phone"]=true;
  ProfileBody body;
  const int received=http.writeToStream(&body);
  if(received>=0 && !deserializeJson(doc,body.text.c_str(),DeserializationOption::Filter(filter)) && doc.is<JsonObject>()){
   const char* name=doc["name"]|"";
   const char* phone=doc["phone"]|"";
   strlcpy(next.name,doc["is_anonymous"].as<bool>()?"Guest account":name[0]?name:phone[0]?phone:"Linked (unnamed)",sizeof(next.name));
  }
 } else if(code==401 || code==404)strlcpy(next.name,"Not linked",sizeof(next.name));
 http.end();
 if(!next.name[0])strlcpy(next.name,"Unavailable",sizeof(next.name));
 portENTER_CRITICAL(&s_mux);
 s_value=next;s_revision=request.revision;s_valid=true;
 portEXIT_CRITICAL(&s_mux);
}
}}
