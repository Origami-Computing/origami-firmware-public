#include "services/ota_access.h"
#include <Preferences.h>
#include <cstring>
namespace services { namespace ota_access {
namespace {
char confirmedKey[kKeySize] = {};
Role confirmedRole = Role::Unknown;
bool valid(const char* key) {
    if (!key || strlen(key) != kKeySize - 1) return false;
    for (const char* c = key; *c; ++c)
        if (!(*c >= '0' && *c <= '9') && !(*c >= 'a' && *c <= 'f')) return false;
    return true;
}
}
bool read(char (&key)[kKeySize]) {
    key[0] = 0;
    Preferences prefs;
    // Shares the factory-reset namespace; no baked fallback or persisted role.
    if (!prefs.begin("secrets", true)) return false;
    // Unprovisioned devices are normal. Avoid Preferences' error log on every
    // menu availability query without caching credentials or changing NVS.
    if (!prefs.isKey("ota_access")) { prefs.end(); return false; }
    const auto value = prefs.getString("ota_access", "");
    prefs.end();
    if (!valid(value.c_str())) return false;
    memcpy(key, value.c_str(), kKeySize);
    return true;
}
bool store(const char* key) {
    if (!key || (*key && !valid(key))) return false;
    Preferences prefs;
    if (!prefs.begin("secrets", false)) return false;
    const bool ok = *key ? prefs.putString("ota_access", key) == kKeySize - 1
                        : !prefs.isKey("ota_access") || prefs.remove("ota_access");
    prefs.end();
    if (ok) { memset(confirmedKey, 0, sizeof(confirmedKey)); confirmedRole = Role::Unknown; }
    return ok;
}
void confirm(const char* key, Role role) {
    memset(confirmedKey, 0, sizeof(confirmedKey));
    confirmedRole = Role::Unknown;
    char current[kKeySize];
    if (key && read(current) && strcmp(current, key) == 0) {
        memcpy(confirmedKey, current, kKeySize);
        confirmedRole = role;
    }
}
Role role() {
    char current[kKeySize];
    return read(current) && strcmp(current, confirmedKey) == 0 ? confirmedRole : Role::Unknown;
}
} }
