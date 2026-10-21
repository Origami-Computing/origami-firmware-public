#pragma once

/**
 * @file crash_report.h
 * @brief Boot-time crash / reset-reason reporting.
 *
 * Reads esp_reset_reason() early in setup() and, when the previous reset looks
 * like a fault (panic, interrupt/task watchdog, brownout), enqueues a crash
 * event (KIND_CRASH) to the telemetry queue for POST /api/device/crashes. The
 * event includes the RTC breadcrumb (last tag + hung task) and the recent log
 * ring so the portal shows what the device was doing when it died.
 */

namespace core {
namespace crash_report {

/** Inspect the reset reason and enqueue a crash event if it was a fault.
 *  Call once in setup() AFTER telemetry::init() + telemetry_queue::init() and
 *  AFTER reading watchdog::hungTaskGuess(), but BEFORE watchdog::init(). */
void reportBootReason();

}  // namespace crash_report
}  // namespace core
