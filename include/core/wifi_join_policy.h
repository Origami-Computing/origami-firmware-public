#pragma once
#include <stdint.h>
#include <string.h>

namespace core { namespace wifi_join {
constexpr uint32_t kDeadlineMs = 20000;
enum class Result { Waiting, Joined, TimedOut };
inline Result assess(uint32_t now, uint32_t started, const char* target,
                     const char* associated, bool connected, uint32_t ip) {
  if ((uint32_t)(now - started) >= kDeadlineMs) return Result::TimedOut;
  if (target && target[0] && associated && strcmp(target, associated) == 0 && connected && ip != 0)
    return Result::Joined;
  return Result::Waiting;
}
} }
