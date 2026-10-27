#pragma once

#include <stddef.h>
#include <stdint.h>

namespace sfx {
// UI-task diagnostics for successfully written blips, reset on each TX open.
struct TypewriterStats {uint32_t played=0,totalGapMs=0,maxGapMs=0,noteCounts[5]{};};
TypewriterStats typewriterStats();

/**
 * Pokémon / Animal Crossing-style per-character text blip.
 *
 * Real Pokémon Gen 1/2 text uses the NES APU's pulse 1 channel at narrow duty (~12.5 %),
 * one short blip per character displayed.  A narrow duty cycle gives the "digital speech"
 * chiptune character — same principle as Animal Crossing animalese and Undertale dialogue.
 *
 * Pitch is selected deterministically from `charIndex` over a five-note pentatonic set
 * (A4/B4/D5/E5/G5) so the same reply always plays the same melody — varied enough to feel
 * lively, not random enough to feel musical.
 *
 * Each blip is 8 ms (128 frames @ 16 kHz), followed by silence to finish its DMA
 * descriptor. A 40 % linear release softens the trailing edge. Blips are limited
 * to one every 56 ms for the same cadence in tutorial and streamed agent text.
 *
 * Unlike sfx::play() (fire-and-forget), the typewriter keeps an I2S TX session open
 * for the full duration of the typing animation so consecutive characters do not pay
 * the codec reconfigure cost.  The caller must call typewriterTeardown() on exit paths
 * (app exit, PTT start) to release playback before recording.
 *
 * Interaction with sfx::play()
 * ----------------------------
 * The sfx engine tracks TX ownership via hal::audio::txGeneration().  When the typewriter
 * holds TX, sfx::play() silently skips (someone else owns the bus).  sfx::service() also
 * detects the generation mismatch and clears its own ownership flag — no conflict.
 *
 * Usage
 * -----
 *   sfx::typewriterOpen();                        // once per typing session
 *   sfx::typewriterPlayChar(c, charIndex);        // once per revealed character
 *   sfx::typewriterTeardown();                    // on exit / PTT start
 */

/** Open an I2S TX session for typewriter playback at `sampleRate` Hz (default 16 kHz).
 *  Configures the TLV320 codec for playback and calls hal::audio::txBegin().
 *  No-op and returns true if already open.  Returns false if the codec or I2S failed. */
bool typewriterOpen(uint32_t sampleRate = 16000);

/** Synthesise and push one character blip (8 ms, 128 frames @ 16 kHz).
 *  Space, newline, tab and non-printable characters produce no sound.
 *  `charIndex` selects pitch from the five-note pentatonic set.
 *  Returns true only when the blip was successfully written to I2S.
 *  No-op if typewriterOpen() has not succeeded. */
bool typewriterPlayChar(char c, size_t charIndex);

/** End the I2S TX session only if it still belongs to the typewriter.
 *  Safe to call when not open (no-op). */
void typewriterTeardown();

/** True while a typewriter TX session is open. */
bool typewriterIsOpen();

/** Open the session if needed, then play one character blip.
 *  Silently tears down and returns if the mic is actively recording (I2S conflict).
 *  Returns true only for an admitted, successfully written blip; callers can
 *  advance a note sequence without whitespace/frame skips changing its melody.
 *  Safe to call unconditionally from the typing animation tick — no agent state required. */
bool typewriterServiceChar(char c, size_t charIndex);

}  // namespace sfx
