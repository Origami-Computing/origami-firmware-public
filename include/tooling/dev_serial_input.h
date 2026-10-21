#pragma once

#include <Arduino.h>

namespace tooling {
namespace dev_serial_input {

void begin();
void service(uint32_t nowMs);
void resetProvisioningState();
bool provisioningRequested();
bool provisioningComplete();

}  // namespace dev_serial_input
}  // namespace tooling
