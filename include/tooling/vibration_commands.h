#pragma once
namespace tooling { namespace vibration_commands {
// The caller supplies current alert policy. Preview requests are denied while
// an alert is busy; inspection and custom-pattern editing remain available.
bool handleCommand(const char* line, bool alertBusy);
} }
