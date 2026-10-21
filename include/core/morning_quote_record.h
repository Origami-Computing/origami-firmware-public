#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace core {
namespace morning_quote_record {

static constexpr uint32_t kMagic = 0x4D515431u;  // "MQT1"
static constexpr uint16_t kVersion = 1;
static constexpr uint16_t kRecordBytes = 320;

enum class Kind : uint8_t { Philosophical = 0, Motivational = 1 };

#pragma pack(push, 1)
struct Record {
  uint32_t magic = kMagic;
  uint16_t version = kVersion;
  uint16_t totalSize = kRecordBytes;
  uint32_t dayKey = 0;  // local YYYYMMDD
  Kind kind = Kind::Philosophical;
  uint8_t quoteLen = 0;
  uint8_t attributionLen = 0;
  uint8_t idLen = 0;
  char quote[224] = {};
  char attribution[48] = {};
  char backendId[32] = {};
};
#pragma pack(pop)

static_assert(sizeof(Record) == kRecordBytes, "Morning quote NVS budget changed");

inline bool printableAscii(const char* value, size_t len) {
  if (!value || len == 0) return false;
  for (size_t i = 0; i < len; ++i) {
    const unsigned char c = (unsigned char)value[i];
    if (c < 0x20 || c > 0x7E) return false;
  }
  return true;
}

inline bool valid(const Record& record) {
  if (record.magic != kMagic || record.version != kVersion ||
      record.totalSize != kRecordBytes || record.dayKey < 20000101u ||
      record.dayKey > 29991231u) return false;
  if (record.kind != Kind::Philosophical && record.kind != Kind::Motivational) return false;
  if (record.quoteLen < 12 || record.quoteLen >= sizeof(record.quote) ||
      record.attributionLen == 0 || record.attributionLen >= sizeof(record.attribution) ||
      record.idLen >= sizeof(record.backendId)) return false;
  if (record.quote[record.quoteLen] != '\0' ||
      record.attribution[record.attributionLen] != '\0' ||
      record.backendId[record.idLen] != '\0') return false;
  return printableAscii(record.quote, record.quoteLen) &&
         printableAscii(record.attribution, record.attributionLen) &&
         (record.idLen == 0 || printableAscii(record.backendId, record.idLen));
}

inline bool matchesDay(const Record& record, uint32_t dayKey) {
  return valid(record) && record.dayKey == dayKey;
}

}  // namespace morning_quote_record
}  // namespace core
