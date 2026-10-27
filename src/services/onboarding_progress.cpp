#include "services/onboarding_progress.h"
#include "services/onboarding_legacy_progress.h"
#include "core/store.h"
#include <Preferences.h>

namespace services { namespace onboarding_progress {
namespace {
constexpr uint32_t kTag = 0x4B050000;
constexpr const char* kNamespace = "onboarding";
constexpr const char* kKey = "guide_v5";
uint32_t s_saved = 0;
}
Progress load() {
    Progress progress;
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return progress;
    uint32_t word = prefs.getUInt(kKey, 0);
    const uint32_t previous = prefs.getUInt("guide_v4", 0), oldWord = prefs.getUInt("guide_v3", 0);
    if (!word && (previous & 0xFFFFFE00u) == 0x4B040000u) {
        word = kTag | (previous & 0x1FFu);
        if (!(previous & 0x100u) && (previous & 255u) >= 21) word = kTag | 24u;
    }
    progress.offered = prefs.getBool("offered", false) || word != 0 || oldWord != 0;
    prefs.end(); s_saved = word;
    if ((word & 0xFFFFFE00u) == kTag && (word & 255) <= 34) {
        progress.completed = (word & 0x100) != 0; progress.page = word & 255;
        if (progress.page >= 29 && progress.page <= 31) progress.page = 29;
        if (progress.page == 34) progress.page = 22;
        if (progress.page >= 12 && progress.page <= 15) progress.page = 9;
        if (progress.page == 19 || progress.page == 20) progress.page = 18;
        if (progress.page == 26 || progress.page == 27) progress.page = 25;
        if (progress.page == 28) progress.page = 9;
        if (progress.page == 23) progress.page = 22;
    } else {
        progress.completed = oldWord && !legacy::shouldOffer(legacy::decode(oldWord));
    }
    return progress;
}
bool markOffered() { return core::store::putBool(kNamespace, "offered", true); }
bool save(unsigned page, bool completed) {
    if(page>34)return false;
    if(page==34)page=22;
    if (page >= 29 && page <= 31) page = 29;
    if (page == 12) page = 9;
    if (page == 19) page = 18;
    if (page == 23) page = 22;
    if (page == 26 || page == 27) page = 25;
    const uint32_t word = kTag | (completed ? 0x100u : 0u) | page;
    if (word == s_saved) return true;
    if (!core::store::putU32(kNamespace, kKey, word)) return false;
    s_saved = word;
    return true;
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool resetForDevelopment() { return core::store::putU32(kNamespace, kKey, 0) && core::store::putU32(kNamespace, "guide_v4", 0) && core::store::putU32(kNamespace, "guide_v3", 0) && core::store::putBool(kNamespace, "offered", false); }
uint32_t savedRecordForDevelopment() { return s_saved; }
#endif
}} // namespace services::onboarding_progress
