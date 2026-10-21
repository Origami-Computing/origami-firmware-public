#include "core/store.h"
#include <Preferences.h>

namespace core { namespace store {

uint32_t getU32(const char* ns, const char* key, uint32_t def) {
    Preferences p;
    if (!p.begin(ns, false)) return def;
    auto v = p.getUInt(key, def);
    p.end();
    return v;
}

bool putU32(const char* ns, const char* key, uint32_t val) {
    Preferences p;
    if (!p.begin(ns, false)) return false;
    const bool ok = p.putUInt(key, val) == sizeof(val);
    p.end();
    return ok;
}

uint16_t getU16(const char* ns, const char* key, uint16_t def) {
    Preferences p;
    if (!p.begin(ns, false)) return def;
    auto v = p.getUShort(key, def);
    p.end();
    return v;
}

bool putU16(const char* ns, const char* key, uint16_t val) {
    Preferences p;
    if (!p.begin(ns, false)) return false;
    const bool ok = p.putUShort(key, val) == sizeof(val);
    p.end();
    return ok;
}

uint8_t getU8(const char* ns, const char* key, uint8_t def) {
    Preferences p;
    if (!p.begin(ns, false)) return def;
    auto v = p.getUChar(key, def);
    p.end();
    return v;
}

bool putU8(const char* ns, const char* key, uint8_t val) {
    Preferences p;
    if (!p.begin(ns, false)) return false;
    const bool ok = p.putUChar(key, val) == sizeof(val);
    p.end();
    return ok;
}

bool getBool(const char* ns, const char* key, bool def) {
    Preferences p;
    if (!p.begin(ns, false)) return def;
    auto v = p.getBool(key, def);
    p.end();
    return v;
}

bool putBool(const char* ns, const char* key, bool val) {
    Preferences p;
    if (!p.begin(ns, false)) return false;
    const bool ok = p.putBool(key, val) == sizeof(val);
    p.end();
    return ok;
}

bool getBlob(const char* ns, const char* key, void* data, size_t len) {
    if (!data || len == 0) return false;
    Preferences p;
    if (!p.begin(ns, true)) return false;
    const bool ok = p.getBytesLength(key) == len && p.getBytes(key, data, len) == len;
    p.end();
    return ok;
}

bool putBlob(const char* ns, const char* key, const void* data, size_t len) {
    if (!data || len == 0) return false;
    Preferences p;
    if (!p.begin(ns, false)) return false;
    const bool ok = p.putBytes(key, data, len) == len;
    p.end();
    return ok;
}

size_t freeEntries(const char* ns) {
    Preferences p;
    if (!p.begin(ns, false)) return 0;
    const size_t entries = p.freeEntries();
    p.end();
    return entries;
}

}}  // namespace core::store
