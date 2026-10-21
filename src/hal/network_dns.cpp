// Compatibility boundary for the pinned Arduino 2.x resolver. Its process-wide
// DNS event bits allow 16s queueing + 15s lookup, beyond the worker watchdog.
// lwIP owns DNS/cache/retries; this adapter only bounds the caller's wait and
// retains callback storage safely after a timeout. No TLS/hostname checks change.
#include <Arduino.h>
#include <WiFiGeneric.h>
#include <IPAddress.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <cstring>
#include <type_traits>

namespace {
constexpr uint32_t kWaitMs = 2500;
constexpr unsigned kSlots = 4;
struct Lookup {
    char hostname[254];
    uint32_t address;
    bool used, done, abandoned;
};
Lookup slots[kSlots] = {};
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;

void complete(const char*, const ip_addr_t* ip, void* context) {
    auto* lookup = static_cast<Lookup*>(context);
    portENTER_CRITICAL(&mutex);
    if (lookup->abandoned) {
        lookup->used = false; // The timed-out caller no longer owns this slot.
    } else {
        lookup->address = ip && IP_IS_V4(ip) ? ip_2_ip4(ip)->addr : 0;
        lookup->done = true;
    }
    portEXIT_CRITICAL(&mutex);
}

void startLookup(void* context) {
    auto* lookup = static_cast<Lookup*>(context);
    // This callback runs on tcpip_thread, the owner of lwIP's DNS table.
    portENTER_CRITICAL(&mutex);
    const bool abandoned = lookup->abandoned;
    if (abandoned) lookup->used = false;
    portEXIT_CRITICAL(&mutex);
    if (abandoned) return;
    ip_addr_t address{};
    const err_t result = dns_gethostbyname(lookup->hostname, &address, complete, lookup);
    if (result != ERR_INPROGRESS) complete(nullptr, result == ERR_OK ? &address : nullptr, lookup);
}
}

extern "C" int __wrap__ZN16WiFiGenericClass10hostByNameEPKcR9IPAddress(
    const char* hostname, IPAddress& result) {
    result = static_cast<uint32_t>(0);
    if (!hostname || !*hostname) return 0;
    if (result.fromString(hostname)) return 1;
    const size_t length = strnlen(hostname, sizeof(slots[0].hostname));
    if (length == sizeof(slots[0].hostname)) return 0;
    Lookup* lookup = nullptr;
    portENTER_CRITICAL(&mutex);
    for (auto& candidate : slots) {
        if (candidate.used) continue;
        candidate = Lookup{};
        candidate.used = true;
        memcpy(candidate.hostname, hostname, length + 1);
        lookup = &candidate;
        break;
    }
    portEXIT_CRITICAL(&mutex);
    if (!lookup) return 0; // Outstanding timed-out callbacks cannot be reused.
    const uint32_t started = millis();
    // Nonblocking queue submission; a full tcpip mailbox must not extend the budget.
    if (tcpip_try_callback(startLookup, lookup) != ERR_OK) {
        portENTER_CRITICAL(&mutex); lookup->used = false; portEXIT_CRITICAL(&mutex);
        return 0;
    }
    for (;;) {
        portENTER_CRITICAL(&mutex);
        const bool done = lookup->done;
        const uint32_t address = lookup->address;
        const bool timeout = !done && uint32_t(millis() - started) >= kWaitMs;
        if (done) lookup->used = false;
        else if (timeout) lookup->abandoned = true;
        portEXIT_CRITICAL(&mutex);
        if (done) { result = address; return address != 0; }
        if (timeout) {
            Serial.printf("[dns] lookup timed out after %lu ms\n", (unsigned long)(millis() - started));
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static_assert(std::is_same<decltype(&WiFiGenericClass::hostByName),
              decltype(&__wrap__ZN16WiFiGenericClass10hostByNameEPKcR9IPAddress)>::value,
              "DNS wrapper must match the SDK static function ABI");
