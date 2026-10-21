#pragma once

/**
 * @file services.h
 * @brief Background services split across two FreeRTOS tasks.
 *
 * Architecture (after ui_task / svc_task split):
 *
 *   ui_task  (Core 1, priority 5) — owns the display pipeline:
 *     sfx::service → chrome.service → system-screen routing → deferred nav →
 *     hal::input → NavHandler dispatch / tick / draw → hal::display::flush
 *
 *   svc_task (Core 0, priority 2, 20 ms cadence) — owns quote/cache completion,
 *     deferred OTA decisions and upload-manager work. No rendering or I2C.
 *     Upload initiation may wait for network ownership; it is not just a tick.
 *
 * RadioPause closes svc_task admission and waits for existing service work
 * before pausing network consumers. ServiceWork uses an atomic admission state;
 * radio teardown must not infer quiescence from a worker handle alone.
 * OTA prompts and the shared activity timestamp use atomic handoffs.
 */

#include <stdint.h>

namespace services {

/** One-time synchronous setup. Does not allocate background task stacks. */
void init();

/** Spawn wifi_mgr and svc_task after the critical UI task is allocated. */
bool startBackgroundTasks();

/** Notify that the user has interacted so the idle timer resets.
 *  Safe from UI or transport tasks; a later store replaces the timestamp. */
void notifyUserActivity();

/** Return the millis() timestamp of the last notifyUserActivity() call.
 *  Used by ui_task for idle / battery-sleep policy. */
uint32_t lastActivityMs();

// ── ui_task interface ────────────────────────────────────────────────────────
// All functions below are called from ui_task (main_menu.cpp) each frame.

/** Atomically consume the coalesced OTA prompt from svc_task. The UI keeps
 * its existing depth check: a consumed prompt may be discarded when covered. */
bool takeUpdatePrompt();

/** Execute the full battery sleep cycle (hardware sleep + wake restore).
 *  Must be called from ui_task — it calls NavHandler and display APIs directly.
 */
void doBatterySleep();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
/** Queue the normal UI handoff without fetching or installing firmware. */
void testUpdatePrompt();
/** Bounded USB regression escape hatch; does not alter saved sleep settings. */
void armSleepTestWake(uint32_t delayMs);
/** Real USB accessory/deep-sleep probe with a temporary eight-second RTC wake.
 * No persisted wake/sleep settings change; production SW1 retains GPIO wake. */
void testPowerSwitchShutdown();
#endif

/** Poll/debounce rev4 SW1 / PWRSW. Returns true if shutdown handling took over. */
bool servicePowerSwitch(uint32_t nowMs);

/** Controlled SW1 shutdown: quiesce software, switch accessories off, then deep sleep. */
void doPowerSwitchShutdown();

}  // namespace services
