#include "control/shipping.h"
#include "services/shipping.h"
#include "services/manufacturing.h"
#include "services/manufacturing_access.h"
#include "services/automation.h"
#include "nav_handler.h"
#include "core/watchdog.h"
#include "hal/hal_input.h"
namespace control {
bool enterShipping(bool freshTutorial) {
    auto& nav=NavHandler::instance();
    if(services::shipping::state()!=services::shipping::State::Normal ||
       services::automation::wantsInputCapture() || !nav.runtimePolicy().allowPowerSleep) return false;
    App* root=nav.root();
    if(!root) return false;
    core::watchdog::unsubscribe();
    nav.shutdown();
    core::watchdog::subscribe();
    // Tutorial exit callbacks may save progress. Reset only after teardown,
    // and do not boot normal apps again between this write and shipping lock.
    services::manufacturing_access::session().close();
    if((!freshTutorial || services::manufacturing::resetTutorial()) && services::shipping::enter()) return true;
    nav.init(root); nav.armInputBlock(400); hal::input::suppressUntilButtonsReleased();
    return false;
}
}
