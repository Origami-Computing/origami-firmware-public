#pragma once
#include <stddef.h>
#include <stdint.h>

namespace core { namespace pixel_rle {
// Native RGB565 words are preserved verbatim, including sprite byte order.
// Pairs are [run length, pixel]. No allocator or display dependency.
inline size_t encode(const uint16_t* pixels, size_t count, uint16_t* out = nullptr) {
    size_t words = 0;
    for (size_t i = 0; i < count;) {
        size_t end = i + 1;
        while (end < count && end - i < 65535 && pixels[end] == pixels[i]) ++end;
        if (out) { out[words] = uint16_t(end - i); out[words + 1] = pixels[i]; }
        words += 2;
        i = end;
    }
    return words;
}
inline bool decode(const uint16_t* runs, size_t words, uint16_t* out, size_t count) {
    if (words % 2) return false;
    size_t written = 0;
    for (size_t i = 0; i < words; i += 2) {
        const size_t n = runs[i];
        if (!n || n > count - written) return false;
        for (size_t j = 0; j < n; ++j) out[written++] = runs[i + 1];
    }
    return written == count;
}
} }
