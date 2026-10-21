#pragma once
#ifdef ORIGAMI_DEV_SERIAL_INPUT
namespace tooling {
// Owns diagnostic transport pauses and regression checks; UI-only commands
// continue through the Morning app. Returns the existing command result.
bool morningUiCommand(const char* command);
}
#endif
