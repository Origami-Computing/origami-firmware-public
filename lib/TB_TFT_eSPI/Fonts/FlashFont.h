#pragma once
// Select one immutable asset catalog; the renderer owns no font heap allocation.
#if defined(ORIGAMI_INSTRUMENT_FONT) && defined(ORIGAMI_SOFT_BLOCK_DISPLAY)
#error Select only one default font catalog
#elif defined(ORIGAMI_INSTRUMENT_FONT)
#include "Instrument/metrics.h"
namespace flash_font = instrument;
#elif defined(ORIGAMI_SOFT_BLOCK_DISPLAY)
#include "SoftBlock/metrics.h"
namespace flash_font = soft_block;
#endif

#if defined(ORIGAMI_INSTRUMENT_FONT)
inline const instrument::Font& selectedFlashFont(uint8_t size) { return instrument::fonts[size-1]; }
#elif defined(ORIGAMI_SOFT_BLOCK_DISPLAY)
inline const soft_block::Font& selectedFlashFont(uint8_t size) { return soft_block::fonts[size-2]; }
#endif
