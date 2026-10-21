#include "control/device_api.h"
#include "apps/menu_app.h"
#include "apps/settings_menu_app.h"
#include "apps/timer_app.h"
#include "nav_handler.h"
#include "services/agent_coordination.h"
#include "services/agent_voice.h"
#include "services/automation.h"
#include "sfx/sfx.h"
#include <string.h>
namespace control {
namespace {
Error convert(services::countdown::Result value) {
    using R = services::countdown::Result;
    switch (value) {
    case R::Ok:
        return Error::None;
    case R::InvalidArgument:
        return Error::InvalidArgument;
    case R::InvalidState:
        return Error::InvalidState;
    case R::ClockUnsynced:
        return Error::ClockUnsynced;
    case R::QueueFull:
        return Error::QueueFull;
    case R::StorageError:
        return Error::StorageError;
    }
    return Error::InvalidState;
}
bool timerMutation(Operation op) {
    return op >= Operation::TimerStart && op <= Operation::TimerCancel;
}
} // namespace
Result execute(const Request &request) {
    Result result;
    result.timer = services::countdown::snapshot();
    auto &nav = NavHandler::instance();
    if (request.checkRevision && (!timerMutation(request.operation) ||
                                  request.expectedRevision != services::countdown::revision()))
        result.error = Error::StaleState;
    if (result.error == Error::None)
        switch (request.operation) {
        case Operation::TimerStart:
            result.error = convert(services::countdown::start(
                request.durationSeconds, result.timer.sound, result.timer.vibration));
            break;
        case Operation::TimerPause:
            result.error = convert(services::countdown::pause());
            break;
        case Operation::TimerResume:
            result.error = convert(services::countdown::resume());
            break;
        case Operation::TimerCancel:
            result.error = convert(services::countdown::reset());
            break;
        case Operation::VolumeSet:
            if (request.volume < 0 || request.volume > 100)
                result.error = Error::InvalidArgument;
            else
                sfx::setVolume(request.volume);
            break;
        case Operation::NavigationOpen:
        case Operation::NavigationHome: {
            if (nav.systemScreenActive() || services::automation::wantsInputCapture() ||
                services::agent_voice::isCapturing() ||
                services::agent_coordination::isSessionActive()) {
                result.error = Error::Busy;
                break;
            }
            // First slice exposes only ordinary app-to-app transitions. Do not interrupt
            // capture, onboarding, provisioning, or another privileged workflow.
            auto *top = nav.top();
            if (!top || (top != nav.root() && top != &apps::menu::app && top != &apps::timer::app &&
                         top != &apps::settings_menu::app)) {
                result.error = Error::Busy;
                break;
            }
            if (request.operation == Operation::NavigationHome) {
                nav.popToRoot();
                break;
            }
            App *target = nullptr;
            if (request.route && !strcmp(request.route, "timer"))
                target = &apps::timer::app;
            else if (request.route && !strcmp(request.route, "settings"))
                target = &apps::settings_menu::app;
            else if (request.route && !strcmp(request.route, "menu"))
                target = &apps::menu::app;
            if (!target)
                result.error = Error::InvalidArgument;
            else if (!nav.open(target))
                result.error = Error::Busy;
            break;
        }
        case Operation::DeviceState:
        case Operation::TimerGet:
        case Operation::VolumeGet:
        case Operation::NavigationState:
            break;
        default:
            result.error = Error::Unsupported;
            break;
        }
    result.timer = services::countdown::snapshot();
    result.timerRevision = services::countdown::revision();
    result.volume = sfx::volume();
    result.app = nav.top() ? nav.top()->id : "unavailable";
    return result;
}
const char *errorName(Error value) {
    switch (value) {
    case Error::None:
        return "none";
    case Error::InvalidArgument:
        return "invalid_argument";
    case Error::InvalidState:
        return "invalid_state";
    case Error::ClockUnsynced:
        return "clock_unsynced";
    case Error::QueueFull:
        return "queue_full";
    case Error::StorageError:
        return "storage_error";
    case Error::StaleState:
        return "stale_state";
    case Error::Busy:
        return "busy";
    case Error::Unsupported:
        return "unsupported";
    }
    return "unsupported";
}
} // namespace control
