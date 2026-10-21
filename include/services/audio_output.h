#pragma once
#include "hal/hal_audio.h"

namespace services { namespace audio_output {

// UI-task confirmed preference selection. Preserve the Settings contract:
// apply immediately, then persist/apply through the HAL's existing path.
// Storage is best effort; this void operation does not certify durable saving.
// Temporary session/diagnostic overrides must not use this persistent operation.
void selectAndSave(hal::audio::OutputRoute route);

} }
