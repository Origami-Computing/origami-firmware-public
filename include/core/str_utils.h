#pragma once
/**
 * @file core/str_utils.h
 * @brief String encoding and manipulation utilities (header-only).
 *
 * Pure functions with no dependencies on Arduino or ESP-IDF.
 */
#include <stddef.h>

namespace core {
namespace str {

// ASCII stand-in for a decoded codepoint the device font cannot draw, or
// nullptr to drop it. Punctuation keeps its meaning; Latin-1 letters lose
// their accent; everything else (emoji, symbols, other scripts) is dropped.
inline const char* asciiFallback(unsigned long cp) {
    switch (cp) {
        case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x202F: return " ";
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0x00B4: return "'";
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0x00AB: case 0x00BB: return "\"";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
        case 0x2022: case 0x00B7: case 0x2023: case 0x2043: return "-";
        case 0x2026: return "...";
        case 0x00D7: return "x";
        case 0x2192: return "->";
        case 0x2190: return "<-";
        case 0x00B0: return " deg";
        case 0x00A9: return "(c)";
        case 0x00AE: return "(R)";
        case 0x2122: return "(TM)";
        case 0x20AC: return "EUR";
        case 0x00A3: return "GBP";
        case 0x00BD: return "1/2";
        case 0x00BC: return "1/4";
        case 0x00DF: return "ss";
        case 0x00C6: return "AE";
        case 0x00E6: return "ae";
        case 0x0152: return "OE";
        case 0x0153: return "oe";
        default: break;
    }
    if (cp >= 0x00C0 && cp <= 0x00FF) {
        // One NUL-terminated letter per codepoint U+00C0..U+00FF; empty = drop (x and / signs).
        static const char kLatin1[] = "A\0A\0A\0A\0A\0A\0A\0C\0E\0E\0E\0E\0I\0I\0I\0I\0D\0N\0O\0O\0O\0O\0O\0\0\0O\0U\0U\0U\0U\0Y\0T\0s\0a\0a\0a\0a\0a\0a\0a\0c\0e\0e\0e\0e\0i\0i\0i\0i\0d\0n\0o\0o\0o\0o\0o\0\0\0o\0u\0u\0u\0u\0y\0t\0y\0";
        const char* letter = kLatin1 + 2 * (cp - 0x00C0);
        return *letter ? letter : nullptr;
    }
    return nullptr;
}

// Convert UTF-8 to printable ASCII for the device font. ASCII bytes (< 0x80)
// are copied as-is; each non-ASCII codepoint becomes its asciiFallback() or is
// dropped, never a '?' placeholder. Malformed bytes are dropped. The output is
// always NUL-terminated and will not exceed cap bytes (including the NUL).
inline void utf8LossyToAscii(const char* src, char* dst, size_t cap) {
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    if (!src || !*src) return;
    size_t o = 0;
    const unsigned char* p = (const unsigned char*)src;
    while (*p && o + 1 < cap) {
        if (*p < 0x80u) {
            dst[o++] = (char)*p++;
            continue;
        }
        unsigned char c = *p;
        size_t        seqLen = 0;
        unsigned long cp = 0;
        if      ((c & 0xE0u) == 0xC0u) { seqLen = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0u) == 0xE0u) { seqLen = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8u) == 0xF0u) { seqLen = 4; cp = c & 0x07u; }
        else { p++; continue; }
        bool ok = true;
        for (size_t i = 1; i < seqLen; i++) {
            if (!p[i] || ((unsigned char)p[i] & 0xC0u) != 0x80u) { ok = false; break; }
            cp = (cp << 6) | ((unsigned char)p[i] & 0x3Fu);
        }
        if (!ok) { p++; continue; }
        p += seqLen;
        const char* sub = asciiFallback(cp);
        while (sub && *sub && o + 1 < cap) dst[o++] = *sub++;
    }
    dst[o] = '\0';
}

}  // namespace str
}  // namespace core
