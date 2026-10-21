#include "services/vibration.h"
/**
 * @file services.cpp
 * @brief Background services — split between svc_task (Core 0) and ui_task (Core 1).
 *
 * svc_task (Core 0) handles quote/cache completion, deferred OTA scheduling
 * and upload-manager work. It does not fetch releases, draw or access I2C.
 * Upload initiation may wait for network ownership before creating its worker;
 * ServiceWork admission therefore spans the complete iteration.
 *
 * Operations using Wire/Wire1 stay on ui_task (Core 1): automation input,
 * pedometer, IO expander, power gauge and codec. Camera SCCB shares these buses.
 * OTA HTTP runs through the OTA workflow on ui_task, whose stack is sized for
 * TLS. The background task posts a navigation request instead.
 *
 * NOTE: hal::ble::service() was moved BACK to ui_task. BLE service does SD
 * file writes (via lockSpi) for BLE file transfer. When it ran on svc_task
 * (Core 0) it held the SPI mutex concurrently with ui_task's display flush,
 * causing the display to freeze until BLE released SPI.
 *
 * ui_task (Core 1, priority 5) — see main_menu.cpp — owns everything else:
 *   automation (I2C), steps (I2C), idle/sleep policy, nav pipeline, draw.
 */

#include "services.h"
#include "core/release_policy.h"
#include "services/background_network.h"

#include <Arduino.h>
#include <atomic>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "hal/hal_tasks.h"
#include "widgets/sync.h"

#include "core/health_policy.h"
#include "services/uploads.h"
#include "config/app_config.h"
#include "services/automation.h"
#include "core/menu_config.h"
#include "services/net_poll.h"
#include "services/ota_client.h"
#include "core/sleep_timer.h"
#include "core/watchdog.h"
#include "services/morning_quote.h"
#include "core/power_button.h"
#include "hal/hal_audio.h"
#include "hal/hal_ble.h"
#include "hal/hal_camera.h"
#include "hal/hal_display.h"
#include "hal/hal_imu.h"
#include "hal/hal_io.h"
#include "hal/hal_power.h"
#include "hal/fuel_gauge_workaround.h"
#include "hal/hal_sd.h"
#include "hal/hal_wifi.h"
#include "nav_handler.h"
#include "Pins.h"
#include "sfx/sfx_lib.h"
#include "ui/home_eyes.h"
#include "ui/kami_motion.h"

namespace services {

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

// UI and Agent transport publish activity; the UI reads it for idle policy.
static std::atomic<uint32_t> s_lastActivityMs{0};

// One coalesced OTA prompt, produced by svc_task and consumed by ui_task.
static std::atomic<bool> s_updatePrompt{false};

// Upload rate-limit (svc_task only).
static uint32_t s_uploadTickMs = 0;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void notifyUserActivity() {
    s_lastActivityMs = millis();
}

uint32_t lastActivityMs() {
    return s_lastActivityMs.load();
}

bool takeUpdatePrompt() {
    return s_updatePrompt.exchange(false);
}

// ---------------------------------------------------------------------------
// Battery sleep — must be called from ui_task (calls NavHandler + Wire).
// ---------------------------------------------------------------------------

static bool wakeButtonHeld() {
    return core::power_button::wakeRequested(false,false,hal::io::readButtonC());
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void testUpdatePrompt() { s_updatePrompt.store(true); }
static uint32_t s_sleepTestWakeAt=0;
void armSleepTestWake(uint32_t delayMs) { s_sleepTestWakeAt=millis()+delayMs; }
#endif
static bool sleepWakeRequested() {
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    if(s_sleepTestWakeAt && (int32_t)(millis()-s_sleepTestWakeAt)>=0) return true;
#endif
    return wakeButtonHeld() || services::automation::shouldExitBatterySleepLoop();
}

static void holdDirectPinsForDeepSleep() {
    pinMode(LCD_RESET_PIN, OUTPUT);
    pinMode(LCD_CS_PIN, OUTPUT);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(LCD_RESET_PIN, LOW);
    digitalWrite(LCD_CS_PIN, HIGH);
    digitalWrite(SD_CS, HIGH);
    gpio_hold_en((gpio_num_t)LCD_RESET_PIN);
    gpio_hold_en((gpio_num_t)LCD_CS_PIN);
    gpio_hold_en((gpio_num_t)SD_CS);
    gpio_deep_sleep_hold_en();
}

static void safeOffAccessoryOutputs() {
    hal::display::setBacklight(false);
    digitalWrite(LCD_RESET_PIN, LOW);
    hal::display::setEnabled(false);
    hal::io::setCameraPwdn(true);
    hal::io::setCameraReset(false);
    hal::io::enableRegulators(false, false);
    hal::io::setAudioReset(true);
    hal::io::setStemmaPower(false);
    hal::io::setVibration(false);
    hal::io::setEnVsys(false);
    pinMode(LCD_CS_PIN, OUTPUT);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(LCD_CS_PIN, HIGH);
    digitalWrite(SD_CS, HIGH);
}

static void waitForPowerSwitchRelease() {
    Serial.println("[power-switch] waiting for SW1 release before deep sleep");
    while (hal::io::readPowerSwitch()) {
        core::watchdog::feed();
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    delay(50);
    uint8_t p0 = 0, p1 = 0, p2 = 0;
    (void)hal::io::readExpanderInputs(&p0, &p1, &p2);
    (void)hal::io::buttonInterruptPending();
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
static uint64_t s_shutdownTestWakeUs = 0;
#endif

void doPowerSwitchShutdown() {
    static bool s_shuttingDown = false;
    if (s_shuttingDown) return;
    s_shuttingDown = true;

    Serial.println("[power-switch] SW1 press -> controlled accessory shutdown");
    core::watchdog::unsubscribe();

    auto& nav = NavHandler::instance();
    App* root = nav.root();
    nav.shutdown(); // Includes waiting for OTA cancellation; releases app reservations.
    core::watchdog::subscribe();
    core::watchdog::feed();

    services::background_network::RadioPause network;
    const auto paused = network.acquire();
    if (paused != services::background_network::RadioPause::Result::Acquired) {
        Serial.printf("[power-switch] shutdown deferred: background %s busy\n",
                      services::background_network::resultName(paused));
        if (root) nav.init(root);
        nav.armInputBlock(400u);
        hal::input::suppressUntilButtonsReleased();
        notifyUserActivity();
        s_shuttingDown = false;
        return;
    }
    Serial.println("[power-switch] network quiescent");
    services::automation::prepareForBatterySleep();
    hal::ble::deinit();
    hal::wifi::powerDown();
    hal::camera::deinit();
    hal::audio::deinitMic();
    hal::sd::end();

    safeOffAccessoryOutputs();
    waitForPowerSwitchRelease();
    core::watchdog::unsubscribe();

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    if (s_shutdownTestWakeUs) {
        const esp_err_t timer = esp_sleep_enable_timer_wakeup(s_shutdownTestWakeUs);
        Serial.printf("[power-shutdown-test] timer_us=%llu result=%d\n",
                      (unsigned long long)s_shutdownTestWakeUs, (int)timer);
    }
#endif
    const esp_err_t ext0 = esp_sleep_enable_ext0_wakeup(GPIO_NUM_1, 0);
    holdDirectPinsForDeepSleep();
    Serial.printf("[power-switch] deep sleep now; wake on TCA_INT_N GPIO1 low, ext0=%d\n",
                  (int)ext0);
    Serial.flush();
    delay(100);
    esp_deep_sleep_start();
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void testPowerSwitchShutdown() {
    // USB bench only: real accessory shutdown and deep sleep, with an added
    // temporary RTC timer so verification does not depend on a physical press.
    s_shutdownTestWakeUs = 8000000ULL;
    Serial.println("[power-shutdown-test] real shutdown; automatic wake in 8 seconds");
    doPowerSwitchShutdown();
    s_shutdownTestWakeUs = 0; // A future refusal must not arm the next real press.
}
#endif

bool servicePowerSwitch(uint32_t nowMs) {
    static bool initialized = false;
    static bool candidate = false;
    static bool stable = false;
    static uint32_t candidateSince = 0;
    static uint32_t lastPoll = 0;
    if ((uint32_t)(nowMs - lastPoll) < 15u) return false;
    lastPoll = nowMs;

    const bool raw = hal::io::readPowerSwitch();
    if (!initialized) {
        initialized = true;
        candidate = stable = raw;
        candidateSince = nowMs;
        return false;
    }
    if (raw != candidate) {
        candidate = raw;
        candidateSince = nowMs;
        return false;
    }
    if (candidate != stable && (uint32_t)(nowMs - candidateSince) >= 35u) {
        stable = candidate;
        if (stable) {
            doPowerSwitchShutdown();
            return true;
        }
    }
    return false;
}

void doBatterySleep() {
    services::vibration::stopAll();
    const bool onExt = hal::power::externalPowerPresent(hal::power::read());
    hal::power::saveFuelWorkaroundBeforeSleep();
    Serial.printf("[services] sleep (timer=%lu s, extPower=%d)\n",
                  (unsigned long)core::sleep_timer::seconds(), (int)onExt);

    NavHandler::instance().prepareForSleep();

    services::background_network::RadioPause network;
    const auto paused = network.acquire();
    if (paused != services::background_network::RadioPause::Result::Acquired) {
        Serial.printf("[services] sleep deferred: background %s busy\n",
                      services::background_network::resultName(paused));
#ifdef ORIGAMI_DEV_SERIAL_INPUT
        s_sleepTestWakeAt = 0;
#endif
        NavHandler::instance().reenterRoot();
        NavHandler::instance().armInputBlock(400u);
        hal::input::suppressUntilButtonsReleased();
        notifyUserActivity();
        return;
    }
    Serial.println("[services] sleep network quiescent");
    services::automation::prepareForBatterySleep();

    hal::display::clearContentArea(TFT_WHITE);
    ui::home_eyes::sleepTransition(hal::display::tft());

    // Fully deinit the NimBLE controller (not just stop advertising) before light
    // sleep so the BT controller isn't running through the sleep transition — the
    // documented coexistence abort cause. Brought back by the wifi-task reconcile
    // on wake only if actually needed (provisioning). NOTE: deliberately do NOT
    // touch the audio codec/I2S here — tearing it down left the codec-mode cache
    // stale on wake so the speaker amp never re-enabled (audio went dead).
    hal::ble::deinit();
    hal::wifi::stop();
    hal::display::setBacklight(false);
    hal::imu::pollWakeUp();

    // Ignore a stuck-low button read left over from the wake gesture.
    // The power-off hold must be released before it can count as a new wake.
    while(wakeButtonHeld()) { core::watchdog::feed(); delay(25); }

    // doBatterySleep() runs on ui_task, which is subscribed to the Task Watchdog.
    // The wait loops below intentionally park this task (busy-wait on USB, or
    // esp_light_sleep on battery) without ever feeding the WDT — so after
    // TWDT_TIMEOUT_MS the watchdog panicked and rebooted the device mid-sleep
    // ("crash in sleep", even on USB). Sleeping is deliberate, not a freeze, so
    // unsubscribe ui_task from freeze-detection for the duration and re-subscribe
    // on wake.
    core::watchdog::unsubscribe();

    if (onExt) {
        // On USB power: skip CPU light-sleep (would wake immediately via charger
        // detect). Hold backlight off until user presses a button or an alarm fires.
        // Deliberately avoid re-reading charger state inside the loop — an I2C
        // glitch returning "Off" would break out after 150 ms and restore instantly.
        while (!sleepWakeRequested()) {
            // Keep the 1 Hz battery account running while the display is asleep.
            (void)hal::power::read();
            vTaskDelay(pdMS_TO_TICKS(150));
        }
    } else {
        // TODO(rev4): IMU INT1 is on the expander (P05), not an ESP GPIO, so
        // wakeUpIntPin() is -1 and ext0 wake is unavailable — needs a rethink
        // (e.g. wake on the expander ~INT line, or a timer-only wake). Guard the
        // call so we never pass an invalid pin to esp_sleep_enable_ext0_wakeup.
        const int imuWakePin = hal::imu::wakeUpIntPin();
        if (imuWakePin >= 0) {
            esp_err_t ext0 = esp_sleep_enable_ext0_wakeup((gpio_num_t)imuWakePin, 1);
            if (ext0 != ESP_OK) {
                Serial.printf("[services] WARN: ext0 wake setup failed (%d)\n", (int)ext0);
            }
        } else {
            Serial.println("[services] rev4: no IMU ext0 wake pin — timer/button wake only");
        }

        for (;;) {
            esp_sleep_enable_timer_wakeup(150ULL * 1000ULL);
            esp_light_sleep_start();
            if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) (void)hal::imu::pollWakeUp();
            hal::power::Status pw = hal::power::read();
            if (hal::power::externalPowerPresent(pw)) break;
            if (sleepWakeRequested()) break;
        }
    }

#ifdef ORIGAMI_DEV_SERIAL_INPUT
    s_sleepTestWakeAt=0;
#endif
    hal::input::suppressUntilButtonsReleased();
    // Re-arm freeze-detection now that ui_task resumes normal looping.
    core::watchdog::subscribe();
    core::watchdog::feed();

    hal::display::setBacklight(true);
    hal::wifi::restore();
    hal::ble::restoreAfterSleep();

    Wire.end();
    Wire.begin(SDA_PIN, SCL_PIN);
    delay(50);
    if (hal::io::reinitAfterI2CRestart()) delay(50);
    hal::imu::pollWakeUp();

    hal::input::resetRotaryEncoderPins();
    hal::input::beginRotaryInterrupts();
    hal::input::resyncRotaryQuadrature();

    // Light sleep stopped I2S BCLK, so the codec PLL lost lock and its DAC config
    // is stale even though the cache says "ready". Invalidate it so the first sound
    // after wake — the boot chime here, and crucially a sleep-wake alarm ring —
    // fully re-inits the codec and actually produces audio.
    hal::audio::invalidateForWake();

    ui::kami_motion::reset();
    NavHandler::instance().reenterRoot();
    network.release();
    sfx::play(sfx::kSfxBoot);
    // Start AFTER blocking root/audio setup so the full wake sequence is visible.
    ui::kami_motion::trigger(ui::kami_expression::Action::Wake,millis());
    NavHandler::instance().armInputBlock(400u);
    NavHandler::instance().invalidateChrome();
    notifyUserActivity();
    Serial.println("[services] wake from battery sleep -> root app");
}

// ---------------------------------------------------------------------------
// svc_task — Core 0, NO I2C, NO NavHandler, NO display.
//
// Runs quote completion, deferred OTA scheduling and upload-manager work.
// Upload initiation can wait for network ownership; the admission guard covers
// the whole iteration, including that wait. BLE remains on ui_task.
//
// Everything that uses Wire/I2C (automation button reads, IMU, power gauge,
// IO expander) stays in ui_task to avoid concurrent Wire access corrupting
// the I2C bus and hanging camera init (SCCB = same bus).
// ---------------------------------------------------------------------------

static void svcTask(void*) {
    core::watchdog::subscribe();   // freeze detection for the background task
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        core::watchdog::feed();
        const uint32_t now = millis();
        services::background_network::ServiceWork work;
        if (!work) continue; // Still feed the watchdog while radio ownership is elsewhere.

        // Process completed agent text replies and persist quotes off ui_task.
        services::morning_quote::serviceBackground(now);

        // BLE service was moved back to ui_task (see main_menu.cpp) to keep
        // all SPI-touching work sequential with hal::display::flush().

        const auto backgroundPolicy = core::health::backgroundPolicy();

        // ---- OTA auto-check -----------------------------------------------
        // svc_task has only 6 KB of stack. A TLS handshake to api.github.com
        // needs ~12–16 KB (mbedTLS cert parsing + bignum key exchange).
        // Calling fetchLatestRelease() here overflows the stack, corrupts
        // adjacent lwIP state, and causes every TCP connect to return -1.
        //
        // Fix: just push the OTA app on the timer. ota_update_app::tick()
        // runs on ui_task (16 KB stack) and does the HTTPS check safely.
        // The app shows "Up to date" when already on the latest version.
        if (core::release_policy::productionOnly) {
            // Removing Developer hold must not enable unattended production
            // updates. The user opens Update Firmware and confirms downloads.
            if (services::automation::otaUpdateRequested()) {
                services::automation::clearOtaRequest();
                Serial.println("[ota] remote update ignored: open Update Firmware manually");
            }
        } else if (!s_updatePrompt.load()) {
            if (services::automation::otaUpdateRequested()) {
                services::automation::clearOtaRequest();
                // Remote requests only open the confirmation UI, never install.
                s_updatePrompt.store(true);
            } else if (hal::wifi::connected() &&
                       backgroundPolicy.allowAutomaticUpdate) {
                static uint32_t s_otaCheckMs = 0;
                if (s_otaCheckMs == 0) s_otaCheckMs = now;
                if ((uint32_t)(now - s_otaCheckMs) >= OTA_AUTO_CHECK_INTERVAL_MS) {
                    s_otaCheckMs = now;
                    s_updatePrompt.store(true);
                }
            }
        }

        // ---- Upload manager -----------------------------------------------
        // Initialization and tick may prepare storage or wait for network ownership.
        // Keep both within ServiceWork admission, not just the worker wakeup.
        if (core::health::backgroundPolicy().allowUploads) {
            if ((uint32_t)(now - s_uploadTickMs) >= 250u) {
                s_uploadTickMs = now;
                services::uploads::ensureInit();
                services::uploads::tick();
                services::background_network::tick();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

void init() {
    s_lastActivityMs = millis();

    if (hal::imu::init()) {
        Serial.println("IMU OK");
        if (hal::imu::enableStepCounter())
            Serial.println("Pedometer ON (LSM6DSL)");
        else
            Serial.println("WARNING: pedometer enable failed");
    } else {
        Serial.println("WARNING: IMU init failed");
    }

}

bool startBackgroundTasks() {
    hal::wifi::startTask();
    if (!hal::tasks::spawn(hal::tasks::Id::Services, svcTask)) {
        Serial.println("[services] FAILED to start svc_task");
        return false;
    }
    Serial.println("[services] svc_task started on Core 0");
    // Unified periodic-HTTP task: owns the automations GET (moved off ui_task)
    // and, in later phases, telemetry/heartbeat/OTA polling.
    services::net_poll::start();
    widgets::sync::startPolling();
    return true;
}

}  // namespace services
