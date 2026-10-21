#pragma once
#include <stdint.h>
namespace services { namespace motion {
// UI-task sensor access with the existing HAL units/failure behavior. Reading
// does not acquire a gyro lease; accelerometer-only consumers need no lease.
bool ready();
bool readAxes(int32_t accel[3], int32_t gyro[3]);

// Own one reference to the HAL's gyro power policy. Idempotent within one
// owner; independent consumers retain independent references. No sampling task,
// calibration state, frequency change or workflow reset is introduced here.
class Gyroscope {
public:
    Gyroscope() = default;
    ~Gyroscope() { close(); }
    Gyroscope(const Gyroscope&) = delete;
    Gyroscope& operator=(const Gyroscope&) = delete;
    bool open();
    void close();
    bool active() const { return active_; }
private:
    bool active_ = false;
};

// UI-task-only FIFO session. Owns the gyro lease, 40-sample PSRAM scratch and
// exclusive FIFO access. open/close are idempotent; failed startup rolls back.
// poll returns -1 on unavailable/read failure, otherwise a bounded sample count.
// Samples are millidegrees/s, oldest first, borrowed until next poll or close.
// reset discards stale samples after a covering view without acquiring twice.
class GyroFifo {
public:
    static constexpr int capacity = 40;
    GyroFifo() = default;
    ~GyroFifo() { close(); }
    GyroFifo(const GyroFifo&) = delete;
    GyroFifo& operator=(const GyroFifo&) = delete;
    bool open();
    bool reset();
    void close();
    bool active() const { return active_; }
    int poll();
    const int32_t (*samples() const)[3] { return samples_; }
    bool readAcceleration(int32_t out[3]) const;
private:
    Gyroscope gyro_;
    int32_t (*samples_)[3] = nullptr;
    bool active_ = false;
};
} }
