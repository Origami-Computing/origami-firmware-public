#include <mbedtls/x509.h>
#include <time.h>
#include "core/board_time.h"

// Arduino 2.0.17's prebuilt x509 archive has HAVE_TIME_DATE disabled. Its
// exported date predicates always return zero. Link wrappers restore those
// checks for every TLS consumer without patching the installed SDK archive.
namespace {
bool compareNow(const mbedtls_x509_time* certificate, int& order) {
    if (!certificate || !core::time::isSynced()) return false;
    const time_t now = ::time(nullptr);
    struct tm utc;
    if (!gmtime_r(&now, &utc)) return false;
    const int actual[] = {utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                          utc.tm_hour, utc.tm_min, utc.tm_sec};
    const int bound[] = {certificate->year, certificate->mon, certificate->day,
                         certificate->hour, certificate->min, certificate->sec};
    order = 0;
    for (unsigned i = 0; i < 6; ++i) {
        if (actual[i] == bound[i]) continue;
        order = actual[i] < bound[i] ? -1 : 1;
        break;
    }
    return true;
}
}

extern "C" int __wrap_mbedtls_x509_time_is_past(const mbedtls_x509_time* to) {
    int order;
    return !compareNow(to, order) || order > 0;
}

extern "C" int __wrap_mbedtls_x509_time_is_future(const mbedtls_x509_time* from) {
    int order;
    return !compareNow(from, order) || order < 0;
}
