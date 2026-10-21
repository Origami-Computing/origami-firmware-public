#pragma once
#include <stddef.h>
#include <stdint.h>

namespace core { namespace store {
    // Writes return true only when Preferences reports the complete value saved.
    uint32_t getU32(const char* ns, const char* key, uint32_t def = 0);
    bool     putU32(const char* ns, const char* key, uint32_t val);
    uint16_t getU16(const char* ns, const char* key, uint16_t def = 0);
    bool     putU16(const char* ns, const char* key, uint16_t val);
    uint8_t  getU8 (const char* ns, const char* key, uint8_t  def = 0);
    bool     putU8 (const char* ns, const char* key, uint8_t  val);
    bool     getBool(const char* ns, const char* key, bool def = false);
    bool     putBool(const char* ns, const char* key, bool val);
    bool     getBlob(const char* ns, const char* key, void* data, size_t len);
    bool     putBlob(const char* ns, const char* key, const void* data, size_t len);
    size_t   freeEntries(const char* ns);
}}
