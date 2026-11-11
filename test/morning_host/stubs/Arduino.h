#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint32_t millis();
struct TestSerial { void println(const char*) {} template <class... T> void printf(const char*, T...) {} };
extern TestSerial Serial;
inline size_t strlcpy(char* dest, const char* src, size_t cap) {
  const size_t size = strlen(src);
  if (cap) { const size_t n = size < cap - 1 ? size : cap - 1; memcpy(dest, src, n); dest[n] = 0; }
  return size;
}
