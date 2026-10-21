#pragma once

#include <stdint.h>

#include "ui/scene.h"

class Print;

namespace tooling {
namespace preview {

void dumpApps(Print& out);
bool dumpScreens(Print& out, const char* appId);
bool dumpInputs(Print& out, const char* appId, const char* screenId);
bool buildScene(const char* appId, const char* screenId, ui::scene::Scene& scene,
                int16_t width, int16_t height);

}  // namespace preview
}  // namespace tooling
