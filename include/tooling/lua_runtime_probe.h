#pragma once
class Print;
namespace tooling {
// Fixed, read-only Lua runtime diagnostics on the UI task; no arbitrary eval.
void probeLuaRuntime(Print& output);
// Fixed timer transition probe; refuses active timers and restores idle setup.
void probeLuaTimer(Print& output);
}
