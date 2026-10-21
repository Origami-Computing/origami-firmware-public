#include "control/device_handoff.h"
#include "services/device_handoff.h"
#include "services/automation.h"
#include "nav_handler.h"
#include "core/watchdog.h"
#include "hal/hal_input.h"
namespace control {
bool scrubDeviceForHandoff() {
    auto& nav=NavHandler::instance();
    if(services::automation::wantsInputCapture()||!nav.root())return false;
    App* root=nav.root();
    core::watchdog::unsubscribe();nav.shutdown();core::watchdog::subscribe();
    if(services::device_handoff::scrubAndRestart())return true;
    nav.init(root);nav.armInputBlock(400);hal::input::suppressUntilButtonsReleased();
    return false;
}
}
