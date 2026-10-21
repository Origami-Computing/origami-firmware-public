#pragma once

#include <stdint.h>

/**
 * Lightweight non-blocking sound-effect engine. Generates square/saw/sine/noise
 * waveforms with frequency sweep, envelope and 3-step arpeggio, writes the
 * samples straight to the HAL I2S TX, then schedules a lazy teardown so back-to-back
 * sounds reuse the open codec/I2S session (no init flap).
 *
 * Replaces the older `hal::synth` module: same `SfxDef` shape, no PSRAM buffer,
 * no dedicated task, and aware of TX ownership (via `hal::audio::txGeneration()`)
 * so we never trample MP3 playback or the agent.
 *
 * Usage:
 *   #include "sfx/sfx_lib.h"
 *   sfx::play(kSfxMenuTick);          // imperative one-shot (menu, nav, tetris)
 *   ...
 *   // from the main loop:
 *   sfx::service();                   // also pumps sfx::activeAudioGroup() (boot)
 *
 * Scene screens use sfx::SoundPlayer + sfx::AudioGroup per docs/scene_architecture.md.
 */

enum SfxWaveform : uint8_t {
  SFX_SQUARE = 0,
  SFX_SAW,
  SFX_SINE,
  SFX_NOISE,
};

struct SfxDef {
  SfxWaveform wave;
  float freqStart;    ///< Hz at t=0
  float freqEnd;      ///< Hz at t=duration (linear sweep when arp1/arp2 == 0)
  float duration;     ///< seconds (intended ≤ ~1s)
  float volume;       ///< 0..1
  float attack;       ///< fraction of duration (linear ramp up)
  float decay;        ///< fraction of duration (linear ramp down at the end)
  float arp1;         ///< Hz for the middle third of the sound (0 = disabled)
  float arp2;         ///< Hz for the final third of the sound (0 = disabled)
};

namespace sfx {

/**
 * Render the sound and push samples to the codec. Non-blocking from the caller's
 * point of view in the sense that no task wait is involved — but the call itself
 * runs the generator inline for `duration` * sample_rate frames (≤ ~30 ms for
 * typical UI clicks). Skips silently when the mic is open, or when another
 * subsystem already owns I2S TX (MP3 playback, agent reply).
 */
void play(const SfxDef& def);

/** UI-task-only SFX volume, 0..100. Route profiles apply; local volume is not synchronized. */
int volume();
void setVolume(int vol0to100);
int scaledVolumeBasisPoints();
void loadVolume();
void saveVolume();

/**
 * Call once per main-loop iteration. Tears down I2S TX a couple of seconds after
 * the last sound finishes so consecutive sounds (menu scrolling) don't pay the
 * I2S install/uninstall cost. No-op when idle.
 */
void service();

/** HAL invokes after playback restoration: clear the active group, reset SFX and tear down typewriter. */
void onPlaybackRestored();

}  // namespace sfx
