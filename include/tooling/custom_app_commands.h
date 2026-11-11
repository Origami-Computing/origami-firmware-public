#pragma once
#include <Arduino.h>
namespace tooling {
// UI-task adapters only. Mutations queue the existing widget worker; serial
// buffers never reference worker-owned memory. No arbitrary flash/file writes.
bool handleCustomAppCommand(const char* line,Print& output);
void serviceCustomAppCommands(uint32_t now);
}
