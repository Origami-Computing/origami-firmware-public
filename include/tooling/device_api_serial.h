#pragma once
#include <Arduino.h>
namespace tooling {
// UI-task only. Recognizes `api {JSON}`; leaves other diagnostics untouched.
bool handleDeviceApi(const char* line, Print& output);
}
