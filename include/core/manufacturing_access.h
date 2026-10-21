#pragma once
#include <stdint.h>
#include <cstring>

namespace core {
inline bool manufacturingReadOnlyCommand(const char* line) {
    return !strcmp(line,"ota status") || !strcmp(line,"station status") ||
           !strcmp(line,"station inventory") || !strcmp(line,"shipping status") || !strcmp(line,"mem");
}
// Volatile USB service session; no physical confirmation. Reboot starts locked.
class ManufacturingAccess {
public:
    bool active() const { return active_; }
    bool pending() const { return false; } // Serial protocol compatibility.
    bool request(uint32_t, bool usbPower) {
        if (!usbPower) return false;
        active_ = true;
        return true;
    }
    void close() { active_ = false; }
private:
    bool active_ = false;
};
}
