#pragma once
#include <stddef.h>

namespace cfg { namespace memory {
// Ordinary malloc/new above this size prefer PSRAM on equipped boards.
// Arduino's 4096-byte default lets general buffers consume the internal heap
// required by task stacks, I2S, WiFi and hardware AES. Explicit DMA/INTERNAL
// requests remain internal; this preference does not reserve a hard floor.
constexpr size_t kGeneralInternalLimit = 256;

// Preallocated at WiFi init, released at deinit. Combined with Arduino's four
// RX buffers, this bounds radio DMA use without per-packet TX fragmentation.
constexpr int kWifiTxBuffers = 4;
} }
