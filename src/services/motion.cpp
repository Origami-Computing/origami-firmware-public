#include "services/motion.h"
#include "hal/hal_imu.h"
namespace services { namespace motion {
bool ready() { return hal::imu::isReady(); }
bool readAxes(int32_t accel[3], int32_t gyro[3]) { return hal::imu::readAxes(accel, gyro); }
bool Gyroscope::open() {
    if (!active_) active_ = hal::imu::acquireGyroscope();
    return active_;
}
void Gyroscope::close() {
    if (!active_) return;
    hal::imu::releaseGyroscope();
    active_ = false;
}
} }
