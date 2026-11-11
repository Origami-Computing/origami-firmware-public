// Runs the production timer against a file-backed store fake. Each invocation
// starts a new process, so reload checks exercise a cold cache like a reboot.
#include "core/sleep_timer.h"
#include "core/store.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>

static bool failWrite = false;
static bool silentWriteFailure = false;
static unsigned reads = 0;
static unsigned writes = 0;

static bool savedEnabled = false;
static bool hasEnabled = false;
static bool failRead = false;
static unsigned boolReads = 0;
namespace core { namespace store {
bool getBool(const char* ns, const char* key, bool fallback) {
    assert(!strcmp(ns, "settings") && !strcmp(key, "sleep_en") && fallback);
    ++boolReads;
    return failRead || !hasEnabled ? fallback : savedEnabled;
}
bool putBool(const char* ns, const char* key, bool value) {
    assert(!strcmp(ns, "settings") && !strcmp(key, "sleep_en"));
    if (failWrite) return false;
    savedEnabled = value; hasEnabled = true;
    return true;
}

uint32_t getU32(const char* ns, const char* key, uint32_t fallback) {
    assert(!strcmp(ns, "settings") && !strcmp(key, "sleep_sec"));
    ++reads;
    FILE* file = fopen("sleep-state.bin", "rb");
    if (!file) return fallback;
    uint32_t value = fallback;
    if (fread(&value, sizeof(value), 1, file) != 1) value = fallback;
    fclose(file);
    return value;
}
bool putU32(const char* ns, const char* key, uint32_t value) {
    assert(!strcmp(ns, "settings") && !strcmp(key, "sleep_sec"));
    ++writes;
    if (failWrite) return false;
    if (silentWriteFailure) return true; // store can report success on a failed NVS write.
    FILE* file = fopen("sleep-state.bin", "wb");
    assert(file);
    assert(fwrite(&value, sizeof(value), 1, file) == 1);
    assert(fclose(file) == 0);
    return true;
}
}}

int main(int argc, char** argv) {
    assert(argc >= 2);
    using namespace core::sleep_timer;
    if (!strcmp(argv[1], "enable")) {
        assert(enabled()); // Missing record defaults on.
        for (bool initial : {false, true}) for (bool target : {false, true}) {
            assert(setEnabled(initial) && enabled() == initial);
            failWrite = true;
            assert(!setEnabled(target) && enabled() == initial);
            failWrite = false;
            assert(setEnabled(target) && enabled() == target);
            failRead = true; assert(enabled()); failRead = false;
            assert(enabled() == target);
        }
        unsigned before = boolReads;
        for (unsigned i=0; i<100; ++i) (void)enabled();
        assert(boolReads == before+100); // Preserve read-through, not a new cache.
        savedEnabled = !savedEnabled;
        assert(enabled() == savedEnabled); // Out-of-band changes remain visible.
    } else if (!strcmp(argv[1], "expect")) {
        assert(argc == 3);
        const uint32_t expected = static_cast<uint32_t>(strtoul(argv[2], nullptr, 10));
        assert(seconds() == expected);
        assert(milliseconds() == expected * 1000u);
        for (unsigned i = 0; i < 100; ++i) assert(seconds() == expected);
        assert(reads == 1); // The UI idle loop must not reopen NVS each iteration.
    } else if (!strcmp(argv[1], "save")) {
        assert(argc == 3);
        const uint32_t value = static_cast<uint32_t>(strtoul(argv[2], nullptr, 10));
        assert(save(value));
        assert(seconds() == value && milliseconds() == value * 1000u);
    } else if (!strcmp(argv[1], "failures")) {
        assert(save(120));
        const unsigned before = writes;
        for (uint32_t invalid : {0u, 9u, 3601u, UINT32_MAX}) {
            assert(!save(invalid));
            assert(seconds() == 120);
        }
        assert(writes == before); // Invalid values must never reach storage.
        failWrite = true;
        assert(!save(60) && seconds() == 120);
        failWrite = false;
        silentWriteFailure = true;
        assert(!save(60) && seconds() == 120);
        silentWriteFailure = false;
        assert(save(60) && seconds() == 60); // Retry succeeds.
    } else {
        assert(false);
    }
    puts("PASS");
}
