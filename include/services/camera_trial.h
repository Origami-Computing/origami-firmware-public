#pragma once
#ifdef ORIGAMI_DEV_SERIAL_INPUT
#include "services/camera_session.h"

namespace services { namespace camera {
// UI-task diagnostic operation. Owns trial naming, driver frames, measurements
// and CSV writes. Borrows the app's session; never owns display or navigation.
// Use once: prepare -> begin -> shutter draw -> capture -> review/recovery -> finish.
// begin intentionally timestamps BEFORE shutter drawing. finish records recovery's
// result outside the measured interval. The caller restores/stops the session
// after capture, including when capture fails. No frames escape this object.
class SettleTrial {
public:
    using PreparationObserver = void (*)(Session::PrepareResult);
    bool prepare(const Session& session, uint32_t run, int frames, unsigned repetition);
    void begin(const hal::camera::Tuning& tuning, const char* mode);
    bool capture(Session& session, const hal::camera::Tuning& tuning,
                 PreparationObserver reportPreparation = nullptr);
    bool finish(bool restored);
    const char* photoPath() const { return path; }
    const char* settingLabel() const { return setting; }
private:
    uint32_t runId = 0;
    int discardFrames = 0;
    unsigned repeat = 0;
    char setting[12] = {}, path[64] = {}, manifestPath[64] = {};
    uint32_t startMs = 0, initStartMs = 0, settleStartMs = 0, discarded = 0;
    uint32_t acquireStartMs = 0, readyMs = 0, writeDoneMs = 0;
    uint16_t width = 0, height = 0;
    size_t bytes = 0;
    uint64_t frameStartUs = 0;
    bool saved = false;
};
} }
#endif
