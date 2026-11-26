#include "services/agent_speech.h"
#include "services/wav_player.h"
#include "services/agent_voice.h"
#include "services/agent_latency.h"
#include "core/memory.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include <cstring>
#include <algorithm>
namespace services { namespace agent_speech {
namespace {
audio::WavPlayer player;
constexpr uint32_t capacity=48000; // two seconds mono, PSRAM only
int16_t* pending=nullptr;
uint32_t readAt=0,writeAt=0,queued=0,lastData=0,drainAt=0,sequence=0;
bool allowed=false,active=false,started=false,finished=false,ending=false;
void clear(){readAt=writeAt=queued=0;active=started=finished=ending=false;drainAt=0;}
}
bool enabled(){return allowed;}
bool busy(){return active || player.sessionOpen();}
void stop(){clear();player.closeSession();void* storage=pending;core::memory::freePsram(storage);pending=nullptr;}
void setEnabled(bool value){allowed=value;if(!value)stop();}
void receive(const char* json){
    JsonDocument doc;if(deserializeJson(doc,json))return;
    const char* type=doc["type"]|"";
    if(!strcmp(type,"speech_start")){
        stop();if(!allowed || agent_voice::isCapturing())return;
        pending=static_cast<int16_t*>(core::memory::allocPsram("agent.speech",capacity*sizeof(int16_t)));
        if(!pending)return;
        sequence=0;active=true;lastData=millis();return;
    }
    if(!active)return;
    if(!strcmp(type,"speech_end")){finished=true;lastData=millis();return;}
    if(!strcmp(type,"speech_error")){stop();return;}
    if(strcmp(type,"speech_data"))return;
    if((doc["sequence"]|UINT32_MAX)!=sequence++){stop();return;}
    const char* encoded=doc["pcm"]|"";
    int16_t decoded[1024];size_t bytes=0;
    if(mbedtls_base64_decode(reinterpret_cast<unsigned char*>(decoded),sizeof(decoded),&bytes,
       reinterpret_cast<const unsigned char*>(encoded),strlen(encoded)) || bytes%2 || bytes/2>capacity-queued){stop();return;}
    for(size_t i=0;i<bytes/2;++i){pending[writeAt]=decoded[i];writeAt=(writeAt+1)%capacity;}
    queued+=bytes/2;lastData=millis();
}
void tick(uint32_t now){
    if(!active)return;
    if(agent_voice::isCapturing() || static_cast<int32_t>(now-lastData)>15000){stop();return;}
    if(!started && (queued>=2400 || (finished&&queued))){
        if(!player.openSession() || !player.startPcmStream(24000)){stop();return;}
        started=true;agent_latency::milestone("speech");
        Serial.println("[agent-speech] playback started");
    }
    if(started && queued){
        const auto count=std::min(queued,capacity-readAt);
        const auto accepted=player.appendPcmStream(pending+readAt,count);
        readAt=(readAt+accepted)%capacity;queued-=accepted;
    }
    if(finished && !queued){
        if(!started){stop();return;}
        if(!ending)ending=player.finishPcmStream();
        if(ending && player.consumeWantsNextTrack() && !drainAt)drainAt=now;
        if(drainAt && now-drainAt>=120u){Serial.println("[agent-speech] playback complete");stop();}
    }
}
} }
