#pragma once
#include <stdint.h>
#include "core/cine_film_matrices.h"
namespace core {
namespace cine {
enum class ColorMode : uint8_t { Sunny, Gold, Portra, Mono, Cool, Vivid, Count };
constexpr unsigned colorCount = unsigned(ColorMode::Count);
// Shared C3→C5 greenward tint correction is baked into Sunny/Gold/Portra/Vivid.
// Cool already includes it; zero-chroma Mono is unchanged. Gamma/Y and WB stay fixed.
constexpr uint8_t gamma[16] = {34, 49, 71, 99, 110, 120, 129, 137, 145, 152, 165, 177, 198, 216, 233, 31};
constexpr uint8_t matrix[11] = {29, 96, 3, 14, 134, 145, 137, 122, 20, 1, 152};
constexpr uint8_t monoMatrix[11] = {29, 96, 3, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t monoGamma[16] = {24, 40, 65, 101, 115, 128, 139, 148, 157, 165, 180, 193, 215, 231, 244, 16};
constexpr uint8_t coolMatrix[11] = {29, 96, 3, 10, 115, 137, 127, 121, 19, 1, 152};
constexpr uint8_t vividMatrix[11] = {29, 96, 3, 16, 169, 188, 177, 161, 26, 1, 152};
struct ColorProfile {
  const char *name;
  const uint8_t *gamma, *matrix;
};
// Gold/Portra are quantized fits to G5/P5, not full nonlinear film LUTs.
// All profiles keep the Sunny sensor WB gains. See docs/cine-color-modes.md.
constexpr ColorProfile colorProfiles[] = {
    {"Sunny", gamma, matrix}, {"Gold", gamma, goldMatrix},
    {"Portra", gamma, portraMatrix}, {"Mono", monoGamma, monoMatrix},
    {"Cool", gamma, coolMatrix}, {"Vivid", gamma, vividMatrix}};
static_assert(sizeof(colorProfiles) / sizeof(colorProfiles[0]) == colorCount, "Color table mismatch");
inline bool validColor(unsigned value) { return value < colorCount; }
inline const ColorProfile &colorProfile(ColorMode mode) {
  return colorProfiles[validColor(unsigned(mode)) ? unsigned(mode) : 0];
}
inline ColorMode advanceColor(ColorMode mode, int delta) {
  const int64_t step = int64_t(validColor(unsigned(mode)) ? unsigned(mode) : 0) + delta;
  return ColorMode((step % int64_t(colorCount) + colorCount) % colorCount);
}
// OV3660 quantization scale: lower means higher quality. See
// docs/cine-jpeg-compression-study.md.
constexpr uint8_t jpegQuantizationScale = 8;
// Q4 real gain ceiling, shared by continuous Auto and manual handoff.
constexpr uint16_t gainCeilingQ4 = 8 * 16;
constexpr int8_t aeLevel = -1;
} // namespace cine
} // namespace core
