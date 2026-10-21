#include "services/motion.h"
#include "hal/hal_imu.h"
#include <esp_heap_caps.h>

namespace services { namespace motion {
namespace { GyroFifo* fifoOwner = nullptr; }

bool GyroFifo::open() {
    if (active_) return true;
    if (fifoOwner && fifoOwner != this) return false;
    if (!hal::imu::isReady()) return false;
    samples_ = static_cast<int32_t (*)[3]>(
        heap_caps_malloc(sizeof(int32_t) * 3 * capacity, MALLOC_CAP_SPIRAM));
    if (!samples_) return false;
    if (!gyro_.open()) { close(); return false; }
    // Claim before programming: partial FIFO register writes need rollback too.
    fifoOwner = this;
    if (!hal::imu::startGyroFifo()) { close(); return false; }
    active_ = true;
    return true;
}

bool GyroFifo::reset() {
    if (!active_) return open();
    if (hal::imu::startGyroFifo()) return true;
    close();
    return false;
}

void GyroFifo::close() {
    if (fifoOwner == this) {
        hal::imu::stopGyroFifo();
        fifoOwner = nullptr;
    }
    active_ = false;
    gyro_.close();
    heap_caps_free(samples_);
    samples_ = nullptr;
}

int GyroFifo::poll() {
    return active_ ? hal::imu::readGyroFifo(samples_, capacity) : -1;
}

bool GyroFifo::readAcceleration(int32_t out[3]) const {
    return active_ && out && hal::imu::readAcceleration(out);
}
} }
