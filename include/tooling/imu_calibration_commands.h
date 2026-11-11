#pragma once
#include <stdint.h>
namespace tooling { namespace imu_calibration_commands {
// Serial adapter for services::imu_sensor_calibration (UI task). `line` is
// lower-cased and trimmed. service() steps a running capture or stream.
bool handleCommand(const char* line, uint32_t nowMs);
void service(uint32_t nowMs);
} }
