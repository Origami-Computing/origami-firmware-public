#include "control/device_api.h"
#include <string.h>

namespace control {
namespace {
const Descriptor kOperations[] = {
    {Operation::DeviceState, "device.state", "Timer, SFX volume and current app", false, nullptr,
     nullptr, 0, 0, nullptr, "kami.device.snapshot"},
    {Operation::TimerGet, "timer.get", "Read the single device countdown", false, nullptr, nullptr,
     0, 0, nullptr, "kami.timer.get"},
    {Operation::TimerStart, "timer.start", "Start a 5..5995 second countdown", true,
     "duration_seconds", "integer", 5, 5995, nullptr, "kami.timer.start"},
    {Operation::TimerPause, "timer.pause", "Pause the device countdown", true, nullptr, nullptr, 0,
     0, nullptr, "kami.timer.pause"},
    {Operation::TimerResume, "timer.resume", "Resume the paused countdown", true, nullptr, nullptr,
     0, 0, nullptr, "kami.timer.resume"},
    {Operation::TimerCancel, "timer.cancel", "Reset the countdown; active alert unchanged", true,
     nullptr, nullptr, 0, 0, nullptr, "kami.timer.cancel"},
    {Operation::VolumeGet, "settings.sfx_volume.get", "Read the 0..100 UI volume", false, nullptr,
     nullptr, 0, 0, nullptr, nullptr},
    {Operation::VolumeSet, "settings.sfx_volume.set", "Set temporary 0..100 UI volume", true,
     "value", "integer", 0, 100, nullptr, nullptr},
    {Operation::NavigationState, "navigation.state", "Read current app", false, nullptr, nullptr, 0,
     0, nullptr, nullptr},
    {Operation::NavigationOpen, "navigation.open", "Open timer, settings or menu", true, "route",
     "enum", 0, 0, "timer,settings,menu", nullptr},
    {Operation::NavigationHome, "navigation.home", "Return home through normal teardown", true,
     nullptr, nullptr, 0, 0, nullptr, nullptr},
};
}
const Descriptor *descriptors(unsigned &count) {
    count = sizeof(kOperations) / sizeof(kOperations[0]);
    return kOperations;
}
const Descriptor *find(const char *name) {
    if (!name)
        return nullptr;
    unsigned count;
    auto *list = descriptors(count);
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(name, list[i].name))
            return list + i;
    return nullptr;
}
} // namespace control
