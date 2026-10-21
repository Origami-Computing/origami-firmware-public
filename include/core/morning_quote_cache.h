#pragma once
#include "core/morning_quote_record.h"

namespace core { namespace morning_quote_record {
// Update an existing day in place; otherwise retain the newer of two records.
inline int replacementSlot(const Record (&records)[2], const bool (&valid)[2],
                           uint32_t day) {
  for(int i=0;i<2;++i) if(valid[i] && records[i].dayKey==day) return i;
  if(!valid[0]) return 0;
  if(!valid[1]) return 1;
  return records[0].dayKey<=records[1].dayKey?0:1;
}
} }
