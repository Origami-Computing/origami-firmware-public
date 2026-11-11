#include "core/board_bringup.h"

#include <SPI.h>
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <sys/time.h>
#include <esp_attr.h>

#include "Pins.h"
#include "core/clock_retention.h"
#include "hal/hal_bus.h"
#include "hal/hal_display.h"
#include "hal/hal_io.h"
#include "hal/hal_sd.h"

namespace core {
namespace board {

// Survives deep sleep; garbage after power-on, so the pair must agree.
struct ScheduledWake { int64_t epoch; int64_t check; };
static RTC_NOINIT_ATTR ScheduledWake s_scheduledWake;

static int64_t scheduledWakeEpoch() {
  return s_scheduledWake.check == ~s_scheduledWake.epoch ? s_scheduledWake.epoch : 0;
}

void armDeepSleepWake(int64_t targetEpoch) {
  s_scheduledWake = ScheduledWake{targetEpoch, ~targetEpoch};
  if (targetEpoch <= 0) return;
  const int64_t remaining = targetEpoch - (int64_t)::time(nullptr);
  const int64_t stage = core::time::scheduledWakeStageSeconds(remaining);
  // Already inside the lead window: wake almost at once and boot.
  esp_sleep_enable_timer_wakeup((uint64_t)(stage > 0 ? stage : 1) * 1000000ULL);
}

struct SleepInputs { uint8_t port0, port2, check; };
static RTC_NOINIT_ATTR SleepInputs s_sleepInputs;

void rememberSleepInputs(uint8_t port0, uint8_t port2) {
  s_sleepInputs = SleepInputs{port0, port2, (uint8_t)~(port0 ^ port2)};
}

static void resumeDeepSleep() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)IO_EXPANDER_INT_B_PIN,0);
  armDeepSleepWake(scheduledWakeEpoch());
  Serial.flush();
  esp_deep_sleep_start();
}

// TCA6424A has a shared interrupt and no per-input mask. Reject non-button wakes
// before its begin() resets output registers or any accessory rail is enabled.
static void filterDeepSleepWake() {
  const auto cause=esp_sleep_get_wakeup_cause();
  if(cause==ESP_SLEEP_WAKEUP_TIMER) {
    // An early stage of a scheduled wake: go back down before any rail or the
    // screen comes up. Only the final stage continues into a normal boot.
    const int64_t target=scheduledWakeEpoch();
    if(target>0 && core::time::scheduledWakeStageSeconds(target-(int64_t)::time(nullptr))>0) {
      Serial.println("[power-control] scheduled wake stage; sleeping on");
      resumeDeepSleep();
    }
    return;
  }
  if(cause!=ESP_SLEEP_WAKEUP_EXT0) return;
  Wire.begin(SDA_PIN,SCL_PIN);
  Wire.setTimeOut(50);
  auto readPort=[](uint8_t port,uint8_t& value) {
    Wire.beginTransmission(0x22); Wire.write(port);
    if(Wire.endTransmission(false)!=0 || Wire.requestFrom(0x22,1)!=1) return false;
    value=(uint8_t)Wire.read(); return true;
  };
  uint8_t first=0xff,second=0xff;
  const bool firstOk=readPort(1,first);
  delay(15);
  const bool secondOk=readPort(1,second);
  const uint8_t buttons=(1u<<(IOX_BUTTON_A&7))|(1u<<(IOX_BUTTON_B&7))|(1u<<(IOX_BUTTON_C&7));
  if(firstOk && secondOk && (~first&~second&buttons)) {
    Serial.println("[power-control] button accepted after deep sleep");
    return;
  }
  uint8_t port0=0,port2=0,unused;
  const bool port0Ok=readPort(0,port0),port2Ok=readPort(2,port2);
  readPort(1,unused);
  // Nothing but the buttons changed: a quick tap, already released by now.
  const bool remembered=s_sleepInputs.check==(uint8_t)~(s_sleepInputs.port0^s_sleepInputs.port2);
  if(port0Ok && port2Ok && remembered &&
     port0==s_sleepInputs.port0 && port2==s_sleepInputs.port2) {
    Serial.println("[power-control] button tap accepted after deep sleep");
    return;
  }
  if(port0Ok && port2Ok) rememberSleepInputs(port0,port2);
  Serial.println("[power-control] ignored non-button wake; accessories remain off");
  resumeDeepSleep();  // Keeps any scheduled wake; a charger edge must not lose it.
}

bool bringupMenuHardware(bool shippingWakeAuthorized) {
#if defined(ARDUINO_USB_CDC_ON_BOOT)
  Serial.setTxTimeoutMs(5);
  Serial.setTxBufferSize(2048);
#endif
  Serial.begin(115200);
#if defined(ORIGAMI_WAIT_FOR_SERIAL_ON_BOOT)
  // Development-only escape hatch for sessions that need the earliest logs.
  // Consumer firmware must not add a full second before the first UI frame.
  delay(1000);
#endif
  Serial.println("=== Origami Menu ===");
  if (!shippingWakeAuthorized) filterDeepSleepWake();

  // A prior SW1 deep sleep holds shared control pins in safe states. Release
  // those holds before normal bringup reconfigures the display and SD pins.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)LCD_RESET_PIN);
  gpio_hold_dis((gpio_num_t)LCD_CS_PIN);
  gpio_hold_dis((gpio_num_t)SD_CS);

  bool ioOk = false;
  for (uint8_t attempt = 0; attempt < 3 && !ioOk; attempt++) {
    ioOk = hal::io::begin();
    if (!ioOk) delay(150);
  }
  if (!ioOk) {
    Serial.println("FATAL: IO expander init failed");
    return false;
  }
  Serial.println("IO expander OK");
  Serial.printf("[boot-phase] io done at %lu ms\n",(unsigned long)millis());

  hal::io::setPmicHighPower();  // rev4: no-op (no PMIC strap on TCA6424A)
  hal::io::configureDefaultPins();

  // rev4: bring up VSYS FIRST. The VSYS load switch powers the audio codec, the
  // SD card and the 2V8 camera rail, so it must be on before any of the sequence
  // below. (~RESET for the expander is driven high inside hal::io::begin().)
  hal::io::setEnVsys(true);
  // Stagger inrush: configureDefaultPins() switched on the STEMMA rail and we
  // just closed the VSYS switch. Let that transient settle before stacking the
  // 2V8/1V8 regulator and SD-card inrush below. On the 500 mA-limited buck-boost
  // path, overlapping inrush sags +3V3 and can time out the shared I2C bus (263).
  delay(50);
  // Rev 4 routes the IMU, codec, and camera SCCB over the accessory controller.
  // Start it only after VSYS has released the assembled bus high/high.
  Wire1.begin(ACC_SDA_PIN, ACC_SCL_PIN);
  Wire1.setClock(100000);
  Wire1.setTimeOut(50);
  // rev4: input current limit (BQ24040 ISET2) is hard-strapped by R30 — no-op here.
  hal::io::setInputCurrentMode(hal::io::InputCurrentMode::Usb500mA);
  hal::io::enableRegulators(true, true);
  delay(100);

  hal::io::setSdEnable(true);  // rev4: no-op (SD powered from VSYS)
  delay(100);

  pinMode(LCD_CS_PIN, OUTPUT);
  pinMode(SD_CS, OUTPUT);
  digitalWrite(LCD_CS_PIN, HIGH);
  digitalWrite(SD_CS, HIGH);

  // Rev-4 rework routes panel RST (J5.4) to GPIO45. Hold it asserted while
  // cycling the switched LCD rail, then let TFT_eSPI pulse it before init.
  // Keep the backlight OFF through panel init. TFT_eSPI no longer owns TFT_BL
  // (removed from build flags), so the LED stays dark until we ramp it below.
  // Hard-switching the backlight FET (Q2) is the biggest inrush event in bringup;
  // snapping it on during the I2C-critical window is what sags +3V3 (-> 263).
  hal::display::coldResetAndInit();

  // coldResetAndInit() creates the canvas, so hal::display::tft() now redirects
  // to that off-screen buffer. Address the controller directly here; otherwise
  // its white init fill remains visible until the UI task eventually flushes.
  // A deep-sleep wake skips the boot animation, which is what normally covers
  // this fill, so start on Home's white or unpainted rows stay as black bars.
  const bool deepSleepWake = esp_reset_reason() == ESP_RST_DEEPSLEEP;
  hal::display::directTft().fillScreen(deepSleepWake ? TFT_WHITE : TFT_BLACK);
  if (deepSleepWake) hal::display::tft().fillScreen(TFT_WHITE);

  // Keep the backlight off until main_menu has rendered and flushed the first
  // real boot-animation frame. TE is generated by the panel controller and can
  // be validated while the LEDs remain dark.
  delay(120);  // at ~53 Hz, a responsive ST7789 should produce several TE edges
  Serial.printf("Display ready dark (DISP.EN=%d DISP.BL=%d TE_edges=%lu)\n",
                hal::io::displayEnableReadback() ? 1 : 0,
                hal::io::backlightReadback() ? 1 : 0,
                (unsigned long)hal::display::teEdgeCount());

  digitalWrite(LCD_CS_PIN, HIGH);
  digitalWrite(SD_CS, HIGH);
  // SD is optional, but its card and the display share one SPI peripheral.
  // Sample card detect across a settle window for diagnostics, then perform a
  // bounded mount sequence. Attempts are intentionally not gated by card
  // detect: prior failures showed that a timing-sensitive false "absent"
  // decision could skip a healthy card for the whole boot.
  Serial.printf("[boot-phase] display done at %lu ms\n",(unsigned long)millis());
  constexpr uint8_t kSdDetectSamples = 8;
  uint8_t detectLow = 0;
  uint8_t detectHigh = 0;
  uint8_t detectFailed = 0;
  Serial.println("[boot][sd] sampling TCA P27 card detect");
  for (uint8_t sample = 0; sample < kSdDetectSamples; ++sample) {
    uint8_t port2 = 0xFF;
    uint8_t txError = 0xFF;
    uint8_t rxCount = 0;
    const bool ok = hal::io::readExpanderPort(2, &port2, &txError, &rxCount);
    if (!ok) {
      ++detectFailed;
    } else if ((port2 & 0x80u) == 0) {
      ++detectLow;
    } else {
      ++detectHigh;
    }
    Serial.printf("[boot][sd] detect sample=%u ok=%d tx=%u rx=%u port2=0x%02X P27=%d\n",
                  (unsigned)(sample + 1), ok ? 1 : 0, (unsigned)txError,
                  (unsigned)rxCount, (unsigned)port2,
                  ok ? (int)((port2 >> 7) & 1u) : -1);
    delay(20);
  }
  Serial.printf("[boot][sd] detect summary present=%u absent=%u failed=%u\n",
                (unsigned)detectLow, (unsigned)detectHigh,
                (unsigned)detectFailed);

  constexpr uint32_t kBootSdFrequencies[] = {10000000u, 1000000u};
  bool sdMounted = false;
  uint32_t finalFrequency = 0;
  for (uint8_t attempt = 0;
       attempt < (sizeof(kBootSdFrequencies) / sizeof(kBootSdFrequencies[0]));
       ++attempt) {
    if (attempt > 0) {
      // The failed probe has already been fully closed by hal::sd::begin().
      // Give the card additional power-up time before the conservative retry.
      delay(250);
    }
    const uint32_t frequency = kBootSdFrequencies[attempt];
    finalFrequency = frequency;
    Serial.printf("[boot][sd] mount attempt=%u frequency=%lu Hz\n",
                  (unsigned)(attempt + 1), (unsigned long)frequency);
    if (hal::sd::begin(frequency)) {
      sdMounted = true;
      break;
    }
  }
  Serial.printf("[boot][sd] mount=%s frequency=%lu Hz\n",
                hal::sd::mountStateName(), (unsigned long)finalFrequency);

  // Never call the Arduino SD API after a failed/skipped mount. Besides the
  // noisy VFS errors, those calls can issue more transactions on the display's
  // SPI bus while it is recovering from a failed card probe.
  if (sdMounted) {
    Serial.printf("[boot-phase] sd mounted at %lu ms\n",(unsigned long)millis());
    (void)hal::sd::ensureDir("/music");
    (void)hal::sd::ensureDir("/photos");
    (void)hal::sd::ensureDir("/recordings");
    (void)hal::sd::ensureDir("/eyes");
    (void)hal::sd::ensureDir("/boot");
    (void)hal::sd::ensureDir("/qr_debug");
  } else {
    Serial.println("[boot][sd] unavailable; continuing with flash/procedural fallbacks");
  }

  return true;
}

}  // namespace board
}  // namespace core
