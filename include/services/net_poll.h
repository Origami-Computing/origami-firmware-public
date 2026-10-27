#pragma once

#include <stdint.h>

/**
 * @file net_poll.h
 * @brief Unified periodic-HTTP task (Core 0).
 *
 * One FreeRTOS task that owns ALL small, periodic, non-urgent outbound HTTP so
 * it never runs on the UI core. Consolidating these into a single task with one
 * TLS-sized stack (a) keeps the display pipeline free of blocking network I/O,
 * and (b) serializes TLS sessions so only one handshake's heap footprint exists
 * at a time (important on this RAM-tight board).
 *
 * Jobs (each on its own "due" timer):
 *   - automations GET   — was a blocking GET on ui_task; now here.
 *   - telemetry/crash    — drain the SD-persisted queue and POST  (later phase).
 *   - heartbeat POST     — periodic device health                  (later phase).
 *   - OTA version check  — push the OTA app to ui_task on a hit     (later phase).
 *
 * Heavy / latency-sensitive transports (voice streaming, media upload) keep
 * their own dedicated tasks — they are intentionally NOT folded in here.
 */

class Print;

namespace services {
namespace net_poll {

/** Start or cancel a pending stop. False during creation/retirement: retry later. */
bool start();
/** Coalesce restoration intent; does not start work or cancel an active stop. */
void requestStart();
/** UI-task, nonblocking: arm one bounded registration burst after onboarding
 * Wi-Fi joins, including reconfirmation with an existing bearer token. Worker
 * consumes intent; existing auth/rate-limit cooldowns survive. */
void requestOnboardingRegistration();
/** Bounded scheduling hint: defer idle Agent reconnects during confirmation. */
bool onboardingRegistrationPriority();
/** Service tick: consume pending intent and admit startup as one decision.
 * False means no request, busy lifecycle or allocation failure; retry on later
 * ticks. Stop cancels older requests; later requests survive in-flight startup. */
bool startRequested();
/** Cancel pending restoration and request shutdown; wait up to timeoutMs.
 * True observes work quiescence;
 * it does not promise RTOS deletion/stack reclamation or prevent a later start.
 * False means creation, work or retirement remains active. */
bool stop(uint32_t timeoutMs = 1500);
/** Request shutdown; UI callers must not wait for in-flight HTTP. */
void requestStop();
bool running();
// Read-only worker position and elapsed time; does not feed or relax the watchdog.
void printStatus(Print& out);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Isolated policy check on device; simulated outcomes, no transport/NVS mutation.
void printRegistrationRetryCheck(Print& out);
#endif

/** Publish the latest UI-core health snapshot. Replaces unsent older health. */
void setHealthSnapshot(const char* json);

/** True once stop() has been requested. Long-running jobs (e.g. device_fs file
 *  transfers) poll this and abort promptly so stop() can quiesce within its
 *  timeout. Voice/music request this stop on a best-effort basis; their audio
 *  suspension result reports whether exit completed or remains pending. */
bool stopRequested();
/** TLS boundary: cancel only this worker's handshake after a stop request. */
bool cancelHandshakeForCurrentTask();

}  // namespace net_poll
}  // namespace services
