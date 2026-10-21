#pragma once
#include <stddef.h>
namespace widgets {
// A single unescaped URL/path segment. Reject separators, traversal, escapes,
// control characters and truncation rather than trying to normalize them.
inline bool validPathSegment(const char* value, size_t maximum) {
    if (!value || !*value) return false;
    for (size_t i = 0; value[i]; ++i) {
        const unsigned char c = value[i];
        if (i >= maximum || !((c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')) return false;
    }
    return true;
}
}
