#pragma once
#include <stddef.h>
#include <stdint.h>

namespace services { namespace wifi_portal {

struct Status {
    uint8_t clients;
    bool stationConnected;
    char stationAddress[24];
};

// One device-wide AP + captive HTTP/DNS session, driven by the UI task. Retains the
// HAL's deferred restore, reconnect and offline-BLE policies. No new worker.
bool open();
void poll();
void close();
Status status();
const char* networkName();
const char* password();

} }
