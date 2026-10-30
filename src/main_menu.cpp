#include "hal/hal_encoder_trace.h"
#include "apps/agent/agent_flow.h"
#include "services/vibration.h"
#include "services/shipping.h"
#include "services/battery_alerts.h"
#include "services/playback_worker.h"
/**
 * @file main_menu.cpp
 * @brief Arduino setup() entry point + ui_task for the Origami menu firmware.
 *
 * Task map
 * --------
 *   ui_task  (Core 1, priority 5) — owns the full display pipeline AND all I2C:
 *     sfx → chrome → automation (I2C) → alert → OTA nav → steps (I2C) →
 *     idle policy (power/ADC) → input → NavHandler → draw → flush
 *
 *   svc_task (Core 0, priority 2, 20 ms cadence) — only the two actual blockers:
 *     BLE service, OTA GitHub HTTPS check, upload manager kick.
 *     NO I2C, NO NavHandler, NO display.
 *
 *   wifi_mgr (Core 0, priority 2) — WiFi boot, join backoff, restore.
 *
 * I2C ownership
 * -------------
 * Wire (I2C bus) is NOT thread-safe across cores.  All Wire-using code —
 * automation button reads (MCP23017), IMU steps (LSM6DSL), power gauge,
 * audio codec, camera SCCB — runs exclusively on ui_task (Core 1).
 * svc_task performs zero I2C operations so the camera driver can safely
 * call Wire.end() / Wire.begin() during camera init/deinit without racing.
 *
 * Cross-task communication
 * ------------------------
 *   services::takeUpdatePrompt() — svc_task posts an OTA prompt; ui_task consumes it
 *   services::notifyUserActivity() — UI/transport atomically publish activity
 */

#ifdef MENU_APP

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/platform.h>

#include "hal/hal_tasks.h"
#include "services/agent_transport.h"
#include "services/agent_inbox.h"
#include "services/tls_trust.h"
#include "services/agent_voice.h"
#include "apps/agent_app.h"
#include "apps/boot_app.h"
#include "services/alarms.h"
#include "services/firmware_update.h"
#include "apps/morning_routine_app.h"
#include "apps/ota_update_app.h"
#include "apps/pomodoro_app.h"
#include "apps/timer_app.h"
#include "services/steps.h"
#include "services/countdown.h"
#include "apps/tutorial_app.h"
#include "apps/factory_test_app.h"
#include "widgets/app.h"
#include "widgets/launch.h"
#include "config/app_config.h"
#include "services/automation.h"
#include "core/board_bringup.h"
#include "core/board_time.h"
#include "tooling/dev_serial_input.h"
#include "core/crash_report.h"
#include "core/device_id.h"
#include "services/device_fs.h"
#include "services/net_poll.h"
#include "services/eyes_asset.h"
#include "core/menu_config.h"
#include "services/ota_client.h"
#include "core/secrets.h"
#include "core/telemetry.h"
#include "core/telemetry_queue.h"
#include "core/watchdog.h"
#include "core/sleep_timer.h"
#include "hal/hal_ble.h"
#include "services/ble_files.h"
#include "hal/hal_audio.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_io.h"
#include "hal/hal_power.h"
#include "hal/hal_tasks.h"
#include "services/agent_transport.h"
#include "services/agent_voice.h"
#ifdef LUA_PROBE
#include "widgets/probe.h"
#endif
#include "nav_handler.h"
#include "services.h"
#include "services/wifi_qr_session.h"
#include "sfx/sfx_lib.h"
#include "ui/automation_alert.h"
#include "services/morning_quote.h"

// ---------------------------------------------------------------------------
// ui_task
// ---------------------------------------------------------------------------

static volatile bool s_uiTaskEnabled = false;
static volatile bool s_setupHealthy = false;

static constexpr uint32_t kNormalProvisionProbeMs = 250;
static constexpr uint32_t kManualProvisionWindowMs = 4000;
// 500 ms paper lead-in + 4.25 s Clean Pull + 100 ms final-logo hold. Keep
// background services deferred until the complete visual handoff has finished.
static constexpr uint32_t kBootSequenceAndHoldMs = 4850;



// System-alert workflow composition stays above the renderer and services.
// Called synchronously on the UI task; consumes all inputs while Timer rings.
static bool handleTimerAlertInput(hal::input::InputEvent event) {
    using EventType = hal::input::EventType;
    if (!services::automation::isAlertActive() ||
        services::automation::activeAlertServerId() != services::countdown::kServerId) return false;
    if (event.type == EventType::ButtonC_Tap) {
        if (services::countdown::snooze(AUTOMATION_SNOOZE_SEC) != services::countdown::Result::Ok) return true;
    } else if (event.type != EventType::ButtonA_Tap && event.type != EventType::ButtonB_Tap) return true;
    services::automation::dismissActiveAlert();
    auto& nav = NavHandler::instance();
    nav.dismissSystemScreen(); // Screen exit stops its sound before navigation.
    if (event.type == EventType::ButtonA_Tap) nav.popToRoot();
    else nav.open(&apps::timer::app);
    return true;
}

static void uiTask(void*) {
    static uint32_t s_stepsTickMs  = 0;
    static uint32_t s_autoTickMs   = 0;  // automation I2C rate limit
    static uint32_t s_alarmTickMs  = 0;  // alarm NVS rate limit
    static uint32_t s_powerTickMs  = 0;  // power read rate limit
    static uint32_t s_bleTickMs    = 0;  // BLE service rate limit

    while (!s_uiTaskEnabled) vTaskDelay(1);

    core::watchdog::subscribe();   // freeze detection for the display pipeline
    static uint32_t s_hbTickMs = 0;  // health heartbeat rate limit

    for (;;) {
        const uint32_t now = millis();

        // Feed the task watchdog: if this loop wedges for TWDT_TIMEOUT_MS the
        // device resets and reports "hung_task: ui" on next boot.
        core::watchdog::feed();
        services::firmware_update::pollSettingsNotice();
        services::ota_client::serviceBootHealth(s_setupHealthy);

        services::wifi_qr::Session::instance().pollCleanup();
        services::audio::PlaybackWorker::pollCleanup();

        if (services::servicePowerSwitch(now)) {
            vTaskDelay(1);
            continue;
        }

        // Consume worker replies on ui_task, even when another app or an
        // alert covers Kami. Callbacks update state; they never navigate/draw.
        services::agent_transport::pollReplies();
        services::agent_inbox::poll();
        services::agent_voice::poll();

        // ---- Health heartbeat ---------------------------------------------
        // Assembled here on Core 1 because battery/power gauge reads are I2C
        // (forbidden on svc_task). Publish is cheap + non-I2C; net_poll ships only the latest snapshot.
        if (s_hbTickMs == 0 || (uint32_t)(now - s_hbTickMs) >= TELEMETRY_HEARTBEAT_INTERVAL_MS) {
            s_hbTickMs = now;
            hal::power::Status pw = hal::power::read();
            JsonDocument hb;
            hb["device_id"]    = core::device::id();
            hb["fw"]           = core::device::firmwareVersion();
            hb["uptime_ms"]    = (uint32_t)now;
            hb["heap_free"]    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            hb["psram_free"]   = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            hb["battery_pct"]  = (int)pw.battery_percent;
            hb["battery_estimated"] = pw.battery_estimated;
            hb["rssi"]         = (int)WiFi.RSSI();
            const auto telemetryStats = core::telemetry_queue::stats();
            hb["telemetry_queued"] = telemetryStats.queued;
            hb["telemetry_dropped"] = telemetryStats.droppedDurable;
            hb["telemetry_journal_failures"] = telemetryStats.journalFailures;
            char json[512];
            serializeJson(hb, json, sizeof(json));
            services::net_poll::setHealthSnapshot(json);
            LOGD("health", "Heartbeat uptime=%lu heap=%u rssi=%d", (unsigned long)now, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (int)WiFi.RSSI());
        }

        // ---- SFX -----------------------------------------------------------
        hal::audio::serviceOutputRoute(now);
        sfx::service();
        services::battery_alerts::service(now);

        // ---- Chrome --------------------------------------------------------
        NavHandler::instance().chrome().service(now);

        // ---- BLE service ---------------------------------------------------
        // Runs here (ui_task, Core 1) so its SPI bus accesses (SD file writes
        // for BLE file transfer) are sequential with hal::display::flush().
        // When run from svc_task (Core 0) concurrently, it held the SPI mutex
        // while ui_task was waiting to flush — causing the display to freeze.
        // Rate-limited to 20 ms: BLE characteristic updates don't need frame rate.
        if ((uint32_t)(now - s_bleTickMs) >= 20u) {
            s_bleTickMs = now;
            hal::ble::service(now, services::automation::ingestBleNotification);
        }

        // ---- Scheduled events and alert vibration -------------------------
        // Retain the 10 ms cadence. Alert input arrives through the normal
        // Screen handler; this tick does not sample buttons independently.
        if ((uint32_t)(now - s_autoTickMs) >= 10u) {
            s_autoTickMs = now;
            services::automation::service(now);
            if (services::automation::takeUserActivityPulse()) services::notifyUserActivity();
        }

        // ---- Alarm clock / timer (NVS) -------------------------------------
        // Rate-limited to 1 Hz. NVS flash operations (possible erase cycles)
        // can take 10-50 ms; calling this every frame at 50-100 Hz means 5-10x
        // more flash activity than the old ~15 Hz loop. 1 Hz is plenty for
        // rescheduling alarms that operate on minute/hour granularity.
        if ((uint32_t)(now - s_alarmTickMs) >= 1000u) {
            s_alarmTickMs = now;
            core::time::service(now);
            services::alarms::poll();
            services::countdown::poll();
        }

        // ---- System alert scene -------------------------------------------
        NavHandler& nav = NavHandler::instance();
        if (services::automation::wantsInputCapture() && !nav.systemScreenActive()) {
            nav.presentSystemScreen(&ui::kScreenAutomationAlert);
        } else if (!services::automation::wantsInputCapture() && nav.systemScreenActive()) {
            nav.dismissSystemScreen();
            nav.armInputBlock(180);
        }

        // Release Pomodoro sound/motor when covered; system alarms take priority.
        apps::pomodoro::serviceFeedback(now);
        services::vibration::servicePulse(now,!services::automation::wantsInputCapture() && !apps::pomodoro::isAlerting());
        services::vibration::servicePreview(now,!services::automation::wantsInputCapture() && !apps::pomodoro::isAlerting());
        services::vibration::serviceEffect(now,!services::automation::wantsInputCapture() && !apps::pomodoro::isAlerting());
        if (nav.systemScreenActive()) {
            hal::input::service(now);
            tooling::dev_serial_input::service(now);
            hal::input::InputEvent alertEvent;
            while (hal::input::poll(alertEvent)) {
                if (!handleTimerAlertInput(alertEvent)) nav.dispatch(alertEvent);
                else if(alertEvent.type==hal::input::EventType::Scroll)hal::input::encoderTrace(hal::input::TraceKind::Alert,alertEvent.traceId,alertEvent.delta);
                services::notifyUserActivity();
            }
            // Input can end the greeting in this very loop. Dismiss before
            // rendering, then fall through to the pending quote navigation.
            if (services::automation::wantsInputCapture()) {
                nav.tick(now);
                nav.draw(hal::display::canvas());
                hal::display::flush();
                vTaskDelay(1);
                continue;
            }
            nav.dismissSystemScreen();
            nav.armInputBlock(180);
        }

        // The Good Morning system screen has fully dismissed. Normalize the
        // interrupted stack and begin the private morning workflow above Kami.
        if (services::automation::takeMorningRoutineRequest()) {
            nav.popToRoot();
            nav.push(&apps::morning_routine::app);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
            Serial.println("[morning] handoff=Quote");
#endif
            services::notifyUserActivity();
        }

        // Offer the tutorial only after Boot has unwound to the real root.
        if (!apps::factory_test::serviceBootOffer()) apps::tutorial::serviceBootOffer();

        // ---- Deferred OTA push from svc_task ------------------------------
        // svc_task posts a prompt on the OTA timer; ota_update_app performs HTTPS
        // on ui_task's larger stack after navigation.
        // All other nav decisions (popToRoot, batterySleep) happen inline below.
        if (services::takeUpdatePrompt()) {
            if (NavHandler::instance().depth() == 1)
                NavHandler::instance().push(&apps::ota_update::app);
        }

        // ---- Widget launch push (Kami agent -> device) --------------------
        // The server POSTs widget_loading when it starts generating; jump to the
        // Lua Widget app's loading screen, which auto-launches on widget_ready.
        if (widgets::launch::takeLoadingRequest()) {
            NavHandler& nav = NavHandler::instance();
            if (nav.top() != &widgets::app) nav.push(&widgets::app);
            widgets::showLoading();
        }

        const auto runtimePolicy = NavHandler::instance().runtimePolicy();

        // ---- Pedometer (I2C to LSM6DSL) ------------------------------------
        // Runs on ui_task so Wire access is single-threaded (no conflict with
        // camera SCCB or MCP23017 reads above).
        if ((uint32_t)(now - s_stepsTickMs) >= 200u) {
            s_stepsTickMs = now;
            if (runtimePolicy.allowStepSampling) {
                services::steps::poll();
            }
        }

        // ---- Idle / sleep policy ------------------------------------------
        // activity pulses from Kami agent and media playback
        if (apps::agent::takeMenuActivityPulse()) services::notifyUserActivity();
        services::device_fs::SyncStatus fsStatus;
        services::device_fs::getSyncStatus(fsStatus);
        const bool fsBusy = fsStatus.active && !fsStatus.done;
        if (runtimePolicy.reportActivity || fsBusy) {
            services::notifyUserActivity();
        }

        // Charger sense at 1 Hz — assume battery until the first read (not USB).
        static bool s_extPowerCached = false;
        if ((uint32_t)(now - s_powerTickMs) >= 1000u) {
            s_powerTickMs = now;
            hal::power::Status pw = hal::power::read();
            s_extPowerCached = hal::power::externalPowerPresent(pw);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
            static bool s_loggedInitialPower = false;
            if (!s_loggedInitialPower) {
                s_loggedInitialPower = true;
                hal::io::dumpChargerPinState("initial-power");
                Serial.printf("[power] pg_asserted=%d chg_asserted=%d state=%u extPower=%d\n",
                              pw.pg_active_low ? 1 : 0, pw.chg_active_low ? 1 : 0,
                              (unsigned)pw.charger, s_extPowerCached ? 1 : 0);
            }
#endif
        }
        const bool extPower = s_extPowerCached;

        // Use a signed delta so a lastActivityMs that is momentarily *ahead* of
        // `now` does not underflow. `now` is sampled once at the top of the loop,
        // but the Kami send task (Core 0) can call notifyUserActivity() a few ms
        // later in the same iteration; an unsigned subtraction would wrap to
        // ~UINT32_MAX and spuriously trip the idle return-to-home / battery sleep.
        int32_t idleDelta = (int32_t)(now - services::lastActivityMs());
        if (idleDelta < 0) idleDelta = 0;
        const uint32_t idleDur = (uint32_t)idleDelta;

        if (runtimePolicy.allowIdleReturn && !fsBusy) {
            if (idleDur >= IDLE_RETURN_TO_KAMI_HOME_MS) {
                if (NavHandler::instance().depth() > 1) {
                    NavHandler::instance().popToRoot();
                } else if (!apps::agent::isAtHomescreen()) {
                    apps::agent::returnToHomescreen();
                }
                // Do not reset the idle timer — the user is still away; sleep should
                // fire at the configured timeout from the last real interaction.
            }
        }

        if (runtimePolicy.allowBatterySleep && !fsBusy) {
            if (!extPower &&
                core::sleep_timer::enabled() &&
                idleDur >= core::sleep_timer::milliseconds() &&
                !services::automation::shouldDeferBatterySleep()) {
                services::doBatterySleep();
            }
        }

        // ---- Normal UI pipeline -------------------------------------------
        hal::input::service(now);
        tooling::dev_serial_input::service(now);

        hal::input::InputEvent ev;
        while (hal::input::poll(ev)) {
            NavHandler::instance().dispatch(ev);
            // Every input — including rotary scroll — counts as user activity. Scroll
            // was previously excluded (to keep rotary noise from blocking battery
            // sleep), but that meant scroll-only navigation never reset the idle timer:
            // once idle >= 60s, every scroll opened the menu and the return-to-home
            // policy popped it straight back to the eyes (with the open/back sounds)
            // until a button press reset the timer. Phantom detents are already
            // filtered by the rotary ISR's quadrature table, so counting scroll as
            // activity is safe.
            services::notifyUserActivity();
        }

        const bool bootWasRoot = nav.root() == &apps::boot::app;
        nav.tick(now);
        nav.draw(hal::display::canvas());
        const bool bootHandoffReady = bootWasRoot && nav.root() == &apps::agent::app;
        if (bootHandoffReady) {
            Serial.printf("[boot-timing] event=kami_frame_ready millis=%lu\n",
                          (unsigned long)millis());
        }

        if (!hal::display::isOptimizedRender()) {
            const uint32_t flushStartUs = bootHandoffReady ? micros() : 0u;
            hal::display::flush();
            if(nav.top()==&apps::agent::app && !nav.systemScreenActive()) apps::agent::flow::acknowledgeDisplayed();
            if (bootHandoffReady) {
                Serial.printf("[boot-timing] event=kami_first_flush_done millis=%lu flush_call_us=%lu last_flush_us=%lu\n",
                              (unsigned long)millis(),
                              (unsigned long)(micros() - flushStartUs),
                              (unsigned long)hal::display::lastFlushUs());
            }
        }

        vTaskDelay(1);
    }
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------

static void paintProvisioningScreen(const char* title, const char* detail) {
    // setup() runs before the canvas UI. Paint the controller, not the
    // redirected off-screen framebuffer, so provisioning feedback is visible.
    TFT_eSPI& tft = hal::display::directTft();
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextSize(2); tft.drawString(title, tft.width() / 2, tft.height() / 2 - 14, 1); tft.setTextSize(1);
    tft.setTextSize(2); tft.drawString(detail, tft.width() / 2, tft.height() / 2 + 14, 1); tft.setTextSize(1);
}

static bool runProvisioningGate(uint32_t normalProbeMs) {
    tooling::dev_serial_input::resetProvisioningState();
    tooling::dev_serial_input::begin();

    bool buttonA = false;
    bool buttonB = false;
    const bool buttonsReadable = hal::io::readButtonsSnapshot(&buttonA, &buttonB);
    const bool manualWindow = buttonsReadable && buttonA && buttonB;
    const uint32_t timeoutMs = manualWindow ? kManualProvisionWindowMs : normalProbeMs;
    bool provisioningVisible = false;
    if (manualWindow) {
        paintProvisioningScreen("Provisioning", "Waiting for host...");
        hal::display::rampBacklight(120);
        provisioningVisible = true;
        Serial.println("PROVISION MANUAL A+B");
    }
    Serial.printf("PROVISION WAIT %lu\n", (unsigned long)timeoutMs);

    const uint32_t start = millis();
    while (!tooling::dev_serial_input::provisioningRequested() &&
           (uint32_t)(millis() - start) < timeoutMs) {
        tooling::dev_serial_input::service(millis());
        delay(1);
    }

    if (!tooling::dev_serial_input::provisioningRequested()) {
        Serial.println("PROVISION SKIP normal_boot");
        return false;
    }

    paintProvisioningScreen("Provisioning", "Copying SD files...");
    if (!provisioningVisible) {
        hal::display::rampBacklight(120);
        provisioningVisible = true;
    }
    Serial.println("PROVISION MODE active");
    while (!tooling::dev_serial_input::provisioningComplete()) {
        tooling::dev_serial_input::service(millis());
        delay(1);
    }

    Serial.println("PROVISION MODE complete");
    paintProvisioningScreen("Provisioning", "Complete");
    delay(500);
    return true;
}

// Arduino-ESP32 bakes CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC=1 into its sdkconfig,
// so every TLS handshake allocates its ~16 KB record buffers from DRAM, where
// the largest free block is too small (~15 KB) -> MBEDTLS_ERR_SSL_ALLOC_FAILED
// (-32512). Redirect all mbedTLS allocations to PSRAM before any TLS context is
// created. (Regression guard: previously removed by 2f82b21, broke the agent.)
static void* mbedtlsPsramCalloc(size_t n, size_t size) {
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void setup() {
    const bool shippingWakeAuthorized = services::shipping::bootGate();
    hal::ble::configureFileReceiver(services::ble_files::receiver);
    mbedtls_platform_set_calloc_free(mbedtlsPsramCalloc, heap_caps_free);

    if (!core::board::bringupMenuHardware(shippingWakeAuthorized)) {
        Serial.println("[menu] board bringup FAILED — halting");
        while (1) delay(1000);
    }
    Serial.println("\n======== Origami Menu (serial trace) ========");
    // Build the shared CA bundle index before any network task can connect.
    services::tls::init();

    // Force the wall clock to "unset" before anything reads it. The RTC carries a
    // stale time across soft reboots; without this it would show a wrong clock and
    // arm alarms off it. Only SNTP (over WiFi) makes the clock valid from here.
    core::time::init();

    runProvisioningGate(kNormalProvisionProbeMs);

    services::ota_client::init();

    core::secrets::init();
    services::morning_quote::begin();
    services::automation::begin();

    // Warm the cached device_id while we're single-threaded, before background
    // tasks (net_poll, heartbeat) read it concurrently.
    Serial.printf("[boot] device_id=%s fw=%s\n",
                  core::device::id(), core::device::firmwareVersion());

    // Telemetry: ring/breadcrumb first, then the SD-backed queue (SD was mounted
    // by board bringup above).
    core::telemetry::init();
    core::telemetry_queue::init();
    // Report a prior crash/freeze. MUST run before watchdog::init() (which clears
    // the hung-task attribution table) so a TASK_WDT reset names the stalest task.
    core::crash_report::reportBootReason();
    core::watchdog::init();

    Serial.printf("[boot] agent buffers deferred — PSRAM contig=%u KB total=%u KB\n",
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024u),
                  (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024u));

    services::init();

    hal::input::resetRotaryEncoderPins();
    hal::input::beginRotaryInterrupts();
    hal::input::resyncRotaryQuadrature();

    // Boot is the initial lightweight root. Initializing Kami first loads its
    // large SD-backed eye sheet and starts transport work before any boot pixels
    // can be shown. The boot screen replaces itself with Kami on completion.
    NavHandler::instance().init(&apps::boot::app);
    NavHandler::instance().armInputBlock(400u);

    NavHandler::instance().chrome().service(millis());
    NavHandler::instance().invalidateChrome();
    NavHandler::instance().chrome().paint(hal::display::tft());

    TaskHandle_t uiHandle = hal::tasks::spawn(hal::tasks::Id::Ui, uiTask);
    if (!uiHandle) {
        Serial.println("[menu] critical UI task allocation failed — halting");
        while (1) delay(1000);
    }

    s_uiTaskEnabled = true;

    // Keep the LEDs dark until the UI task has produced and flushed a valid
    // boot frame. This turns the former white interval into useful animation.
    const uint32_t firstFrameDeadline = millis() + 500u;
    while (hal::display::lastFlushUs() == 0u &&
           (int32_t)(millis() - firstFrameDeadline) < 0) {
        delay(1);
    }
    hal::display::rampBacklight(120);
    Serial.printf("[boot-time] first frame visible at %lu ms flush_us=%lu\n",
                  (unsigned long)millis(),
                  (unsigned long)hal::display::lastFlushUs());

    // The animation is fully embedded and needs none of the work below. Keep
    // SD hashing, the Lua diagnostic, Wi-Fi, and periodic background services
    // out of its critical three-second render window.
    const uint32_t bootVisibleAt = millis();
    while ((uint32_t)(millis() - bootVisibleAt) < kBootSequenceAndHoldMs) {
        delay(10);
    }

    // Provisioning may have supplied the canonical home-eye sheet. Validate it
    // after the boot animation; missing assets are repaired after Wi-Fi starts.
    (void)services::eyes_asset::inspect();

#ifdef LUA_PROBE
    widgets::probeRun();  // Development diagnostic, deferred beyond first paint.
#endif

    s_setupHealthy = services::startBackgroundTasks();
    if (!s_setupHealthy) {
        Serial.println("[menu] background services unavailable; UI remains active");
    }
    Serial.printf("Setup complete at %lu ms\n", (unsigned long)millis());
}

void loop() {
    vTaskDelete(nullptr);
}

#endif  // MENU_APP
