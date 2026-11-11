#include "services/onboarding_progress.h"
#include "services/onboarding_legacy_progress.h"
#include "core/store.h"
#include <Preferences.h>

namespace services { namespace onboarding_progress {
namespace {
constexpr uint32_t kTag = 0x4B070000;
constexpr const char* kNamespace = "onboarding";
constexpr const char* kKey = "guide_v7";
uint32_t s_saved = 0;
}
Progress load() {
    Progress progress; Preferences prefs;
    if (!prefs.begin(kNamespace,true)) return progress;
    const uint32_t word=prefs.getUInt(kKey,0);
    const uint32_t v6=prefs.getUInt("guide_v6",0);
    const uint32_t old=prefs.getUInt("guide_v5",0),v4=prefs.getUInt("guide_v4",0),v3=prefs.getUInt("guide_v3",0);
    progress.offered=prefs.getBool("offered",false)||word||v6||old||v4||v3;
    prefs.end();s_saved=word;
    if((word&0xFFFF8000u)==kTag && (word&255u)<=141 && (word&255u)!=44){
      progress.page=word&255;progress.completed=word&0x100;
      progress.interest=(word>>9)&3; if(progress.interest>2)progress.interest=0;
      progress.account=word&0x800;progress.joinWifi=word&0x1000;progress.setupPublished=word&0x2000;progress.connected=word&0x4000;
    }else{
      // Old incomplete routes restart with the new controls; completed users stay done.
      progress.completed=(((v6&0xffffe000u)==0x4b060000u)&&(v6&0x100))||(((old&0xfffffe00u)==0x4b050000u)&&(old&0x100))||(((v4&0xfffffe00u)==0x4b040000u)&&(v4&0x100))||(v3&&!legacy::shouldOffer(legacy::decode(v3)));
    }
    return progress;
}
bool markOffered() { return core::store::putBool(kNamespace, "offered", true); }
bool save(unsigned page,bool completed,unsigned interest,bool account,bool joinWifi,bool setupPublished,bool connected) {
    if(page>141||page==44||interest>2)return false;
    if(page==12)page=setupPublished||account&&interest?40:41;
    if(page==19||page==20)page=18;
    if(page==23)page=22;
    if(page==27)page=25;
    if(page==30||page==31||page==43)page=40;
    if(page>=129&&page<=134)page=128;
    if(page==124)page=123;
    const uint32_t word=kTag|(completed?0x100u:0u)|(interest<<9)|(account?0x800u:0u)|(joinWifi?0x1000u:0u)|(setupPublished?0x2000u:0u)|(connected?0x4000u:0u)|page;
    if(word==s_saved)return true;
    if(!core::store::putU32(kNamespace,kKey,word))return false;
    s_saved=word;return true;
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool resetForDevelopment() { return core::store::putU32(kNamespace, kKey, 0) && core::store::putU32(kNamespace, "guide_v6", 0) && core::store::putU32(kNamespace, "guide_v5", 0) && core::store::putU32(kNamespace, "guide_v4", 0) && core::store::putU32(kNamespace, "guide_v3", 0) && core::store::putBool(kNamespace, "offered", false); }
uint32_t savedRecordForDevelopment() { return s_saved; }
#endif
}} // namespace services::onboarding_progress
