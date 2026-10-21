#pragma once

#include "sfx/sfx.h"

// Sound presets. Grouped by domain (shell + per-app) so adding a new sound
// next to its usage stays cheap. Tweak frequencies/envelopes here without
// chasing call sites.

namespace sfx {

// Approved battery cues; same recipes as the computer auditions.
inline constexpr SfxDef kSfxLowBatteryTension = {
    SFX_SINE, 880.f, 587.f, 0.48f, 0.42f, 0.06f, 0.30f, 622.f, 587.f
};
inline constexpr SfxDef kSfxPluggedIn = {
    SFX_SINE, 880.f, 1760.f, 0.24f, 0.40f, 0.03f, 0.48f, 1320.f, 1760.f
};

// ---- Shell / system ---------------------------------------------------------

/** Boot or wake from sleep: 3-note rising arpeggio C5 → E5 → G5. */
inline constexpr SfxDef kSfxBoot = {
    SFX_SQUARE, 523.f, 523.f, 0.25f, 0.5f, 0.02f, 0.25f, 659.f, 784.f
};
/** Boot logo letter reveal — short rising blip per BMP frame. */
inline constexpr SfxDef kSfxBootLogoTick = {
    SFX_SQUARE, 660.f, 990.f, 0.045f, 0.4f, 0.02f, 0.35f, 0.f, 0.f
};
/** Boot "Presents" caption — soft two-note chime. */
inline constexpr SfxDef kSfxBootPresents = {
    SFX_SQUARE, 523.f, 659.f, 0.12f, 0.45f, 0.03f, 0.3f, 784.f, 0.f
};
/** Boot eye morph landing — bright pop before Kami eyes. */
inline constexpr SfxDef kSfxBootEyeLand = {
    SFX_SQUARE, 880.f, 440.f, 0.08f, 0.5f, 0.01f, 0.4f, 0.f, 0.f
};
/** App-list rotary detent — tiny bright pip. */
inline constexpr SfxDef kSfxMenuTick = {
    SFX_SQUARE, 1100.f, 1100.f, 0.018f, 0.28f, 0.04f, 0.45f, 0.f, 0.f
};
/** Launching an app — short rising chirp. */
inline constexpr SfxDef kSfxOpen = {
    SFX_SQUARE, 880.f, 1760.f, 0.05f, 0.45f, 0.02f, 0.3f, 0.f, 0.f
};
/** Returning to the app list — short falling chirp. */
inline constexpr SfxDef kSfxBack = {
    SFX_SQUARE, 1200.f, 600.f, 0.05f, 0.4f, 0.02f, 0.4f, 0.f, 0.f
};

// ---- Alarms ----------------------------------------------------------------

inline constexpr SfxDef kSfxAlarmNavigate = {
    SFX_SQUARE, 1100.f, 1100.f, 0.018f, 0.28f, 0.04f, 0.45f, 0.f, 0.f
};
inline constexpr SfxDef kSfxAlarmOpen = {
    SFX_SQUARE, 660.f, 990.f, 0.055f, 0.42f, 0.03f, 0.35f, 0.f, 0.f
};
inline constexpr SfxDef kSfxAlarmAdd = {
    SFX_SQUARE, 523.f, 523.f, 0.11f, 0.45f, 0.03f, 0.30f, 659.f, 784.f
};
inline constexpr SfxDef kSfxAlarmOn = {
    SFX_SQUARE, 700.f, 1100.f, 0.07f, 0.42f, 0.03f, 0.30f, 0.f, 0.f
};
inline constexpr SfxDef kSfxAlarmOff = {
    SFX_SQUARE, 900.f, 420.f, 0.07f, 0.38f, 0.03f, 0.40f, 0.f, 0.f
};
inline constexpr SfxDef kSfxAlarmSave = {
    SFX_SQUARE, 784.f, 784.f, 0.12f, 0.45f, 0.03f, 0.30f, 988.f, 0.f
};
inline constexpr SfxDef kSfxAlarmError = {
    SFX_SAW, 220.f, 140.f, 0.08f, 0.35f, 0.02f, 0.50f, 0.f, 0.f
};
// Morning alarm motifs deliberately stay on the compact staging synth path:
// square waves, three consonant pitches, modest level, and generous silence
// between repeats. The 880-1568 Hz fundamentals avoid asking the small speaker
// to reproduce the low, full-bodied PCM timbres that sounded thin on hardware.
// Attack and decay are fractions of each definition's duration.
inline constexpr SfxDef kSfxAlarmClassicPreview = {
    SFX_SQUARE, 988.f, 988.f, 0.48f, 0.34f, 0.07f, 0.34f, 1175.f, 1480.f
};
inline constexpr SfxDef kSfxAlarmChimePreview = {
    SFX_SQUARE, 880.f, 880.f, 0.46f, 0.34f, 0.07f, 0.34f, 1109.f, 1320.f
};
inline constexpr SfxDef kSfxAlarmDigitalPreview = {
    SFX_SQUARE, 1047.f, 1047.f, 0.42f, 0.33f, 0.06f, 0.32f, 1175.f, 1568.f
};
inline constexpr SfxDef kSfxAlarmGentlePreview = {
    SFX_SQUARE, 988.f, 988.f, 0.50f, 0.31f, 0.09f, 0.38f, 1175.f, 1320.f
};
inline constexpr SfxDef kSfxAlarmClassicRing = {
    SFX_SQUARE, 988.f, 988.f, 0.78f, 0.38f, 0.07f, 0.34f, 1175.f, 1480.f
};
inline constexpr SfxDef kSfxAlarmChimeRing = {
    SFX_SQUARE, 880.f, 880.f, 0.84f, 0.38f, 0.07f, 0.34f, 1109.f, 1320.f
};
inline constexpr SfxDef kSfxAlarmDigitalRing = {
    SFX_SQUARE, 1047.f, 1047.f, 0.66f, 0.36f, 0.06f, 0.32f, 1175.f, 1568.f
};
inline constexpr SfxDef kSfxAlarmGentleRing = {
    SFX_SQUARE, 988.f, 988.f, 0.90f, 0.34f, 0.09f, 0.38f, 1175.f, 1320.f
};

// ---- Tetris -----------------------------------------------------------------

inline constexpr SfxDef kSfxTetrisMove     = { SFX_SQUARE, 600.f,  600.f,  0.025f, 0.30f, 0.05f, 0.30f, 0.f,    0.f    };
inline constexpr SfxDef kSfxTetrisRotate   = { SFX_SQUARE, 900.f,  700.f,  0.040f, 0.40f, 0.05f, 0.40f, 0.f,    0.f    };
inline constexpr SfxDef kSfxTetrisDrop     = { SFX_SQUARE, 300.f,   80.f,  0.100f, 0.50f, 0.02f, 0.50f, 0.f,    0.f    };
inline constexpr SfxDef kSfxTetrisLock     = { SFX_NOISE,  200.f,  100.f,  0.060f, 0.35f, 0.01f, 0.60f, 0.f,    0.f    };
inline constexpr SfxDef kSfxTetrisLine1    = { SFX_SQUARE, 523.f, 1047.f,  0.150f, 0.50f, 0.02f, 0.30f, 0.f,    0.f    };
inline constexpr SfxDef kSfxTetrisLine4    = { SFX_SQUARE, 523.f,  523.f,  0.300f, 0.60f, 0.02f, 0.20f, 784.f,  1047.f };
inline constexpr SfxDef kSfxTetrisGameOver = { SFX_SAW,    400.f,   60.f,  0.500f, 0.50f, 0.01f, 0.70f, 0.f,    0.f    };
inline constexpr SfxDef kSfxTetrisLevelUp  = { SFX_SQUARE, 660.f,  660.f,  0.250f, 0.50f, 0.02f, 0.20f, 880.f, 1175.f  };

}  // namespace sfx
