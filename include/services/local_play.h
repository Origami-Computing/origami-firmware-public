#pragma once
#include <stdint.h>

namespace services { namespace local_play {
// Single UI-task LAN session. Fixed-size datagrams, same-link broadcast only.
// An app must close its socket on cover/exit. No Wi-Fi configuration changes.
constexpr unsigned packetSize=64;
using Packet=uint8_t[packetSize];
uint32_t open(); // Nonzero random session ID, zero if the LAN socket is unavailable.
void close(uint32_t owner);
bool send(uint32_t owner,const Packet& packet);
bool receive(uint32_t owner,Packet& packet);
}}
