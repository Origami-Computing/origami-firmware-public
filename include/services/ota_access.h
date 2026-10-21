#pragma once
#include <stddef.h>
namespace services { namespace ota_access {
enum class Role { Unknown, Production, Developer };
constexpr size_t kKeySize = 65;
// NVS credential lifecycle. The caller cancels OTA before replacing the key.
bool read(char (&key)[kKeySize]);
bool store(const char* key);
// Cache only permissions confirmed by the server, bound to the current key.
void confirm(const char* key, Role role);
Role role();
} }
