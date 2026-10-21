#pragma once
#include "services/countdown.h"
#include <stdint.h>
namespace control {
// UI-task composition boundary shared by local transports and scripting.
// Does not own a hardware session, worker, or a parallel navigation stack.
enum class Operation {
    DeviceState,
    TimerGet,
    TimerStart,
    TimerPause,
    TimerResume,
    TimerCancel,
    VolumeGet,
    VolumeSet,
    NavigationState,
    NavigationOpen,
    NavigationHome
};
struct Descriptor {
    Operation operation;
    const char *name;
    const char *summary;
    bool mutates;
    const char *argument;
    const char *type;
    int minimum, maximum;
    const char *choices;
    const char *luaBinding;
};
const Descriptor *descriptors(unsigned &count);
const Descriptor *find(const char *name);
struct Request {
    Operation operation = Operation::DeviceState;
    uint32_t durationSeconds = 0, expectedRevision = 0;
    bool checkRevision = false;
    int volume = 0;
    const char *route = nullptr; // Borrowed for execute only; never retained.
};
enum class Error {
    None,
    InvalidArgument,
    InvalidState,
    ClockUnsynced,
    QueueFull,
    StorageError,
    StaleState,
    Busy,
    Unsupported
};
struct Result {
    Error error = Error::None;
    services::countdown::Record timer{};
    uint32_t timerRevision = 0;
    int volume = 0;
    const char *app = nullptr; // Static app catalog ID.
};
// Settings volume in this slice is temporary only; persistence is not implied.
Result execute(const Request &request);
const char *errorName(Error error);
} // namespace control
