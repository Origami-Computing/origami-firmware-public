#pragma once

namespace core { namespace health {
// App constraints, evaluated on the UI task. Permission is not a resource grant:
// HAL ownership, upload reservations, system alerts and device-wide guards still apply.
struct RuntimePolicy {
    bool allowStepSampling;
    bool allowIdleReturn;
    bool allowBatterySleep;
    bool reportActivity;
    // C-hold standby permission, distinct from automatic battery sleep and SW1 shutdown.
    bool allowPowerSleep;
    bool allowBackgroundUploads = true;
    bool allowAutomaticUpdate = true;
    RuntimePolicy(bool steps=true, bool idle=true, bool sleep=true, bool activity=false, bool powerSleep=true)
        : allowStepSampling(steps), allowIdleReturn(idle),
          allowBatterySleep(sleep), reportActivity(activity), allowPowerSleep(powerSleep) {}
};
inline RuntimePolicy combine(RuntimePolicy a, RuntimePolicy b) {
    RuntimePolicy result{a.allowStepSampling && b.allowStepSampling,
            a.allowIdleReturn && b.allowIdleReturn,
            a.allowBatterySleep && b.allowBatterySleep,
            a.reportActivity || b.reportActivity,
            a.allowPowerSleep && b.allowPowerSleep};
    result.allowBackgroundUploads = a.allowBackgroundUploads && b.allowBackgroundUploads;
    result.allowAutomaticUpdate = a.allowAutomaticUpdate && b.allowAutomaticUpdate;
    return result;
}
// Preserve the existing active-media/game behavior: inhibit both idle actions
// and refresh activity while busy, so their timeout starts after the operation.
inline RuntimePolicy whileBusy(bool busy) { return {true, !busy, !busy, busy}; }
// Coherent scheduling hints for the background task. These do not grant a
// resource lease; service admission and network ownership remain mandatory.
struct BackgroundPolicy { bool allowUploads; bool allowAutomaticUpdate; };
// UI task only, after workflow or navigation state changes. No app pointers
// escape; depth gates automatic prompts, not remote update requests.
void publishBackgroundPolicy(RuntimePolicy policy, int navigationDepth);
BackgroundPolicy backgroundPolicy(); // Any task; one atomic snapshot.
} }
