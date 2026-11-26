#pragma once
// Reads a JPEG's capture time from its EXIF block (pure, no I/O; host-testable).
//
// Origami's camera writes DateTimeOriginal (0x9003) in UTC with
// OffsetTimeOriginal (0x9011) "+00:00" when the clock is known, and omits both
// otherwise (core/cine_exif.cpp). Other cameras usually write local time,
// sometimes with an offset. The result reports which: with an offset the time
// converts to a UTC instant; without one it is already wall-clock local time.
#include <stddef.h>
#include <stdint.h>

namespace core {
namespace exif {

struct CaptureTime {
    int year, month, day, hour, minute, second;
    bool hasOffset;      // offsetMinutes is valid; the fields are local to that offset
    int offsetMinutes;   // e.g. +00:00 -> 0, -07:00 -> -420
};

namespace detail {
struct Tiff {
    const uint8_t* p; size_t n; bool big;
    bool ok(size_t at, size_t len) const { return at <= n && len <= n - at; }
    uint16_t u16(size_t at) const { return big ? uint16_t(p[at] << 8 | p[at + 1]) : uint16_t(p[at] | p[at + 1] << 8); }
    uint32_t u32(size_t at) const {
        return big ? uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | p[at + 3]
                   : uint32_t(p[at + 3]) << 24 | uint32_t(p[at + 2]) << 16 | uint32_t(p[at + 1]) << 8 | p[at];
    }
    // Finds an ASCII (type 2) or LONG (type 4) tag in the IFD at `ifd`.
    // ASCII: returns the string's offset and length. LONG: returns the value in `at`.
    bool find(uint32_t ifd, uint16_t tag, uint32_t& at, uint32_t& len) const {
        if (!ok(ifd, 2)) return false;
        const unsigned count = u16(ifd);
        if (count > 512 || !ok(ifd + 2, size_t(count) * 12)) return false;
        for (unsigned i = 0; i < count; ++i) {
            const size_t e = ifd + 2 + size_t(i) * 12;
            if (u16(e) != tag) continue;
            const uint16_t type = u16(e + 2);
            const uint32_t items = u32(e + 4);
            if (type == 4 && items == 1) { at = u32(e + 8); len = 4; return true; }
            if (type != 2 || items == 0 || items > 64) return false;
            at = items <= 4 ? uint32_t(e + 8) : u32(e + 8);
            len = items;
            return ok(at, len);
        }
        return false;
    }
};
inline bool digits(const uint8_t* s, int n, int& out) {
    out = 0;
    for (int i = 0; i < n; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        out = out * 10 + (s[i] - '0');
    }
    return true;
}
}  // namespace detail

// "YYYY:MM:DD HH:MM:SS"
inline bool parseDateTime(const uint8_t* s, size_t len, CaptureTime& t) {
    if (len < 19 || s[4] != ':' || s[7] != ':' || s[10] != ' ' || s[13] != ':' || s[16] != ':') return false;
    if (!detail::digits(s, 4, t.year) || !detail::digits(s + 5, 2, t.month) || !detail::digits(s + 8, 2, t.day) ||
        !detail::digits(s + 11, 2, t.hour) || !detail::digits(s + 14, 2, t.minute) || !detail::digits(s + 17, 2, t.second))
        return false;
    return t.year >= 1970 && t.year <= 2200 && t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 &&
           t.hour <= 23 && t.minute <= 59 && t.second <= 60;
}

// "+HH:MM" / "-HH:MM"
inline bool parseOffset(const uint8_t* s, size_t len, int& minutes) {
    int h = 0, m = 0;
    if (len < 6 || (s[0] != '+' && s[0] != '-') || s[3] != ':' || !detail::digits(s + 1, 2, h) ||
        !detail::digits(s + 4, 2, m) || h > 14 || m > 59)
        return false;
    minutes = (s[0] == '-' ? -1 : 1) * (h * 60 + m);
    return true;
}

// Scans the JPEG markers before image data for APP1 "Exif". Only the first
// `bytes` are read, so a caller may pass just the file's opening few KiB.
inline bool readCaptureTime(const uint8_t* jpeg, size_t bytes, CaptureTime& out) {
    if (!jpeg || bytes < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return false;
    size_t at = 2;
    while (at + 4 <= bytes && jpeg[at] == 0xFF) {
        const uint8_t marker = jpeg[at + 1];
        if (marker == 0xDA || marker == 0xD9) return false;  // image data starts; no EXIF
        const size_t len = size_t(jpeg[at + 2]) << 8 | jpeg[at + 3];
        if (len < 2 || at + 2 + len > bytes) return false;
        if (marker == 0xE1 && len >= 16 && jpeg[at + 4] == 'E' && jpeg[at + 5] == 'x' && jpeg[at + 6] == 'i' &&
            jpeg[at + 7] == 'f' && jpeg[at + 8] == 0 && jpeg[at + 9] == 0) {
            const uint8_t* tiff = jpeg + at + 10;
            const size_t n = len - 8;
            if (!((tiff[0] == 'I' && tiff[1] == 'I') || (tiff[0] == 'M' && tiff[1] == 'M'))) return false;
            const detail::Tiff t{tiff, n, tiff[0] == 'M'};
            if (t.u16(2) != 42) return false;
            const uint32_t ifd0 = t.u32(4);
            uint32_t exifIfd = 0, len4 = 0, s = 0, sl = 0;
            CaptureTime ct{};
            bool found = false;
            if (t.find(ifd0, 0x8769, exifIfd, len4) && t.find(exifIfd, 0x9003, s, sl))
                found = parseDateTime(tiff + s, sl, ct);
            if (!found && t.find(ifd0, 0x0132, s, sl)) found = parseDateTime(tiff + s, sl, ct);  // DateTime
            if (!found) return false;
            ct.hasOffset = false;
            ct.offsetMinutes = 0;
            if (exifIfd && t.find(exifIfd, 0x9011, s, sl)) ct.hasOffset = parseOffset(tiff + s, sl, ct.offsetMinutes);
            out = ct;
            return true;
        }
        at += 2 + len;
    }
    return false;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant).
inline int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = unsigned((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

// UTC seconds for a time with a known offset.
inline int64_t toUnix(const CaptureTime& t) {
    return daysFromCivil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second -
           int64_t(t.offsetMinutes) * 60;
}

}  // namespace exif
}  // namespace core
