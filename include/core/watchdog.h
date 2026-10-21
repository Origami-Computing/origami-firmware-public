#pragma once

/**
 * @file watchdog.h
 * @brief Task watchdog (TWDT) freeze detection with hung-task attribution.
 *
 * Subscribed tasks must call feed() each loop. If one stops feeding for
 * TWDT_TIMEOUT_MS the hardware TWDT panics and resets the device — the only way
 * to recover a truly frozen task. We can't POST while frozen, so attribution
 * happens on the NEXT boot: each feed() stamps a per-task timestamp in
 * RTC_NOINIT memory (survives the reset), and after a TASK_WDT reset the stalest
 * task is reported as the likely culprit. This is a heuristic; the hardware
 * watchdog's task list is authoritative. Retired handles are excluded even when
 * retained task storage reuses the same address for a different worker name.
 * crash_report reads hungTaskGuess() and reports it.
 *
 * Order matters at boot: read hungTaskGuess() (crash_report) BEFORE init(),
 * because init() clears the RTC table for the new run.
 */

namespace core {
namespace watchdog {

/** Configure the TWDT (panic-on-timeout) and reset the attribution table for
 *  this run. Call once at boot AFTER crash_report has read hungTaskGuess(). */
void init();

/** Subscribe the CURRENT task to the TWDT and register it for attribution.
 *  Call once from each watched task before its loop. Reuses the slot by task
 *  name, so a task destroyed and recreated (e.g. uploadq) is safe to re-subscribe. */
void subscribe();

/** Unsubscribe the CURRENT task from the TWDT. Dynamic tasks (uploadq) MUST call
 *  this before vTaskDelete — otherwise the watchdog keeps waiting on the dead
 *  task and falsely resets the device. */
void unsubscribe();

/** Reset the TWDT for the current task and record its feed time. Call every
 *  loop iteration of a subscribed task. */
void feed();

/** Name of the task that fed least recently before the last reset — the likely
 *  hung task. Empty if RTC state is invalid (cold boot). Read BEFORE init(). */
const char* hungTaskGuess();

}  // namespace watchdog
}  // namespace core
