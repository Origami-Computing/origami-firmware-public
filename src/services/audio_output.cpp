#include "services/audio_output.h"

namespace services { namespace audio_output {
void selectAndSave(hal::audio::OutputRoute route) {
    // Do not collapse these calls: lazy loading, physical application order and
    // storage failure behavior are part of the existing confirmation contract.
    hal::audio::setOutputRoute(route);
    hal::audio::outputRouteSaveToNvs(route);
}
} }
