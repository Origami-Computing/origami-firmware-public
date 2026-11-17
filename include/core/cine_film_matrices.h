// Gold/Portra matrix data derived from spektrafilm by Andrea Volpato.
// https://github.com/andreavolpato/spektrafilm
// CC BY-SA 4.0; modified/fitted by thesephist's Kami Color study, 2026-11-12.
// Shared greenward tint correction composed after the original fit.
// See third_party/licenses/spektrafilm/ for the license and changes.
#pragma once
#include <stdint.h>
namespace core {
namespace cine {
constexpr uint8_t goldMatrix[11] = {29, 96, 3, 30, 120, 146, 139, 84, 59, 1, 152};
constexpr uint8_t portraMatrix[11] = {29, 96, 3, 17, 145, 158, 123, 53, 75, 1, 152};
} // namespace cine
} // namespace core
