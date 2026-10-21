#include "ui/direct_bands.h"

namespace ui { namespace detail {
// UI-task scratch storage, borrowed only for one synchronous draw. Every paint
// callback must cover its requested band; no Scene may retain these pixels.
TFT_eSprite* directBandSprite() {
    static TFT_eSprite band(&hal::display::directTft());
    if (!band.created()) {
        band.setColorDepth(16);
        band.createSprite(280, 32);
    }
    return band.created() ? &band : nullptr;
}
} }
