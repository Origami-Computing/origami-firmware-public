#pragma once
#include <stddef.h>
#include <stdint.h>

namespace core { namespace images {
// Require exactly one bounded JPEG envelope, with frame and scan headers.
// Length-delimited metadata may contain marker bytes; entropy may contain
// stuffing/restarts. This does not validate entropy or decoder compatibility.
// Borrowed input, no allocations; linear work bounded by the caller's size cap.
inline bool singleJpegEnvelope(const uint8_t* data, size_t size) {
    if (!data || size < 4 || data[0] != 0xff || data[1] != 0xd8) return false;
    size_t at = 2;
    bool scanning = false, frame = false, scan = false;
    while (at < size) {
        if (scanning) while (at < size && data[at] != 0xff) ++at;
        if (at == size || data[at] != 0xff) return false;
        while (at < size && data[at] == 0xff) ++at;
        if (at == size) return false;
        const uint8_t marker = data[at++];
        if (scanning && (marker == 0 || (marker >= 0xd0 && marker <= 0xd7))) continue;
        scanning = false;
        if (marker == 0xd9) return frame && scan && at == size;
        if (!marker || marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7)) return false;
        if (marker == 1) continue;
        if (marker < 0xc0 || size - at < 2) return false;
        const size_t length = (size_t(data[at]) << 8) | data[at + 1];
        if (length < 2 || length > size - at) return false;
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) frame = true;
        if (marker == 0xda) scanning = scan = true;
        at += length;
    }
    return false;
}
} }
