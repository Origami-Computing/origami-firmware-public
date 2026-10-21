#pragma once
#include <stddef.h>
#include <stdint.h>
namespace core { namespace muni {
constexpr unsigned kMaxBuses = 12, kMaxArrivals = 3;
struct Bus { float latitude=0, longitude=0; uint32_t timestamp=0; };
struct Arrival { uint32_t epoch=0; char headsign[48]={}; char trip[40]={}; };
struct Feed {
    Bus buses[kMaxBuses]; Arrival arrivals[kMaxArrivals];
    uint8_t busCount=0, arrivalCount=0;
    uint32_t vehicleTimestamp=0, predictionTimestamp=0;
};
// Bounded JSON, route 5 only. Call on worker; outputs are owned values.
// Each parser changes only its corresponding half after full schema validation.
bool parseVehicles(const char* json, size_t length, Feed& out);
bool parseArrivals(const char* json, size_t length, const char* stop, uint32_t now, Feed& out);
} }
