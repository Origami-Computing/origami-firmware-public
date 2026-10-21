#pragma once

#include <TB_TFT_eSPI.h>
#include <stddef.h>
#include <stdint.h>

namespace ui {
namespace text {

// ── Monospace bitmap font (6×8 px per cell, scaled by `sz`) ─────────────────
//
// wrappedSized / countWrapped / skipWrapped use TFT_eSPI font 1.
// Wrapping uses the selected default face's advances; line height is
// default_font::height(sz) + (sz>=2 ? 6 : 3).

/** Word-wrap `s` into `d` at (x,y). Returns Y just below last printed line. */
int wrappedSized(TFT_eSPI& d, const char* s, int x, int y,
                 int maxW, int maxLines, uint16_t fg, uint16_t bg, uint8_t sz);

/** Like wrappedSized but renders only the first `prefixChars` chars.
 *  `scratch` must be at least prefixChars+1 bytes. */
int wrappedSizedPrefix(TFT_eSPI& d, const char* s, size_t prefixChars,
                       int x, int y, int maxW, int maxLines,
                       uint16_t fg, uint16_t bg, uint8_t sz,
                       char* scratch, size_t scratchCap);

/** Count visual wrapped lines for size `sz`. */
int countWrapped(const char* s, int maxW, uint8_t sz);

/** Return pointer into `s` past `skipLines` visual wrapped lines. */
const char* skipWrapped(const char* s, int skipLines, int maxW, uint8_t sz);

// ── PixelFont (Pokémon / Animal Crossing dialogue style) ────────────────────
//
// Instrument uses font 1, proportional advances and 26px default line pitch.
// Other builds use PixelFont GFX (12px advance, 24px default line pitch).
// These functions select the face; y retains the legacy top+14 baseline API.

static constexpr uint8_t kPokeSz = 2;
static constexpr int     kPokeCW = 6 * kPokeSz;   // 12 px per glyph
#ifdef ORIGAMI_INSTRUMENT_FONT
static constexpr int kPokeLH=26;
#else
static constexpr int     kPokeLH = 9 * kPokeSz + 6; // 24 px per line (6 px inter-line gap; gives descenders breathing room)

#endif

// Instrument builds retain the baseline API but use proportional font 1.
/** Word-wrap using PixelFont at textSize=kPokeSz. Returns Y past last line. */
int wrappedPokemon(TFT_eSPI& d, const char* s, int x, int y,
                   int maxW, int maxLines, uint16_t fg, uint16_t bg,
                   int lineHeight = kPokeLH);

/** Like wrappedPokemon but renders only the first `prefixChars` chars.
 *  `scratch` must be at least prefixChars+1 bytes. */
int wrappedPokemonPrefix(TFT_eSPI& d, const char* s, size_t prefixChars,
                         int x, int y, int maxW, int maxLines,
                         uint16_t fg, uint16_t bg,
                         char* scratch, size_t scratchCap,
                         int lineHeight = kPokeLH);

/** Count visual wrapped lines for PixelFont. */
int countPokemon(const char* s, int maxW);

/** Return pointer into `s` past `skipLines` visual PixelFont lines. */
const char* skipPokemon(const char* s, int skipLines, int maxW);

}  // namespace text
}  // namespace ui
