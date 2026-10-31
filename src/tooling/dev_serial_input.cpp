#include "hal/hal_encoder_trace.h"
#if defined(KAMI_GAMES) && KAMI_GAMES
#include "apps/tennis_app.h"
#include "apps/bowling_app.h"
#include "apps/golf_app.h"
#endif
#include "tooling/device_api_serial.h"
#include "tooling/lua_runtime_probe.h"
#include "sfx/alarm_catalog.h"
#include "hal/audio_retirement.h"
#include "tooling/dev_serial_input.h"

#ifdef ORIGAMI_DEV_SERIAL_INPUT

#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include "sfx/sfx_typewriter.h"

#include <ctype.h>
#include <math.h>
#include <new>
#include <memory>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <esp_heap_caps.h>
#include <esp_camera.h>
#include <Wire.h>

#include "Pins.h"
#include "app.h"
#include "services/wifi_provision.h"
#include "apps/alarm_clock_app.h"
#include "services/automation.h"
#include "services/shipping.h"
#include "apps/factory_test_app.h"
#include "services/manufacturing_access.h"
#include "core/release_policy.h"
#include "services/factory_test.h"
#include "services/manufacturing.h"
#include "services/device_handoff.h"
#include "control/device_handoff.h"
#include "services/onboarding_progress.h"
#include "control/shipping.h"
#include "services/alarms.h"
#include "core/store.h"
#ifdef MENU_APP
#include "apps/camera_menu_app.h"
#include "apps/doodle_jump_app.h"
#include "apps/trex_app.h"
#include "apps/dumb_flappy_bird_app.h"
#include "apps/hardware_test_app.h"
#include "services/doodle_controls.h"
#include "apps/morning_routine_app.h"
#include "apps/pomodoro_app.h"
#include "apps/agent_app.h"
#include "apps/voice_recorder_app.h"
#include "core/recording_waveform.h"
#include "apps/photos_app.h"
#include "apps/videos_app.h"
#include "apps/tutorial_app.h"
#include "apps/safe_area_app.h"
#include "apps/settings_menu_app.h"
#include "apps/wifi_setup_app.h"
#include "services.h"
#include "ui/home_eyes.h"
#include "services/morning_quote.h"
#endif
#include "widgets/app.h"
#include "services/uploads.h"
#include "services/background_network.h"
#include "services/peripheral_reservation.h"
#include "apps/mp3_menu_app.h"
#include "core/audio_dsp.h"
#include "services/media_volume.h"
#include "core/memory.h"
#include "core/menu_config.h"
#include "services/ota_client.h"
#include "services/firmware_update.h"
#include "core/release_policy.h"
#include "core/mic_tuning.h"
#include "services/mp3_player.h"
#include "services/playback_worker.h"
#include "core/secrets.h"
#include "core/device_id.h"
#include "services/device_fs.h"
#include "services/media_delete.h"
#include "services/net_poll.h"
#include "core/telemetry.h"
#include "apps/agent/agent_flow.h"
#include "services/agent_transport.h"
#include "core/power_button.h"
#include "ui/automation_alert.h"
#include "sfx/audio_group.h"
#include "tooling/vibration_commands.h"
#include "tooling/tls_diagnostics.h"
#include "tooling/morning_diagnostics.h"
#include "core/watchdog.h"
#include "core/wav_writer.h"
#include "hal/hal_audio.h"
#include "hal/hal_bus.h"
#include "hal/hal_camera.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "core/rotary_decoder.h"
#include "hal/hal_io.h"
#include "hal/hal_imu.h"
#include "hal/hal_power.h"
#include "hal/fuel_gauge_workaround.h"
#include "hal/hal_sd.h"
#include "hal/hal_tasks.h"
#include "services/qr_decode.h"
#include "services/wifi_qr_session.h"
#include "hal/hal_wifi.h"
#include "nav_handler.h"
#include "services.h"
#include "sfx/sfx.h"
#include "sfx/sfx_lib.h"
#include "tooling/preview_registry.h"
#include "ui/battery_indicator.h"
#include "ui/scene.h"
#include "ui/morning_eyes.h"

#include <SD.h>

namespace tooling {
namespace dev_serial_input {
namespace {

using hal::input::EventType;

struct ScheduledEvent {
  EventType type = EventType::None;
  int16_t delta = 0;
  uint32_t dueMs = 0;
};

static constexpr uint16_t kLineCap = 320; // Includes a full 223-character quote plus its bench command.
static constexpr uint8_t kScheduleCap = 24;
static constexpr uint16_t kSdPathCap = 160;
static constexpr uint16_t kSdRxChunk = 512;
static constexpr uint16_t kSdRxBudgetPerService = 4096;
static char s_line[kLineCap];
static uint16_t s_lineLen = 0;
static bool s_lineOverflow = false;
static ScheduledEvent s_events[kScheduleCap];
static uint8_t s_head = 0;
static uint8_t s_tail = 0;
static bool s_provisioningRequested = false;
static bool s_provisioningComplete = false;
// Diagnostic-only state must not reserve internal RAM on every boot.
// Retain the owner after first use: a timed-out worker may still reference it.
static services::audio::Mp3Player* s_devMp3=nullptr;

struct SdUploadState {
  std::unique_ptr<UploadQueue::FileMutation> mutation;
  bool active = false;
  char path[kSdPathCap] = {};
  uint32_t expectedSize = 0;
  uint32_t received = 0;
  uint32_t expectedCrc = 0;
  uint32_t crc = 0xFFFFFFFFu;
  File file;
};

static SdUploadState s_sdUpload;
static uint8_t s_sdRxBuf[kSdRxChunk];

struct SdDownloadState {
  bool active = false;
  char path[kSdPathCap] = {};
  uint32_t size = 0;
  uint32_t sent = 0;
  uint32_t crc = 0xFFFFFFFFu;
  File file;
};

struct MicTuneRecordState {
  bool active = false;
  bool backgroundPaused = false;
  uint32_t startedMs = 0;
  uint32_t durationMs = 0;
  char path[kSdPathCap] = {};
  core::WavWriter writer;
  core::audio::mic_tuning::Processor processor;
};

static SdDownloadState s_sdDownload;
static services::background_network::PeripheralReservation s_sdDownloadReservation;
static MicTuneRecordState* s_micTuneRecord = nullptr;

static bool schedule(EventType type, int16_t delta, uint32_t dueMs) {
  const uint8_t next = (uint8_t)((s_head + 1) % kScheduleCap);
  if (next == s_tail) {
    Serial.println("[dev-input] schedule full");
    return false;
  }
  s_events[s_head].type = type;
  s_events[s_head].delta = delta;
  s_events[s_head].dueMs = dueMs;
  s_head = next;
  return true;
}

static void lowerTrim(char* s) {
  if (!s) return;
  char* start = s;
  while (*start && isspace((unsigned char)*start)) start++;
  if (start != s) memmove(s, start, strlen(start) + 1);

  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
  for (size_t i = 0; i < n; i++) s[i] = (char)tolower((unsigned char)s[i]);
}

static void printHelp() {
  Serial.println("[dev-input] commands:");
  Serial.println("  a | b | c          tap button A/B/C");
  Serial.println("  ap/ar bp/br cp/cr  press/release A/B/C");
  Serial.println("  ab | chord         inject A+B chord");
  Serial.println("  input buttons 0..3 test held levels through chord detection (A=1 B=2)");
  Serial.println("  up | down          scroll -1/+1");
  Serial.println("  rot N              scroll by signed N");
  Serial.println("  menu               scroll once from Eyes into menu");
  Serial.println("  morning open|test|status  open settings / preview wake-up / inspect state");
#ifdef MENU_APP
  Serial.println("  morning routine     open the quote/reflection flow directly");
  Serial.println("  morning quote retry|seed|status  fetch, seed, or inspect today's quote");
  Serial.println("  morning quote alarm 180|cancel  one-shot alarm + fresh server quote (120-900 seconds)");
#endif
  Serial.println("  home               pop to root");
  Serial.println("  redraw             mark display dirty");
  Serial.println("  backlight on|off   drive rev-4 DISP.BL through Q2");
  Serial.println("  display on|off     drive rev-4 DISP.EN load switch");
  Serial.println("  display status     show control readbacks, TE level/edge count");
#ifdef MENU_APP
  Serial.println("  display stats      field repaint and canvas transfer counters");
#endif
  Serial.println("  display recover    cold-cycle panel rail and reinitialize");
#ifdef MENU_APP
  Serial.println("  safearea help      live case opening / content padding calibration");
#endif
  Serial.println("  display bitbang    bypass SPI driver; slow raw ST7789 init/test");
  Serial.println("  camera test        init OV3660 at QVGA and acquire one JPEG");
  Serial.println("  camera full        10-frame JPEG + SD integrity + reinit test");
  Serial.println("  camera vga|hd|3mp  clean single-resolution JPEG probe");
  Serial.println("  camera qsweep vga|hd|5mp|all  save JPEG quality comparison set to /qtest");
#ifdef MENU_APP
  Serial.println("  camera open        open the live Camera app");
  Serial.println("  hardware open|status|step audio|camera|recording [INDEX]  inspect isolated Hardware Test steps");
  Serial.println("  photos status      report gallery count, selection, and rendered pixels");
  Serial.println("  camera settle RUN FRAMES REPEAT  VGA trial; FRAMES=-1 for 600ms baseline");
#endif
  Serial.println("  battery icon PERCENT 0|1 [-1|0|1] | off | status (display-only, 120s)");
  Serial.println("  battery test       read BQ27220 SOC/voltage/current/capacity");
  Serial.println("  battery mode host|gauge   select displayed SOC (saved)");
  Serial.println("  battery scale -10        signed current multiplier, |scale|=0.1..20 (saved)");
  Serial.println("  battery capacity 1000    host capacity, 100..20000 mAh (saved)");
  Serial.println("  battery soc 100          anchor host estimate to a KNOWN charge, 0..100%");
  Serial.println("  battery reset            re-seed HOST estimate from voltage; no gauge writes");
  Serial.println("  battery watch on|off     print raw/corrected telemetry at 1 Hz");
  Serial.println("  battery history          dump last 10 minutes (RAM; lost on reset)");
  Serial.println("  mic test           capture and measure two seconds of raw microphone audio");
  Serial.println("  mic settings       show live WAV-recorder microphone tuning");
  Serial.println("  mic gain X         set digital gain 0.00..4.00x");
  Serial.println("  mic hpf off|HZ     set high-pass filter off or 20..500 Hz");
  Serial.println("  mic limiter off|DB set hard limiter ceiling -20..0 dBFS");
  Serial.println("  mic defaults|save  restore defaults or persist live settings");
  Serial.println("  mic record SEC [LABEL]  record a 1..30 second tuning WAV");
  Serial.println("  mic status|cancel  show or stop command-driven recording");
  Serial.println("  imu test           sample accelerometer and gyroscope ranges");
  Serial.println("  imu int test       verify IMU INT1 through the IO expander");
  Serial.println("  vibration test     run vibration motor for one second");
  Serial.println("  input status       show physical buttons and rotary GPIO state");
  Serial.println("  ee inputs          sample all EE checklist inputs for 8 seconds");
  Serial.println("  ee outputs         exercise safe expander-controlled outputs");
  Serial.println("  io status|reinit   dump TCA registers or simulate I2C-bus recovery");
  Serial.println("  tasks              dump live task stack high-water marks");
  Serial.println("  audio test         play a 440 Hz speaker tone for two seconds");
  Serial.println("  headphone audio    play a 440 Hz headphone-only tone for two seconds");
  Serial.println("  headphone test     detect jack insertion/removal for 8 seconds");
  Serial.println("  audio output [auto|speaker|aux|both]  show or save the output mode");
  Serial.println("  music status       inspect production playback state without changing it");
  Serial.println("  playback delay MS  one-shot worker acknowledgment delay (0..5000; 0 cancels)");
  Serial.println("  playback status    inspect delayed acknowledgment, cleanup and audio generations");
  Serial.println("  sd status          read-only card, mount and upload readiness");
  Serial.println("  sd test            detect, mount, list, and verify SD read/write/delete");
  Serial.println("  music test         play the first MP3 on SD through the speaker");
  Serial.println("  mp3 gain PCT       set Music + recordings PCM gain 0..100; 'mp3 gain off' restores profile");
  Serial.println("  mp3 volume         show shared media speaker/headphone volume profiles");
  Serial.println("  mp3 volume ROUTE MIN MAX STEPS  set media equal-dB profile live; ROUTE=speaker|headphone");
  Serial.println("  mp3 volume save    persist media profiles used by Music + Audio Recordings");
  Serial.println("  sfx volume [0..100] show or set device sound-effect volume");
  Serial.println("  sfx volume save    persist current device sound-effect volume");
  Serial.println("  morning sound N    play one installed alarm motif (N=0..3)");
  Serial.println("  morning test N     repeat one installed alarm with the real wake screen");
  Serial.println("  morning stop       stop a Studio alarm preview");
  Serial.println("  doodle tune        show live Doodle Jump IMU control tuning");
  Serial.println("  doodle speed PX    set max Doodle horizontal speed live");
  Serial.println("  doodle dz ZERO SOFT  set dead zones in degrees");
  Serial.println("  doodle soft FRAC   set soft-zone speed fraction");
  Serial.println("  doodle sign +/-1   flip Doodle roll steering direction");
  Serial.println("  doodle save        persist current Doodle tuning across reboot");
  Serial.println("  doodle defaults    restore default Doodle controls");
  Serial.println("  display red|green|blue|white|black  direct panel fill");
  Serial.println("  tls probe HOST PORT verify a TLS handshake without application data");
  Serial.println("  widget stats       observe Lua draws and framebuffer transfers");
  Serial.println("  scene              dump current scene nodes");
  Serial.println("  power boot         report deep-sleep reset and RTC timer wake");
  Serial.println("  power shutdown-test  REAL accessory shutdown/deep sleep; USB wake in 8 seconds");
  Serial.println("  power self-test    check C click/hold and wake policy");
  Serial.println("  pomodoro open|status|test N  inspect / shorten running interval to 1..10s");
  Serial.println("  power test-wake MS arm a temporary USB sleep test wake (3000..15000)");
  Serial.println("  scene set ID PROP VALUE  override node property");
  Serial.println("  scene clear        clear scene overrides");
  Serial.println("  preview apps       list previewable apps");
  Serial.println("  preview screens APP");
  Serial.println("  preview inputs APP [SCREEN]");
  Serial.println("  preview scene APP [SCREEN]  build + dump preview scene");
  Serial.println("  preview show APP [SCREEN]   build + render preview scene");
  Serial.println("  wifi status        show current WiFi state");
  Serial.println("  wifi list          list saved SSIDs");
  Serial.println("  wifi add SSID PASS save STA credential + connect");
  Serial.println("  wifi add JSON      provision JSON credentials (supports spaces)");
  Serial.println("  wifi connect       reload saved creds + connect");
#ifdef MENU_APP
  Serial.println("  wifi qrtest saved|account|missing  test active scanner's post-decode join");
  Serial.println("  wifi qropen|qrstatus|qrguardtest  scanner bench diagnostics");
  Serial.println("  wifi qrstopdelay MS  one-shot idle shutdown delay (0..10000)");
  Serial.println("  wifi qrfail jobqueue|resultqueue|task|off  one-shot idle startup fault");
#endif
  Serial.println("  backend status     show backend URL");
  Serial.println("  backend set URL    set URL (persistent only in dev mode)");
  Serial.println("  backend mode dev|prod  same as Settings > Server");
  Serial.println("  api status         show API key state");
  Serial.println("  api set KEY        persist API key");
  Serial.println("  ota auth status    show OTA credential state");
  Serial.println("  ota auth set TOKEN persist OTA access key (64 hex)");
  Serial.println("  ota auth clear     remove persisted OTA token");
  Serial.println("  ota status         show OTA build, hardware, channel, and hold");
  Serial.println("  ota channel NAME   select public/private/staging using normal access checks");
  Serial.println("  queue status       show upload order, attempts, turn, and cooldown");
  Serial.println("  queue enqueue image|audio /upload_queue/test-NAME  enqueue retained test media");
  Serial.println("  queue enqueue video /videos/test-NAME.avi         enqueue retained video fixture");
  Serial.println("  queue clear        clear live upload queue + persisted manifest");
  Serial.println("  provision begin    enter boot-time provisioning mode");
  Serial.println("  provision done     leave boot-time provisioning mode");
  Serial.println("  sdput SIZE CRC PATH  upload one binary file to SD");
  Serial.println("  sdget PATH           download a file as CRC-checked base64 chunks");
  Serial.println("  sddelete PATH        delete one file through guarded media deletion");
  Serial.println("  sdclear PATH         delete and recreate one SD directory");
}

static void rawDisplayByte(uint8_t value) {
  for (int bit = 7; bit >= 0; --bit) {
    digitalWrite(SPI_SCK, LOW);
    digitalWrite(SPI_MOSI, (value >> bit) & 1u);
    delayMicroseconds(2);
    digitalWrite(SPI_SCK, HIGH);
    delayMicroseconds(2);
  }
  digitalWrite(SPI_SCK, LOW);
}

static void rawDisplayCommand(uint8_t command, const uint8_t* data = nullptr, size_t len = 0) {
  digitalWrite(LCD_CS_PIN, LOW);
  digitalWrite(LCD_DC_PIN, LOW);
  rawDisplayByte(command);
  digitalWrite(LCD_DC_PIN, HIGH);
  for (size_t i = 0; i < len; ++i) rawDisplayByte(data[i]);
  digitalWrite(LCD_CS_PIN, HIGH);
}

static uint32_t countTeRisingEdges(uint32_t durationMs) {
  uint32_t edges = 0;
  int previous = digitalRead(LCD_TEAR_PIN);
  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < durationMs) {
    const int current = digitalRead(LCD_TEAR_PIN);
    if (current == HIGH && previous == LOW) ++edges;
    previous = current;
    delayMicroseconds(50);
  }
  return edges;
}

static void runRawDisplayTest() {
  hal::display::setBacklight(false);
  hal::display::setEnabled(false);
  pinMode(LCD_CS_PIN, OUTPUT);
  pinMode(LCD_DC_PIN, OUTPUT);
  pinMode(SPI_MOSI, OUTPUT);
  pinMode(SPI_SCK, OUTPUT);
  digitalWrite(LCD_CS_PIN, LOW);
  digitalWrite(LCD_DC_PIN, LOW);
  digitalWrite(SPI_MOSI, LOW);
  digitalWrite(SPI_SCK, LOW);
  delay(1000);
  digitalWrite(LCD_CS_PIN, HIGH);
  hal::display::setEnabled(true);
  delay(200);

  rawDisplayCommand(0x01);  // SWRESET
  delay(150);
  rawDisplayCommand(0x11);  // SLPOUT
  delay(150);
  const uint8_t colmod[] = {0x55};
  rawDisplayCommand(0x3A, colmod, sizeof(colmod));  // RGB565
  const uint8_t madctl[] = {0x00};
  rawDisplayCommand(0x36, madctl, sizeof(madctl));
  rawDisplayCommand(0x21);  // INVON
  const uint8_t teon[] = {0x00};
  rawDisplayCommand(0x35, teon, sizeof(teon));
  rawDisplayCommand(0x29);  // DISPON
  delay(100);

  const uint32_t teEdges = countTeRisingEdges(250);
  Serial.printf("[display-dev] raw init TE edges/250ms=%lu\n", (unsigned long)teEdges);

  const uint8_t columns[] = {0x00, 0x00, 0x00, 0xEF};
  const uint8_t rows[] = {0x00, 0x00, 0x01, 0x17};
  rawDisplayCommand(0x2A, columns, sizeof(columns));
  rawDisplayCommand(0x2B, rows, sizeof(rows));
  digitalWrite(LCD_CS_PIN, LOW);
  digitalWrite(LCD_DC_PIN, LOW);
  rawDisplayByte(0x2C);  // RAMWR
  digitalWrite(LCD_DC_PIN, HIGH);
  // Three horizontal bands: red, green, blue. This intentionally writes every
  // pixel so success is visible without relying on the existing frame buffer.
  for (uint32_t pixel = 0; pixel < 240u * 280u; ++pixel) {
    uint16_t color = pixel < 240u * 93u ? 0xF800 :
                     pixel < 240u * 186u ? 0x07E0 : 0x001F;
    rawDisplayByte((uint8_t)(color >> 8));
    rawDisplayByte((uint8_t)color);
  }
  digitalWrite(LCD_CS_PIN, HIGH);
  hal::display::setBacklight(true);
  Serial.println("[display-dev] raw RGB bands complete");
}

static void runAudioTest(hal::audio::OutputRoute route =
                             hal::audio::OutputRoute::SpeakerOnly) {
  const bool headphones = route == hal::audio::OutputRoute::HeadphonesOnly;
  Serial.printf("[audio-dev] 440 Hz %s test starting\n",
                headphones ? "headphone-only" : "speaker");
  hal::audio::txEnd();
  hal::audio::hardwareReset();
  const bool codecOk = hal::audio::initCodec(0x18);
  const bool playbackOk = codecOk && hal::audio::configureForPlayback();
  if (playbackOk) {
    hal::audio::setOutputRoute(route);
  }
  const bool txOk = playbackOk && hal::audio::txBegin(44100);
  Serial.printf("[audio-dev] codec=%d playback=%d i2s_tx=%d\n",
                codecOk ? 1 : 0, playbackOk ? 1 : 0, txOk ? 1 : 0);
  if (!txOk) {
    hal::audio::txEnd();
    return;
  }

  int16_t samples[256];
  float phase = 0.0f;
  size_t totalBytes = 0;
  const uint32_t startMs = millis();
  while ((uint32_t)(millis() - startMs) < 2000u) {
    const float peakGain = headphones ? 0.5f : services::audio::media_volume::kSpeakerMaxGainBasisPoints / 10000.0f;
    core::dsp::generateSine(samples, 256, phase, 440.0f, 44100.0f, peakGain);
    size_t bytesWritten = 0;
    const esp_err_t err = hal::audio::txWrite(samples, 256, &bytesWritten,
                                               pdMS_TO_TICKS(100));
    totalBytes += bytesWritten;
    if (err != ESP_OK) {
      Serial.printf("[audio-dev] write failed err=%d after %u bytes\n",
                    (int)err, (unsigned)totalBytes);
      break;
    }
  }
  hal::audio::txEnd();
  Serial.printf("[audio-dev] tone complete bytes=%u\n", (unsigned)totalBytes);
}

static void printEeInputs(uint32_t elapsed, uint8_t p0, uint8_t p1, uint8_t p2,
                          int rotA, int rotB, int irq, bool irqLatched) {
  Serial.printf("[ee-inputs] t=%lu P0=%02X P1=%02X P2=%02X "
                "IMU_INT=%d AUDIO_INT=%d PWRSW=%d C=%d B=%d A=%d "
                "PG=%d CHG=%d SD_DET_N=%d ROT=%d%d TCA_INT_N=%d latched=%d\n",
                (unsigned long)elapsed, p0, p1, p2,
                (p0 >> (IOX_IMU_INT1 & 7)) & 1,
                (p0 >> (IOX_AUDIO_INT & 7)) & 1,
                (p1 >> (IOX_PWRSW & 7)) & 1,
                (p1 >> (IOX_BUTTON_C & 7)) & 1,
                (p1 >> (IOX_BUTTON_B & 7)) & 1,
                (p1 >> (IOX_BUTTON_A & 7)) & 1,
                (p2 >> (IOX_CHARGER_PG & 7)) & 1,
                (p2 >> (IOX_CHARGER_CHG & 7)) & 1,
                (p2 >> (IOX_SD_DETECT & 7)) & 1,
                rotA, rotB, irq, irqLatched ? 1 : 0);
}

static void runEeInputTest() {
  Serial.println("[ee-inputs] actuate A/B/C, encoder, PWRSW; shake board; remove/reinsert SD");
  uint8_t old0 = 0, old1 = 0, old2 = 0;
  bool haveOld = false;
  int oldRotA = -1, oldRotB = -1, oldIrq = -1;
  const uint32_t startMs = millis();
  while ((uint32_t)(millis() - startMs) < 8000u) {
    uint8_t p0 = 0, p1 = 0, p2 = 0;
    if (!hal::io::readExpanderInputs(&p0, &p1, &p2)) {
      delay(20);
      continue;
    }
    const int rotA = digitalRead(ROTARY_A_PIN);
    const int rotB = digitalRead(ROTARY_B_PIN);
    const int irq = digitalRead(IO_EXPANDER_INT_B_PIN);
    const bool latched = hal::io::buttonInterruptPending();
    if (!haveOld || p0 != old0 || p1 != old1 || p2 != old2 ||
        rotA != oldRotA || rotB != oldRotB || irq != oldIrq || latched) {
      printEeInputs(millis() - startMs, p0, p1, p2, rotA, rotB, irq, latched);
      old0 = p0; old1 = p1; old2 = p2;
      oldRotA = rotA; oldRotB = rotB; oldIrq = irq;
      haveOld = true;
    }
    delay(10);
  }
  Serial.println("[ee-inputs] done");
}

static void runImuInterruptTest() {
  Serial.println("[imu-int] routing accelerometer data-ready to INT1");
  if (!hal::imu::isReady() || !hal::imu::setAccelDataReadyInterrupt(true)) {
    Serial.println("[imu-int] FAIL: could not enable data-ready interrupt");
    return;
  }

  bool sawHigh = false;
  bool sawLowAfterRead = false;
  uint8_t p0 = 0, p1 = 0, p2 = 0;
  const uint32_t highDeadline = millis() + 250;
  while ((int32_t)(millis() - highDeadline) < 0) {
    if (hal::io::readExpanderInputs(&p0, &p1, &p2) &&
        ((p0 >> (IOX_IMU_INT1 & 7)) & 1)) {
      sawHigh = true;
      break;
    }
    delay(2);
  }

  int32_t accel[3] = {};
  int32_t gyro[3] = {};
  const bool readOk = hal::imu::readAxes(accel, gyro);
  // At 416 Hz the next sample can reassert DRDY before the slower TCA I2C read
  // observes the clear. Disable only our temporary source, then verify release.
  const bool restoreOk = hal::imu::setAccelDataReadyInterrupt(false);
  const uint32_t lowDeadline = millis() + 50;
  while ((int32_t)(millis() - lowDeadline) < 0) {
    if (hal::io::readExpanderInputs(&p0, &p1, &p2) &&
        !((p0 >> (IOX_IMU_INT1 & 7)) & 1)) {
      sawLowAfterRead = true;
      break;
    }
    delay(1);
  }

  Serial.printf("[imu-int] high=%d low-after-read=%d sample=%d restore=%d "
                "accel=%ld,%ld,%ld result=%s\n",
                sawHigh ? 1 : 0, sawLowAfterRead ? 1 : 0, readOk ? 1 : 0,
                restoreOk ? 1 : 0, (long)accel[0], (long)accel[1],
                (long)accel[2],
                (sawHigh && sawLowAfterRead && readOk && restoreOk) ? "PASS"
                                                                   : "FAIL");
}

static bool probeAccessoryAddress(uint8_t address) {
  if (!hal::bus::lockI2c(pdMS_TO_TICKS(250))) return false;
  Wire1.beginTransmission(address);
  const bool ack = Wire1.endTransmission(true) == 0;
  hal::bus::unlockI2c();
  return ack;
}

static void printEeOutputPins(const char* tag) {
  uint8_t p0 = 0, p1 = 0, p2 = 0;
  const bool ok = hal::io::readExpanderInputs(&p0, &p1, &p2);
  Serial.printf("[ee-outputs] %s ok=%d 2V8=%d VIBE=%d STEMMA=%d 1V8=%d "
                "AUDIO_RST_N=%d VSYS_EN=%d DISP_EN=%d CAM_RST_N=%d "
                "CAM_PWDN=%d DISP_BL=%d\n",
                tag, ok ? 1 : 0,
                (p0 >> (IOX_REGULATOR_2V8_EN & 7)) & 1,
                (p0 >> (IOX_VIBRATION_MOTOR & 7)) & 1,
                (p0 >> (IOX_STEMMA_POWER_EN & 7)) & 1,
                (p0 >> (IOX_REGULATOR_1V8_EN & 7)) & 1,
                (p1 >> (IOX_AUDIO_RESET & 7)) & 1,
                (p2 >> (IOX_EN_VSYS & 7)) & 1,
                (p2 >> (IOX_DISPLAY_EN & 7)) & 1,
                (p2 >> (IOX_CAMERA_RESET & 7)) & 1,
                (p2 >> (IOX_CAMERA_PWDN & 7)) & 1,
                (p1 >> (IOX_DISPLAY_BL & 7)) & 1);
}

static void runEeOutputTest() {
  Serial.println("[ee-outputs] safe output exercise starting");
  printEeOutputPins("initial");

  hal::io::setStemmaPower(false);
  printEeOutputPins("stemma-off");
  hal::io::setStemmaPower(true);
  printEeOutputPins("stemma-on");

  hal::io::setVibration(true);
  printEeOutputPins("vibe-on");
  delay(750);
  hal::io::setVibration(false);
  printEeOutputPins("vibe-off");

  hal::io::setCameraPwdn(true);
  hal::io::setCameraReset(false);
  hal::io::enableRegulators(false, false);
  printEeOutputPins("camera-off-reset");
  hal::io::enableRegulators(true, true);
  delay(20);
  hal::io::setCameraReset(true);
  hal::io::setCameraPwdn(false);
  printEeOutputPins("camera-on-release");

  const bool codecBefore = probeAccessoryAddress(0x18);
  hal::io::setAudioReset(true);
  delay(10);
  const bool codecDuringReset = probeAccessoryAddress(0x18);
  printEeOutputPins("audio-reset-asserted");
  hal::io::setAudioReset(false);
  delay(100);
  const bool codecAfter = probeAccessoryAddress(0x18);
  printEeOutputPins("audio-reset-released");

  Serial.printf("[ee-outputs] codec ACK before/reset/after=%d/%d/%d\n",
                codecBefore ? 1 : 0, codecDuringReset ? 1 : 0,
                codecAfter ? 1 : 0);
  Serial.println("[ee-outputs] done (VSYS/DISP outputs intentionally not cycled)");
}

static const char* headsetStatusName(uint8_t status) {
  switch (status) {
    case TLV320_HEADSET_NONE: return "NONE";
    case TLV320_HEADSET_WITHOUT_MIC: return "HEADPHONES";
    case TLV320_HEADSET_WITH_MIC: return "HEADSET_WITH_MIC";
    default: return "INVALID";
  }
}

static const char* outputRouteName(hal::audio::OutputRoute route) {
  switch (route) {
    case hal::audio::OutputRoute::Auto: return "auto";
    case hal::audio::OutputRoute::SpeakerOnly: return "speaker";
    case hal::audio::OutputRoute::HeadphonesOnly: return "aux";
    default: return "both";
  }
}

static void printOutputRoute() {
  uint8_t hp = 0, spk = 0;
  const bool regsOk = hal::audio::readRegister(1, 31, &hp) &&
                      hal::audio::readRegister(1, 32, &spk);
  Serial.printf("[audio-output] mode=%s effective=%s jack=%u regsOk=%d HP=0x%02X SPK=0x%02X\n",
                outputRouteName(hal::audio::getOutputRoute()),
                outputRouteName(hal::audio::getEffectiveOutputRoute()),
                hal::audio::headsetStatus(), regsOk ? 1 : 0, hp, spk);
}

static void runHeadphoneTest() {
  Serial.println("[headphone-test] starting; remove/reinsert plug during 8s window");
  hal::audio::txEnd();
  hal::audio::hardwareReset();
  const bool codecOk = hal::audio::initCodec(0x18);
  const bool playbackOk = codecOk && hal::audio::configureForPlayback();
  // The production codec PLL is sourced from BCLK, so keep I2S TX running while
  // the analog jack detector settles. tx_desc_auto_clear supplies silence.
  const bool clockOk = playbackOk && hal::audio::txBegin(44100);
  const bool detectOk = clockOk && hal::audio::configureHeadsetDetection();
  Serial.printf("[headphone-test] codec=%d playback=%d clock=%d detector=%d\n",
                codecOk ? 1 : 0, playbackOk ? 1 : 0, clockOk ? 1 : 0,
                detectOk ? 1 : 0);
  if (!detectOk) {
    hal::audio::txEnd();
    return;
  }
  delay(500);

  uint8_t previous = 0xFF;
  uint8_t previousDetectReg = 0xFF;
  uint8_t previousStatusReg = 0xFF;
  const uint32_t startMs = millis();
  // Keep this bounded below the 15-second task watchdog. This command runs on
  // the UI/service path, so a 30-second synchronous diagnostic caused a real
  // watchdog reset even though the audio hardware itself was healthy.
  while ((uint32_t)(millis() - startMs) < 8000u) {
    const uint8_t status = hal::audio::headsetStatus();
    const uint8_t flags = hal::audio::headsetIrqFlags(true);
    uint8_t detectReg = 0xFF;
    uint8_t statusReg = 0xFF;
    uint8_t micbiasReg = 0xFF;
    uint8_t timerReg = 0xFF;
    const bool regsOk = hal::audio::readRegister(0, 67, &detectReg) &&
                        hal::audio::readRegister(0, 46, &statusReg) &&
                        hal::audio::readRegister(1, 46, &micbiasReg) &&
                        hal::audio::readRegister(3, 16, &timerReg);
    if (status != previous || flags != 0 || detectReg != previousDetectReg ||
        statusReg != previousStatusReg) {
      Serial.printf("[headphone-test] t=%lu status=%u %s irq=0x%02X "
                    "regsOk=%d P0R67=0x%02X P0R46=0x%02X "
                    "P1R46=0x%02X P3R16=0x%02X\n",
                    (unsigned long)(millis() - startMs), status,
                    headsetStatusName(status), flags, regsOk ? 1 : 0,
                    detectReg, statusReg, micbiasReg, timerReg);
      previous = status;
      previousDetectReg = detectReg;
      previousStatusReg = statusReg;
    }
    delay(100);
  }
  Serial.printf("[headphone-test] done final=%u %s\n", previous,
                headsetStatusName(previous));
  hal::audio::txEnd();
}

static const char* sdTypeName(uint8_t type) {
  switch (type) {
    case CARD_MMC: return "MMC";
    case CARD_SD: return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    case CARD_NONE: return "none";
    default: return "unknown";
  }
}

static bool mountSdForTest() {
  const bool detected = hal::io::sdDetect();
  Serial.printf("[sd-dev] detect=%d (active-low socket switch)\n", detected ? 1 : 0);
  if (!detected) {
    Serial.println("[sd-dev] FAIL: card is not detected");
    return false;
  }

  // The display and SD card share SPI, but each has an independent chip select;
  // never change panel power as a side effect of an SD diagnostic. Reuse a
  // production mount because SD.end()/begin() would invalidate File objects
  // owned by telemetry/widgets. Only mount when inserted after boot.
  const bool reused = hal::sd::mounted();
  const bool mounted = reused || hal::sd::begin(10000000);
  Serial.printf("[sd-dev] mount=%d reused=%d spi_hz=10000000\n",
                mounted ? 1 : 0, reused ? 1 : 0);
  return mounted;
}

static int findMp3Files(char* paths, int maxFiles, int pathLen) {
  int count = hal::sd::listFiles("/music", ".mp3", paths, maxFiles, pathLen);
  if (count == 0) count = hal::sd::listFiles("/", ".mp3", paths, maxFiles, pathLen);
  return count;
}

static bool runSdTest() {
  Serial.println("[sd-dev] full test starting");
  if (!mountSdForTest()) return false;

  hal::bus::lockSpi();
  const uint8_t type = SD.cardType();
  const uint64_t cardBytes = SD.cardSize();
  const uint64_t totalBytes = SD.totalBytes();
  const uint64_t usedBytes = SD.usedBytes();
  hal::bus::unlockSpi();
  Serial.printf("[sd-dev] type=%s card=%llu MiB fs_total=%llu MiB used=%llu MiB\n",
                sdTypeName(type),
                (unsigned long long)(cardBytes / (1024u * 1024u)),
                (unsigned long long)(totalBytes / (1024u * 1024u)),
                (unsigned long long)(usedBytes / (1024u * 1024u)));

  static constexpr int kMaxMp3 = 24;
  static constexpr int kPathLen = 192;
  char paths[kMaxMp3 * kPathLen] = {};
  const int mp3Count = findMp3Files(paths, kMaxMp3, kPathLen);
  Serial.printf("[sd-dev] mp3_files=%d%s\n", mp3Count,
                mp3Count == kMaxMp3 ? " (listing capped)" : "");
  for (int i = 0; i < mp3Count; ++i) {
    Serial.printf("[sd-dev] mp3[%d]=%s\n", i, paths + i * kPathLen);
  }

  static const char kTestPath[] = "/origami_n16r8_sd_test.tmp";
  static const char kPayload[] = "origami-n16r8-sd-rw-2026-07-28\n";
  if (SD.exists(kTestPath)) (void)hal::sd::removeFile(kTestPath);
  const bool writeOk = hal::sd::writeFile(
      kTestPath, reinterpret_cast<const uint8_t*>(kPayload), strlen(kPayload));
  char readback[96] = {};
  const size_t readLen = writeOk ? hal::sd::readFile(kTestPath, readback, sizeof(readback)) : 0;
  const bool readOk = readLen == strlen(kPayload) && memcmp(readback, kPayload, readLen) == 0;
  const bool deleteOk = hal::sd::removeFile(kTestPath);
  const bool gone = !SD.exists(kTestPath);
  Serial.printf("[sd-dev] write=%d read=%d bytes=%u delete=%d gone=%d\n",
                writeOk ? 1 : 0, readOk ? 1 : 0, (unsigned)readLen,
                deleteOk ? 1 : 0, gone ? 1 : 0);
  const bool pass = type != CARD_NONE && cardBytes > 0 && writeOk && readOk && deleteOk && gone;
  Serial.printf("[sd-dev] RESULT=%s\n", pass ? "PASS" : "FAIL");
  return pass;
}

static bool runCameraFullTest() {
  static constexpr char kPath[] = "/camera_full_test.jpg";
  Serial.println("[camera-full] starting");
  core::memory::logSnapshot("camera-full-before");

  bool initOk = hal::camera::init(HAL_CAMERA_FRAMESIZE_QVGA,
                                   HAL_CAMERA_PIXFORMAT_JPEG);
  unsigned goodFrames = 0;
  size_t minBytes = SIZE_MAX;
  size_t maxBytes = 0;
  uint32_t signature = 2166136261u;
  for (unsigned i = 0; initOk && i < 10; ++i) {
    hal::camera::CameraFrame frame{};
    if (!hal::camera::getFrame(&frame)) break;
    const bool dimensionsOk = frame.width == 320 && frame.height == 240;
    const bool jpegOk = frame.len >= 4 && frame.buf[0] == 0xFF &&
                        frame.buf[1] == 0xD8 &&
                        frame.buf[frame.len - 2] == 0xFF &&
                        frame.buf[frame.len - 1] == 0xD9;
    if (dimensionsOk && jpegOk) {
      ++goodFrames;
      if (frame.len < minBytes) minBytes = frame.len;
      if (frame.len > maxBytes) maxBytes = frame.len;
      for (size_t j = 0; j < frame.len; j += 97) {
        signature = (signature ^ frame.buf[j]) * 16777619u;
      }
    }
    Serial.printf("[camera-full] frame=%u bytes=%u dims=%d jpeg=%d\n",
                  i + 1, (unsigned)frame.len, dimensionsOk ? 1 : 0,
                  jpegOk ? 1 : 0);
    hal::camera::returnFrame();
  }

  bool sdWriteOk = false;
  bool sdReadOk = false;
  size_t sdBytes = 0;
  if (initOk && hal::sd::mounted()) {
    hal::bus::lockSpi();
    SD.remove(kPath);
    sdWriteOk = hal::camera::captureToFile(kPath);
    if (sdWriteOk) {
      File f = SD.open(kPath, FILE_READ);
      if (f) {
        sdBytes = f.size();
        const int soi0 = f.read();
        const int soi1 = f.read();
        int eoi0 = -1;
        int eoi1 = -1;
        if (sdBytes >= 4 && f.seek(sdBytes - 2)) {
          eoi0 = f.read();
          eoi1 = f.read();
        }
        sdReadOk = sdBytes >= 4 && soi0 == 0xFF && soi1 == 0xD8 &&
                   eoi0 == 0xFF && eoi1 == 0xD9;
        f.close();
      }
    }
    SD.remove(kPath);
    hal::bus::unlockSpi();
  }
  hal::camera::deinit();

  struct ResolutionProbe {
    framesize_t size;
    uint16_t width;
    uint16_t height;
    const char* name;
  };
  static const ResolutionProbe kResolutions[] = {
      {FRAMESIZE_VGA, 640, 480, "VGA"},
      {FRAMESIZE_HD, 1280, 720, "HD"},
      {FRAMESIZE_QXGA, 2048, 1536, "3MP"},
  };
  bool resolutionsOk = true;
  for (const ResolutionProbe& probe : kResolutions) {
    const bool resInit = hal::camera::init(probe.size, PIXFORMAT_JPEG);
    bool resFrame = false;
    size_t resBytes = 0;
    uint16_t actualWidth = 0;
    uint16_t actualHeight = 0;
    uint8_t soi0 = 0, soi1 = 0, eoi0 = 0, eoi1 = 0;
    if (resInit) {
      hal::camera::CameraFrame frame{};
      if (hal::camera::getFrame(&frame)) {
        resBytes = frame.len;
        actualWidth = frame.width;
        actualHeight = frame.height;
        if (frame.len >= 4) {
          soi0 = frame.buf[0];
          soi1 = frame.buf[1];
          eoi0 = frame.buf[frame.len - 2];
          eoi1 = frame.buf[frame.len - 1];
        }
        resFrame = frame.width == probe.width && frame.height == probe.height &&
                   frame.len >= 4 && frame.buf[0] == 0xFF && frame.buf[1] == 0xD8 &&
                   frame.buf[frame.len - 2] == 0xFF && frame.buf[frame.len - 1] == 0xD9;
        hal::camera::returnFrame();
      }
    }
    hal::camera::deinit();
    resolutionsOk = resolutionsOk && resInit && resFrame;
    Serial.printf("[camera-full] resolution=%s init=%d frame=%d actual=%ux%u "
                  "bytes=%u markers=%02X%02X/%02X%02X\n",
                  probe.name, resInit ? 1 : 0, resFrame ? 1 : 0,
                  actualWidth, actualHeight, (unsigned)resBytes,
                  soi0, soi1, eoi0, eoi1);
  }

  const bool reinitOk = hal::camera::init(HAL_CAMERA_FRAMESIZE_QVGA,
                                           HAL_CAMERA_PIXFORMAT_JPEG);
  bool recoveryFrameOk = false;
  if (reinitOk) {
    hal::camera::CameraFrame frame{};
    recoveryFrameOk = hal::camera::getFrame(&frame) && frame.len >= 4 &&
                      frame.buf[0] == 0xFF && frame.buf[1] == 0xD8 &&
                      frame.buf[frame.len - 2] == 0xFF &&
                      frame.buf[frame.len - 1] == 0xD9;
    hal::camera::returnFrame();
  }
  hal::camera::deinit();
  core::memory::logSnapshot("camera-full-after");

  const bool pass = initOk && goodFrames == 10 && sdWriteOk && sdReadOk &&
                    resolutionsOk && reinitOk && recoveryFrameOk;
  Serial.printf("[camera-full] result=%s frames=%u range=%u..%u sig=%08lx "
                "sdWrite=%d sdRead=%d sdBytes=%u resolutions=%d reinit=%d recovery=%d\n",
                pass ? "PASS" : "FAIL", goodFrames,
                goodFrames ? (unsigned)minBytes : 0u, (unsigned)maxBytes,
                (unsigned long)signature, sdWriteOk ? 1 : 0,
                sdReadOk ? 1 : 0, (unsigned)sdBytes, resolutionsOk ? 1 : 0,
                reinitOk ? 1 : 0,
                recoveryFrameOk ? 1 : 0);
  return pass;
}

static bool runCameraResolutionTest(framesize_t size, uint16_t expectedWidth,
                                    uint16_t expectedHeight, const char* name) {
  Serial.printf("[camera-res] %s starting\n", name);
  core::memory::logSnapshot("camera-res-before");
  const bool initOk = hal::camera::init(size, PIXFORMAT_JPEG);
  bool frameOk = false;
  size_t bytes = 0;
  uint16_t width = 0, height = 0;
  if (initOk) {
    hal::camera::CameraFrame frame{};
    if (hal::camera::getFrame(&frame)) {
      width = frame.width;
      height = frame.height;
      bytes = frame.len;
      frameOk = width == expectedWidth && height == expectedHeight &&
                frame.len >= 4 && frame.buf[0] == 0xFF && frame.buf[1] == 0xD8 &&
                frame.buf[frame.len - 2] == 0xFF && frame.buf[frame.len - 1] == 0xD9;
      hal::camera::returnFrame();
    }
  }
  hal::camera::deinit();
  core::memory::logSnapshot("camera-res-after");
  Serial.printf("[camera-res] %s result=%s init=%d frame=%d actual=%ux%u bytes=%u\n",
                name, (initOk && frameOk) ? "PASS" : "FAIL", initOk ? 1 : 0,
                frameOk ? 1 : 0, width, height, (unsigned)bytes);
  return initOk && frameOk;
}

struct CameraQualitySweepPreset {
  framesize_t size;
  uint16_t width;
  uint16_t height;
  const char* name;
  const char* slug;
  bool needsWifiPowerDown;
};

static size_t sdFileSize(const char* path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  const size_t size = f.size();
  f.close();
  return size;
}

static void cameraQualitySweepYield() {
  core::watchdog::feed();
  vTaskDelay(1);
}

static bool runCameraQualitySweepOne(const CameraQualitySweepPreset& preset, bool restoreWifiAfter) {
  static constexpr int kQualities[] = {6, 8, 10, 12, 14, 18, 24};
  const uint32_t runId = millis();

  Serial.printf("[camera-qsweep] %s starting run=%lu qualities=6,8,10,12,14,18,24\n",
                preset.name, (unsigned long)runId);
  core::memory::logSnapshot("camera-qsweep-before");
  cameraQualitySweepYield();

  const bool sdOk = hal::sd::mounted() || hal::sd::begin(10000000);
  if (!sdOk || !hal::sd::ensureDir("/qtest")) {
    Serial.println("[camera-qsweep] FAIL: SD not mounted or /qtest unavailable");
    return false;
  }
  cameraQualitySweepYield();

  if (preset.needsWifiPowerDown) {
    Serial.println("[camera-qsweep] powering WiFi down for contiguous PSRAM");
    hal::wifi::powerDown();
    delay(160);
  } else {
    hal::wifi::stop();
    delay(120);
  }
  cameraQualitySweepYield();

  char manifestPath[64];
  snprintf(manifestPath, sizeof(manifestPath), "/qtest/%s_%lu.csv", preset.slug,
           (unsigned long)runId);
  File manifest = SD.open(manifestPath, FILE_WRITE);
  if (manifest) {
    manifest.println("resolution,width,height,jpeg_q,path,bytes,ok");
  }

  bool allOk = true;
  for (int q : kQualities) {
    hal::camera::deinit();
    delay(80);
    cameraQualitySweepYield();

    hal::camera::Tuning tuning{};
    tuning.autofocus = true;
    tuning.singleShotAf = false;

    const bool initOk = hal::camera::init((int)preset.size, PIXFORMAT_JPEG, &tuning, q);
    cameraQualitySweepYield();
    if (initOk) {
      const uint32_t settleMs = preset.size >= FRAMESIZE_QSXGA ? 1100u
                              : preset.size >= FRAMESIZE_HD   ? 800u
                                                               : 600u;
      hal::camera::settleAutoExposure(settleMs);
      cameraQualitySweepYield();
      hal::camera::CameraFrame warmup{};
      if (hal::camera::getFrame(&warmup)) hal::camera::returnFrame();
      cameraQualitySweepYield();
    }

    char path[64];
    snprintf(path, sizeof(path), "/qtest/%s_Q%02d_%lu.jpg", preset.slug, q,
             (unsigned long)runId);
    const bool saved = initOk && hal::camera::captureToFile(path);
    cameraQualitySweepYield();
    const size_t bytes = saved ? sdFileSize(path) : 0;
    cameraQualitySweepYield();
    const bool ok = initOk && saved;
    allOk = allOk && ok;

    Serial.printf("[camera-qsweep] %s q=%d ok=%d path=%s bytes=%u\n",
                  preset.name, q, ok ? 1 : 0, path, (unsigned)bytes);
    if (manifest) {
      manifest.printf("%s,%u,%u,%d,%s,%u,%d\n",
                      preset.name, preset.width, preset.height, q, path,
                      (unsigned)bytes, ok ? 1 : 0);
      manifest.flush();
      cameraQualitySweepYield();
    }
  }

  if (manifest) manifest.close();
  hal::camera::deinit();
  if (restoreWifiAfter) hal::wifi::scheduleRestore();
  cameraQualitySweepYield();
  core::memory::logSnapshot("camera-qsweep-after");
  Serial.printf("[camera-qsweep] %s result=%s manifest=%s\n",
                preset.name, allOk ? "PASS" : "FAIL", manifestPath);
  return allOk;
}

static bool runCameraQualitySweep(const char* which) {
  static const CameraQualitySweepPreset kPresets[] = {
      {FRAMESIZE_VGA, 640, 480, "VGA", "vga", false},
      {FRAMESIZE_HD, 1280, 720, "HD", "hd", true},
      {FRAMESIZE_QSXGA, 2560, 1920, "5MP", "5mp", true},
  };

  if (!which || !*which) {
    Serial.println("[camera-qsweep] usage: camera qsweep vga|hd|5mp|all");
    return false;
  }
  if (strcmp(which, "all") == 0) {
    bool ok = true;
    for (const CameraQualitySweepPreset& preset : kPresets) {
      ok = runCameraQualitySweepOne(preset, false) && ok;
      cameraQualitySweepYield();
    }
    hal::wifi::scheduleRestore();
    Serial.printf("[camera-qsweep] all result=%s\n", ok ? "PASS" : "FAIL");
    return ok;
  }
  for (const CameraQualitySweepPreset& preset : kPresets) {
    if (strcmp(which, preset.slug) == 0 || strcmp(which, preset.name) == 0) {
      return runCameraQualitySweepOne(preset, true);
    }
  }
  Serial.println("[camera-qsweep] unknown preset; use vga|hd|5mp|all");
  return false;
}

static bool runBatteryTest() {
  hal::power::FuelGaugeTelemetry gauge = {};
  if (!hal::power::readFuelGauge(gauge)) {
    Serial.println("[battery-test] FAIL: BQ27220 telemetry unavailable/stale");
    return false;
  }

  const uint16_t batteryStatus = gauge.battery_status;
  const uint16_t opStatus = gauge.operation_status;
  const int32_t tempDeciC = (int32_t)gauge.temperature_deci_kelvin - 2731;
  const bool present = (batteryStatus & (1u << 3)) != 0;
  const bool fullFlag = (batteryStatus & (1u << 9)) != 0;
  const bool discharging = (batteryStatus & 1u) != 0;
  const bool initialized = (opStatus & (1u << 5)) != 0;
  const hal::power::Status power = hal::power::read();
  Serial.printf("[battery-test] SOC=%u%% voltage=%u mV current=%d mA "
                "remaining=%u mAh full=%u mAh health=%u%% temp=%ld.%01ld C\n",
                (unsigned)gauge.percent, (unsigned)gauge.voltage_mv, (int)gauge.current_ma,
                (unsigned)gauge.remaining_mah, (unsigned)gauge.full_mah, (unsigned)gauge.health_percent,
                (long)(tempDeciC / 10), (long)labs(tempDeciC % 10));
  Serial.printf("[battery-test] present=%d initialized=%d gaugeFull=%d "
                "discharging=%d charger=%u PG=%d CHG=%d rawStatus=%04X op=%04X\n",
                present ? 1 : 0, initialized ? 1 : 0, fullFlag ? 1 : 0,
                discharging ? 1 : 0, (unsigned)power.charger,
                power.pg_active_low ? 1 : 0, power.chg_active_low ? 1 : 0,
                (unsigned)batteryStatus, (unsigned)opStatus);
  Serial.printf("[battery-test] design=%u mAh OCVgood=%d OCVfail=%d chargeInhibited=%d "
                "sample=%lu age_ms=%lu ui=%u%% segments=%u\n",
                (unsigned)gauge.design_mah, (batteryStatus & (1u << 5)) ? 1 : 0,
                (batteryStatus & (1u << 13)) ? 1 : 0, (batteryStatus & (1u << 8)) ? 1 : 0,
                (unsigned long)gauge.sample_count, (unsigned long)(millis() - gauge.sampled_ms),
                (unsigned)power.battery_percent, (unsigned)hal::power::batterySegments(power));
  hal::power::printFuelWorkaround();
  return present && initialized && hal::power::batterySocKnown(power);
}

static bool handleBatteryCommand(const char* line) {
  using namespace hal::power;
  if (strncmp(line, "battery ", 8) != 0) return false;
  const char* command = line + 8;
  bool ok = false;
  if (strcmp(command, "history") == 0) { printFuelHistory(); return true; }
  if (strcmp(command, "watch on") == 0 || strcmp(command, "watch off") == 0) {
    const bool on = strcmp(command, "watch on") == 0;
    setFuelWorkaroundWatch(on);
    Serial.printf("[fuel-host] watch=%s\n", on ? "on" : "off");
    return true;
  }
  if (strcmp(command, "mode host") == 0) ok = setFuelWorkaroundMode(true);
  else if (strcmp(command, "mode gauge") == 0) ok = setFuelWorkaroundMode(false);
  else if (strcmp(command, "reset") == 0) ok = resetFuelWorkaroundEstimate();
  else {
    const char* value = nullptr;
    enum { Scale, Capacity, Soc } operation = Scale;
    if (strncmp(command, "scale ", 6) == 0) { value = command + 6; operation = Scale; }
    else if (strncmp(command, "capacity ", 9) == 0) { value = command + 9; operation = Capacity; }
    else if (strncmp(command, "soc ", 4) == 0) { value = command + 4; operation = Soc; }
    if (value) {
      char* end = nullptr;
      const double number = strtod(value, &end);
      const bool parsed = end && end != value;
      while (end && isspace((unsigned char)*end)) ++end;
      if (parsed && end && *end == '\0' && isfinite(number)) {
        if (operation == Scale) ok = setFuelWorkaroundScale(number);
        else if (operation == Soc) ok = setFuelWorkaroundPercent(number);
        else if (number >= 100 && number <= 20000 && floor(number) == number)
          ok = setFuelWorkaroundCapacity((unsigned)number);
      }
    }
  }
  Serial.println(ok ? "[fuel-host] OK (host only; gauge configuration unchanged)" :
                     "[fuel-host] ERROR: invalid command/value or NVS save failed; see help/log");
  // Re-read power to refresh source selection; a cache hit never reintegrates.
  (void)hal::power::read();
  printFuelWorkaround();
  return true;
}

static bool runMicTest() {
  Serial.println("[mic-test] starting 2-second capture");
  hal::audio::txEnd();
  hal::audio::deinitMic();
  hal::audio::hardwareReset();
  const bool codecOk = hal::audio::initCodec(0x18);
  const bool micOk = codecOk && hal::audio::initMic();
  uint64_t samples = 0;
  uint64_t sumSquares = 0;
  uint64_t sumAbs = 0;
  int32_t minSample = INT32_MAX;
  int32_t maxSample = INT32_MIN;
  uint32_t readErrors = 0;
  const uint32_t deadline = millis() + 2000u;
  int32_t raw[256];
  while (micOk && (int32_t)(millis() - deadline) < 0) {
    size_t bytesRead = 0;
    const esp_err_t err = hal::audio::readMic(raw, sizeof(raw), &bytesRead,
                                               pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
      ++readErrors;
      continue;
    }
    const size_t count = bytesRead / sizeof(raw[0]);
    for (size_t i = 0; i < count; ++i) {
      const int32_t sample = raw[i] >> 16;
      if (sample < minSample) minSample = sample;
      if (sample > maxSample) maxSample = sample;
      const int64_t signedSample = sample;
      sumSquares += (uint64_t)(signedSample * signedSample);
      sumAbs += (uint64_t)(sample < 0 ? -signedSample : signedSample);
    }
    samples += count;
  }
  hal::audio::endMicSession();
  const uint32_t rms = samples ? (uint32_t)sqrt((double)sumSquares / (double)samples) : 0;
  const uint32_t meanAbs = samples ? (uint32_t)(sumAbs / samples) : 0;
  const bool signalOk = samples >= 40000u && maxSample > minSample && rms > 1u;
  Serial.printf("[mic-test] result=%s codec=%d mic=%d samples=%llu errors=%u "
                "min=%ld max=%ld rms=%u meanAbs=%u\n",
                (codecOk && micOk && signalOk) ? "PASS" : "FAIL",
                codecOk ? 1 : 0, micOk ? 1 : 0,
                (unsigned long long)samples, (unsigned)readErrors,
                (long)minSample, (long)maxSample, (unsigned)rms,
                (unsigned)meanAbs);
  return codecOk && micOk && signalOk;
}

static void printMicTuningSettings() {
  const core::audio::mic_tuning::Settings s = core::audio::mic_tuning::settings();
  Serial.printf("MIC SETTINGS gain=%u.%02ux gain_bp=%u hpf=",
                s.gainBasisPoints / 10000,
                (s.gainBasisPoints % 10000) / 100,
                (unsigned)s.gainBasisPoints);
  if (s.highPassHz == 0) Serial.print("off");
  else Serial.print((unsigned)s.highPassHz);
  Serial.print(" limiter=");
  if (s.limiterDbfs > 0) Serial.print("off");
  else Serial.print((int)s.limiterDbfs);
  Serial.println(" target=wav");
}

static void resumeMicTuneBackground() {
  if (!s_micTuneRecord || !s_micTuneRecord->backgroundPaused) return;
  s_micTuneRecord->backgroundPaused = false;
  services::background_network::resumeAfterPeripheralIo();
}

static void finishMicTuneRecording(bool success, const char* reason) {
  if (!s_micTuneRecord ||
      (!s_micTuneRecord->active && !s_micTuneRecord->backgroundPaused)) return;
  const uint32_t elapsed = millis() - s_micTuneRecord->startedMs;
  s_micTuneRecord->writer.close();
  hal::audio::endMicSession();
  s_micTuneRecord->active = false;
  resumeMicTuneBackground();

  const auto& processor = s_micTuneRecord->processor;
  const uint64_t samples = processor.samples();
  const uint32_t rms = samples
      ? (uint32_t)sqrt((double)processor.sumSquares() / (double)samples)
      : 0;
  const uint32_t meanAbs = samples
      ? (uint32_t)(processor.sumAbs() / samples)
      : 0;
  const float peakDbfs = processor.peak() > 0
      ? 20.0f * log10f((float)processor.peak() / 32768.0f)
      : -120.0f;

  if (success) {
    Serial.printf("MICREC OK path=%s duration_ms=%lu samples=%llu rms=%u mean_abs=%u "
                  "peak=%u peak_dbfs=%.2f over_range=%u limited=%u\n",
                  s_micTuneRecord->path, (unsigned long)elapsed,
                  (unsigned long long)samples, (unsigned)rms, (unsigned)meanAbs,
                  (unsigned)processor.peak(), peakDbfs,
                  (unsigned)processor.overRangeSamples(),
                  (unsigned)processor.limitedSamples());
  } else {
    Serial.printf("MICREC ERR reason=%s path=%s duration_ms=%lu samples=%llu\n",
                  reason ? reason : "unknown", s_micTuneRecord->path,
                  (unsigned long)elapsed, (unsigned long long)samples);
  }
  delete s_micTuneRecord;
  s_micTuneRecord = nullptr;
}

static bool validMicLabel(const char* label) {
  if (!label || !label[0]) return false;
  for (const char* p = label; *p; ++p) {
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return false;
  }
  return true;
}

static bool startMicTuneRecording(float seconds, const char* label) {
  if ((s_micTuneRecord && s_micTuneRecord->active) ||
      s_sdDownload.active || s_sdUpload.active ||
      hal::audio::micReady() || hal::audio::txReady()) {
    Serial.println("MICREC ERR reason=audio_or_transfer_busy");
    return true;
  }
  if (seconds < 1.0f || seconds > 30.0f || !validMicLabel(label)) {
    Serial.println("MICREC ERR reason=args range_seconds=1..30 label=A-Za-z0-9_- ");
    return true;
  }
  if (!hal::sd::mounted() || !hal::sd::ensureDir("/recordings") ||
      !hal::sd::ensureDir("/recordings/tuning")) {
    Serial.println("MICREC ERR reason=sd");
    return true;
  }
  if (!services::background_network::suspendForPeripheralIo(5000)) {
    Serial.println("MICREC ERR reason=background_busy");
    return true;
  }
  s_micTuneRecord = new (std::nothrow) MicTuneRecordState();
  if (!s_micTuneRecord) {
    services::background_network::resumeAfterPeripheralIo();
    Serial.println("MICREC ERR reason=state_alloc");
    core::memory::logSnapshot("micrec.state_alloc.fail");
    return true;
  }
  s_micTuneRecord->backgroundPaused = true;
  s_micTuneRecord->startedMs = millis();
  s_micTuneRecord->durationMs = (uint32_t)(seconds * 1000.0f + 0.5f);
  s_micTuneRecord->path[0] = '\0';
  s_micTuneRecord->processor.reset(48000);

  snprintf(s_micTuneRecord->path, sizeof(s_micTuneRecord->path),
           "/recordings/tuning/TUNE_%010lu_%s.wav",
           (unsigned long)millis(), label);
  hal::audio::hardwareReset();
  const bool codecOk = hal::audio::initCodec(0x18);
  if (codecOk) hal::audio::configureForRecording();
  if (!hal::audio::initMic()) {
    finishMicTuneRecording(false, "mic_init");
    return true;
  }
  if (!s_micTuneRecord->writer.open(s_micTuneRecord->path, 48000, 1)) {
    finishMicTuneRecording(false, "wav_open");
    return true;
  }

  s_micTuneRecord->startedMs = millis();
  s_micTuneRecord->active = true;
  const core::audio::mic_tuning::Settings s = core::audio::mic_tuning::settings();
  Serial.printf("MICREC START path=%s duration_ms=%lu gain_bp=%u hpf=%u limiter=%d\n",
                s_micTuneRecord->path, (unsigned long)s_micTuneRecord->durationMs,
                (unsigned)s.gainBasisPoints, (unsigned)s.highPassHz,
                (int)s.limiterDbfs);
  return true;
}

static void pumpMicTuneRecording(uint32_t nowMs) {
  if (!s_micTuneRecord || !s_micTuneRecord->active) return;
  for (int burst = 0; burst < 16; ++burst) {
    int32_t raw[256];
    size_t bytesRead = 0;
    const esp_err_t err = hal::audio::readMic(raw, sizeof(raw), &bytesRead, 0);
    if (err != ESP_OK) {
      finishMicTuneRecording(false, "mic_read");
      return;
    }
    if (bytesRead == 0) break;
    const size_t count = bytesRead / sizeof(raw[0]);
    int16_t pcm[256];
    s_micTuneRecord->processor.processRaw(raw, pcm, count);
    if (!s_micTuneRecord->writer.write(pcm, (int)count)) {
      finishMicTuneRecording(false, "sd_write");
      return;
    }
  }
  if ((uint32_t)(nowMs - s_micTuneRecord->startedMs) >= s_micTuneRecord->durationMs) {
    finishMicTuneRecording(true, nullptr);
  }
}

static bool handleMicTuningCommand(char* line) {
  if (strcmp(line, "mic test") == 0) return false;
  if (strcmp(line, "mic settings") == 0 || strcmp(line, "mic") == 0) {
    printMicTuningSettings();
    return true;
  }
  if (strcmp(line, "mic defaults") == 0) {
    core::audio::mic_tuning::resetDefaults();
    Serial.println("MIC SETTINGS defaults_live=1 persist_with=mic_save");
    printMicTuningSettings();
    return true;
  }
  if (strcmp(line, "mic save") == 0) {
    Serial.printf("MIC SETTINGS save=%s\n",
                  core::audio::mic_tuning::save() ? "ok" : "failed");
    printMicTuningSettings();
    return true;
  }
  if (strcmp(line, "mic gain off") == 0) {
    core::audio::mic_tuning::setGainBasisPoints(10000);
    printMicTuningSettings();
    return true;
  }
  float gain = -1.0f;
  if (sscanf(line, "mic gain %f", &gain) == 1) {
    if (gain < 0.0f || gain > 4.0f) {
      Serial.println("MIC SETTINGS ERR gain_range=0.00..4.00");
      return true;
    }
    core::audio::mic_tuning::setGainBasisPoints((int)(gain * 10000.0f + 0.5f));
    printMicTuningSettings();
    return true;
  }
  if (strcmp(line, "mic hpf off") == 0) {
    core::audio::mic_tuning::setHighPassHz(0);
    printMicTuningSettings();
    return true;
  }
  int hpf = -1;
  if (sscanf(line, "mic hpf %d", &hpf) == 1) {
    if (hpf != 0 && (hpf < 20 || hpf > 500)) {
      Serial.println("MIC SETTINGS ERR hpf_range=off_or_20..500");
      return true;
    }
    core::audio::mic_tuning::setHighPassHz(hpf);
    printMicTuningSettings();
    return true;
  }
  if (strcmp(line, "mic limiter off") == 0) {
    core::audio::mic_tuning::setLimiterDbfs(1);
    printMicTuningSettings();
    return true;
  }
  int limiter = 1;
  if (sscanf(line, "mic limiter %d", &limiter) == 1) {
    if (limiter < -20 || limiter > 0) {
      Serial.println("MIC SETTINGS ERR limiter_range=off_or_-20..0");
      return true;
    }
    core::audio::mic_tuning::setLimiterDbfs(limiter);
    printMicTuningSettings();
    return true;
  }
  if (strcmp(line, "mic status") == 0) {
    const bool active = s_micTuneRecord && s_micTuneRecord->active;
    Serial.printf("MICREC STATUS active=%u path=%s elapsed_ms=%lu target_ms=%lu\n",
                  active ? 1 : 0,
                  active && s_micTuneRecord->path[0] ? s_micTuneRecord->path : "none",
                  active
                      ? (unsigned long)(millis() - s_micTuneRecord->startedMs) : 0ul,
                  active ? (unsigned long)s_micTuneRecord->durationMs : 0ul);
    return true;
  }
  if (strcmp(line, "mic cancel") == 0) {
    if (s_micTuneRecord && s_micTuneRecord->active) {
      finishMicTuneRecording(false, "cancelled");
    }
    else Serial.println("MICREC STATUS active=0");
    return true;
  }
  float seconds = 0.0f;
  char label[25] = "clip";
  const int fields = sscanf(line, "mic record %f %24s", &seconds, label);
  if (fields >= 1) return startMicTuneRecording(seconds, label);
  if (strncmp(line, "mic ", 4) == 0) {
    Serial.println("MIC SETTINGS ERR usage");
    return true;
  }
  return false;
}

static bool runImuTest() {
  int32_t accel[3] = {}, gyro[3] = {};
  int32_t accelMin[3] = {INT32_MAX, INT32_MAX, INT32_MAX};
  int32_t accelMax[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
  int32_t gyroMin[3] = {INT32_MAX, INT32_MAX, INT32_MAX};
  int32_t gyroMax[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
  unsigned good = 0;
  for (unsigned sample = 0; sample < 20; ++sample) {
    if (hal::imu::readAxes(accel, gyro)) {
      ++good;
      for (unsigned axis = 0; axis < 3; ++axis) {
        if (accel[axis] < accelMin[axis]) accelMin[axis] = accel[axis];
        if (accel[axis] > accelMax[axis]) accelMax[axis] = accel[axis];
        if (gyro[axis] < gyroMin[axis]) gyroMin[axis] = gyro[axis];
        if (gyro[axis] > gyroMax[axis]) gyroMax[axis] = gyro[axis];
      }
    }
    delay(20);
  }
  Serial.printf("[imu-test] result=%s samples=%u accel=%ld,%ld,%ld "
                "ranges=%ld..%ld/%ld..%ld/%ld..%ld gyro=%ld,%ld,%ld "
                "ranges=%ld..%ld/%ld..%ld/%ld..%ld\n",
                good == 20 ? "PASS" : "FAIL", good,
                (long)accel[0], (long)accel[1], (long)accel[2],
                (long)accelMin[0], (long)accelMax[0],
                (long)accelMin[1], (long)accelMax[1],
                (long)accelMin[2], (long)accelMax[2],
                (long)gyro[0], (long)gyro[1], (long)gyro[2],
                (long)gyroMin[0], (long)gyroMax[0],
                (long)gyroMin[1], (long)gyroMax[1],
                (long)gyroMin[2], (long)gyroMax[2]);
  return good == 20;
}

static void runVibrationTest() {
  Serial.println("[vibration-test] motor ON for 1000 ms");
  hal::io::setVibration(true);
  delay(1000);
  hal::io::setVibration(false);
  Serial.println("[vibration-test] motor OFF");
}

static void printInputStatus() {
  bool a = false, b = false, c = false;
  const bool buttonsOk = hal::io::readButtonsSnapshot(&a, &b, &c);
  Serial.printf("[input-status] buttonsOk=%d A=%d B=%d C=%d rotaryA=%d rotaryB=%d detents=%d\n",
                buttonsOk ? 1 : 0, a ? 1 : 0, b ? 1 : 0, c ? 1 : 0,
                hal::input::readRotaryA(), hal::input::readRotaryB(),
                hal::input::takeRotaryDetents());
}

static void runMusicTest() {
  if (hal::audio::retirementPending()) {
    Serial.println("[music-dev] RESULT=FAIL reason=audio_retiring");
    return;
  }
  Serial.println("[music-dev] production MP3 playback test starting");
  if (!runSdTest()) {
    Serial.println("[music-dev] RESULT=FAIL reason=sd");
    return;
  }

  static constexpr int kPathLen = 192;
  char path[kPathLen] = {};
  if (findMp3Files(path, 1, kPathLen) < 1) {
    Serial.println("[music-dev] RESULT=FAIL reason=no_mp3_in_/music_or_root");
    return;
  }

  if (!s_devMp3) {
    void* memory=heap_caps_malloc(sizeof(services::audio::Mp3Player),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!memory) {
      Serial.println("[music-dev] RESULT=FAIL reason=diagnostic_memory");
      return;
    }
    s_devMp3=new (memory) services::audio::Mp3Player;
  }

  // The production Music app disables the normal framebuffer flush while a
  // track is playing and performs only explicitly locked direct-panel draws.
  // Match that SPI ownership model even with the panel physically removed.
  hal::display::enterOptimizedRender();
  if (!s_devMp3->stopTask()) {
    hal::display::exitOptimizedRender();
    Serial.println("[music-dev] RESULT=FAIL reason=stop_pending");
    return;
  }
  s_devMp3->stopTrack();
  // Match the production Music app ordering: release any microphone/I2S DMA
  // first, reserve the decoder task stack, then allocate TX DMA in startTrack.
  // Doing this in the opposite order can fragment internal DMA RAM enough that
  // the 4 KB decoder task cannot be created after normal cloud/widget activity.
  hal::audio::deinitMic();
  hal::audio::txEnd();
  hal::audio::hardwareReset();
  const bool codecOk = hal::audio::initCodec(0x18);
  const bool playbackOk = codecOk && hal::audio::configureForPlayback();
  if (playbackOk) hal::audio::setOutputRoute(hal::audio::OutputRoute::SpeakerOnly);
  const bool buffersOk = playbackOk && s_devMp3->init();
  const bool taskOk = buffersOk && s_devMp3->startTask();
  const bool trackOk = taskOk && s_devMp3->startTrack(path);
  Serial.printf("[music-dev] file=%s codec=%d playback=%d buffers=%d task=%d track=%d\n",
                path, codecOk ? 1 : 0, playbackOk ? 1 : 0,
                buffersOk ? 1 : 0, taskOk ? 1 : 0, trackOk ? 1 : 0);
  if (!trackOk) {
    if (!s_devMp3->stopTask()) {
      hal::display::exitOptimizedRender();
      Serial.println("[music-dev] RESULT=FAIL reason=stop_pending");
      return;
    }
    s_devMp3->stopTrack();
    hal::display::exitOptimizedRender();
    Serial.printf("[music-dev] RESULT=FAIL reason=%s\n",
                  taskOk ? "decode_or_i2s" : "decoder_task_internal_ram");
    return;
  }

  const services::audio::Mp3Player::TrackInfo& info = s_devMp3->trackInfo();
  Serial.printf("[music-dev] format=%dHz channels=%d bitrate=%lukbps estimated_ms=%lu title=\"%s\" artist=\"%s\"\n",
                info.hz, info.channels, (unsigned long)info.bitrateKbps,
                (unsigned long)info.durationMs, info.title, info.artist);
  s_devMp3->setVolume(80);
  const uint32_t startMs = millis();
  uint32_t nextLogMs = 1000;
  bool eof = false;
  while ((uint32_t)(millis() - startMs) < 10000u) {
    const uint32_t elapsed = millis() - startMs;
    if (elapsed >= nextLogMs) {
      Serial.printf("[music-dev] playing wall_ms=%lu decoded_ms=%lu\n",
                    (unsigned long)elapsed, (unsigned long)s_devMp3->elapsedMs());
      nextLogMs += 1000;
    }
    if (s_devMp3->consumeWantsNextTrack()) {
      eof = true;
      break;
    }
    delay(20);
  }
  const uint32_t decodedMs = s_devMp3->elapsedMs();
  Serial.println("[music-dev] teardown decoder task");
  if (!s_devMp3->stopTask()) {
    hal::display::exitOptimizedRender();
    Serial.println("[music-dev] RESULT=FAIL reason=stop_pending");
    return;
  }
  Serial.println("[music-dev] teardown I2S/file");
  s_devMp3->stopTrack();
  Serial.println("[music-dev] teardown display mode");
  hal::display::exitOptimizedRender();
  Serial.println("[music-dev] teardown complete");
  const bool progressed = decodedMs >= 9000u || eof;
  Serial.printf("[music-dev] RESULT=%s decoded_ms=%lu eof=%d (confirm audible music physically)\n",
                progressed ? "PASS" : "FAIL", (unsigned long)decodedMs, eof ? 1 : 0);
}

static void scheduleTap(char button, uint32_t nowMs) {
  EventType press = EventType::ButtonB_Press;
  EventType release = EventType::ButtonB_Release;
  EventType tap = EventType::ButtonB_Tap;
  if (button == 'a') {
    press = EventType::ButtonA_Press;
    release = EventType::ButtonA_Release;
    tap = EventType::ButtonA_Tap;
  } else if (button == 'c') {
    press = EventType::ButtonC_Press;
    release = EventType::ButtonC_Release;
    tap = EventType::ButtonC_Tap;
  }
  schedule(press, 0, nowMs + 5);
  schedule(release, 0, nowMs + 90);
  schedule(tap, 0, nowMs + 92);
}

static char* trimInPlace(char* s) {
  if (!s) return s;
  while (*s && isspace((unsigned char)*s)) s++;
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
  return s;
}

static void copyLine(char* dst, size_t dstLen, const char* src) {
  if (!dst || dstLen == 0) return;
  if (!src) src = "";
  strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = '\0';
}

static bool startsWithWord(const char* text, const char* word) {
  if (!text || !word) return false;
  const size_t n = strlen(word);
  if (strncasecmp(text, word, n) != 0) return false;
  return text[n] == '\0' || isspace((unsigned char)text[n]);
}

static void renderCurrentScene() {
  if (hal::display::isOptimizedRender()) {
    hal::display::beginDirectDraw();
    ui::scene::renderCurrentScene(hal::display::directTft());
    hal::display::endDirectDraw();
  } else {
    ui::scene::renderCurrentScene(hal::display::canvas());
    hal::display::markAllDirty();
  }
}

static bool handleSceneCommand(const char* rawLine) {
  char raw[kLineCap];
  copyLine(raw, sizeof(raw), rawLine);
  char* line = trimInPlace(raw);
  if (!startsWithWord(line, "scene") && !startsWithWord(line, "nodes") &&
      !startsWithWord(line, "set") && !startsWithWord(line, "unset")) {
    return false;
  }

  if (startsWithWord(line, "nodes")) {
    ui::scene::dumpCurrentScene(Serial);
    return true;
  }

  if (startsWithWord(line, "scene")) {
    line = trimInPlace(line + 5);
    if (!line[0]) {
      ui::scene::dumpCurrentScene(Serial);
      return true;
    }
  }

  if (startsWithWord(line, "clear")) {
    ui::scene::clearOverrides();
    renderCurrentScene();
    Serial.println("[scene] overrides cleared");
    return true;
  }

  const bool isUnset = startsWithWord(line, "unset");
  if (startsWithWord(line, "set") || isUnset) {
    char* cursor = line + (isUnset ? 5 : 3);
    cursor = trimInPlace(cursor);
    char* nodeId = cursor;
    while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
    if (*cursor) *cursor++ = '\0';
    cursor = trimInPlace(cursor);
    char* prop = cursor;
    while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
    if (*cursor) *cursor++ = '\0';
    cursor = trimInPlace(cursor);

    if (!nodeId[0] || !prop[0] || (!isUnset && !cursor[0])) {
      Serial.println("[scene] usage: scene set ID PROP VALUE | scene unset ID PROP");
      return true;
    }
    if (isUnset) {
      const bool ok = ui::scene::clearOverride(nodeId, prop);
      renderCurrentScene();
      Serial.printf("[scene] unset %s.%s %s\n", nodeId, prop, ok ? "ok" : "missing");
      return true;
    }

    const bool ok = ui::scene::setOverrideFromText(nodeId, prop, cursor);
    renderCurrentScene();
    Serial.printf("[scene] set %s.%s=%s %s\n", nodeId, prop, cursor, ok ? "ok" : "failed");
    return true;
  }

  Serial.println("[scene] usage: scene | scene set ID PROP VALUE | scene clear");
  return true;
}

static bool nextToken(char*& cursor, char* out, size_t outLen) {
  if (!out || outLen == 0) return false;
  out[0] = '\0';
  cursor = trimInPlace(cursor);
  if (!cursor || !cursor[0]) return false;
  char* start = cursor;
  while (*cursor && !isspace((unsigned char)*cursor)) cursor++;
  const char saved = *cursor;
  *cursor = '\0';
  copyLine(out, outLen, start);
  if (saved) cursor++;
  return out[0] != '\0';
}

static uint32_t crc32UpdateRaw(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
  }
  return crc;
}

static bool validSdPath(const char* path);

static size_t base64Encode(const uint8_t* src, size_t len, char* out, size_t outCap) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t needed = ((len + 2u) / 3u) * 4u;
  if (!src || !out || outCap <= needed) return 0;
  size_t si = 0;
  size_t oi = 0;
  while (si < len) {
    const size_t remain = len - si;
    const uint32_t a = src[si++];
    const uint32_t b = remain > 1 ? src[si++] : 0;
    const uint32_t c = remain > 2 ? src[si++] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    out[oi++] = kAlphabet[(triple >> 18) & 0x3F];
    out[oi++] = kAlphabet[(triple >> 12) & 0x3F];
    out[oi++] = remain > 1 ? kAlphabet[(triple >> 6) & 0x3F] : '=';
    out[oi++] = remain > 2 ? kAlphabet[triple & 0x3F] : '=';
  }
  out[oi] = '\0';
  return oi;
}

static void finishSdDownload(const char* error = nullptr) {
  if (s_sdDownload.file) {
    hal::bus::lockSpi();
    s_sdDownload.file.close();
    hal::bus::unlockSpi();
  }
  if (error) {
    Serial.printf("SDGET ERR reason=%s path=%s offset=%lu\n", error,
                  s_sdDownload.path, (unsigned long)s_sdDownload.sent);
  } else {
    Serial.printf("SDGET END size=%lu crc=%08lx path=%s\n",
                  (unsigned long)s_sdDownload.sent,
                  (unsigned long)~s_sdDownload.crc, s_sdDownload.path);
  }
  s_sdDownloadReservation.release();
  s_sdDownload = SdDownloadState();
}

static void pumpSdDownload() {
  if (!s_sdDownload.active) return;
  static constexpr size_t kRawChunk = 384;
  uint8_t raw[kRawChunk];
  char encoded[((kRawChunk + 2) / 3) * 4 + 1];

  // One checked packet per UI tick: slow/disconnected hosts cannot consume
  // four consecutive timeout budgets or silently truncate the data stream.
  const uint32_t remaining = s_sdDownload.size - s_sdDownload.sent;
  if (remaining == 0) {
    finishSdDownload();
    return;
  }
  const size_t wanted = remaining < sizeof(raw) ? (size_t)remaining : sizeof(raw);
  hal::bus::lockSpi();
  const int got = s_sdDownload.file.read(raw, wanted);
  hal::bus::unlockSpi();
  if (got <= 0) {
    finishSdDownload("read");
    return;
  }
  const size_t encodedLen = base64Encode(raw, (size_t)got, encoded, sizeof(encoded));
  if (encodedLen == 0) {
    finishSdDownload("encode");
    return;
  }
  const uint32_t offset = s_sdDownload.sent;
  char packet[sizeof(encoded) + 64];
  const int length = snprintf(packet, sizeof(packet), "SDGET DATA offset=%lu bytes=%d %s\n",
                              (unsigned long)offset, got, encoded);
#if defined(ARDUINO_USB_CDC_ON_BOOT)
  Serial.setTxTimeoutMs(100);
#endif
  const bool delivered = length > 0 && size_t(length) < sizeof(packet) &&
      Serial.write(reinterpret_cast<const uint8_t*>(packet), size_t(length)) == size_t(length);
#if defined(ARDUINO_USB_CDC_ON_BOOT)
  Serial.setTxTimeoutMs(5);
#endif
  if (!delivered) {
    Serial.println(); // Terminate a partial packet before the error record.
    finishSdDownload("transmit");
    return;
  }
  s_sdDownload.crc = crc32UpdateRaw(s_sdDownload.crc, raw, (size_t)got);
  s_sdDownload.sent += (uint32_t)got;
}

static bool startSdDownload(const char* path) {
  if (s_sdDownload.active || s_sdUpload.active ||
      (s_micTuneRecord && s_micTuneRecord->active)) {
    Serial.println("SDGET ERR reason=busy");
    return true;
  }
  if (!hal::sd::mounted() || !validSdPath(path)) {
    Serial.println("SDGET ERR reason=args_or_sd");
    return true;
  }
  if (s_sdDownloadReservation.acquire(true,5000) !=
      services::background_network::PeripheralReservation::Result::Acquired) {
    Serial.println("SDGET ERR reason=background_busy");
    return true;
  }

  hal::bus::lockSpi();
  File file = SD.open(path, FILE_READ);
  const uint32_t size = file ? (uint32_t)file.size() : 0;
  hal::bus::unlockSpi();
  if (!file || size == 0 || size > 16u * 1024u * 1024u) {
    if (file) {
      hal::bus::lockSpi();
      file.close();
      hal::bus::unlockSpi();
    }
    s_sdDownloadReservation.release();
    s_sdDownload = SdDownloadState();
    Serial.println("SDGET ERR reason=open_or_size");
    return true;
  }

  s_sdDownload.active = true;
  copyLine(s_sdDownload.path, sizeof(s_sdDownload.path), path);
  s_sdDownload.size = size;
  s_sdDownload.sent = 0;
  s_sdDownload.crc = 0xFFFFFFFFu;
  s_sdDownload.file = file;
  Serial.printf("SDGET BEGIN size=%lu path=%s\n", (unsigned long)size, path);
  return true;
}

static bool validSdPath(const char* path) {
  if (!path || path[0] != '/') return false;
  const size_t len = strlen(path);
  if (len < 2 || len >= kSdPathCap) return false;
  if (strstr(path, "..") || strchr(path, '\\')) return false;
  for (size_t i = 0; i < len; i++) {
    const unsigned char ch = (unsigned char)path[i];
    if (ch < 0x20 || ch == 0x7F) return false;
  }
  return true;
}

static const char* basenameOnly(const char* name) {
  if (!name) return "";
  const char* slash = strrchr(name, '/');
  return slash ? slash + 1 : name;
}

static bool ensureParentDirs(const char* filePath) {
  if (!validSdPath(filePath)) return false;
  char path[kSdPathCap];
  copyLine(path, sizeof(path), filePath);
  for (char* cursor = path + 1; *cursor; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    hal::bus::lockSpi();
    const bool ok = SD.exists(path) || SD.mkdir(path);
    hal::bus::unlockSpi();
    *cursor = '/';
    if (!ok) return false;
  }
  return true;
}

static bool removePathRecursiveLocked(const char* path) {
  File item = SD.open(path);
  if (!item) return true;
  const bool isDir = item.isDirectory();
  item.close();

  if (!isDir) return SD.remove(path);

  File dir = SD.open(path);
  if (!dir) return false;
  for (;;) {
    File child = dir.openNextFile();
    if (!child) break;
    const char* childName = basenameOnly(child.name());
    char childPath[kSdPathCap];
    const int n = snprintf(childPath, sizeof(childPath), "%s/%s", path, childName);
    child.close();
    if (n <= 0 || (size_t)n >= sizeof(childPath)) {
      dir.close();
      return false;
    }
    if (!removePathRecursiveLocked(childPath)) {
      dir.close();
      return false;
    }
  }
  dir.close();
  return SD.rmdir(path);
}

static bool clearDirContents(const char* path) {
  if (!hal::sd::mounted() || !validSdPath(path)) return false;
  hal::bus::lockSpi();
  File dir = SD.open(path);
  if (!dir) {
    const bool made = SD.mkdir(path);
    hal::bus::unlockSpi();
    return made;
  }
  const bool isDir = dir.isDirectory();
  dir.close();
  if (!isDir) {
    const bool ok = SD.remove(path) && SD.mkdir(path);
    hal::bus::unlockSpi();
    return ok;
  }

  dir = SD.open(path);
  if (!dir) {
    hal::bus::unlockSpi();
    return false;
  }
  bool ok = true;
  for (;;) {
    File child = dir.openNextFile();
    if (!child) break;
    const char* childName = basenameOnly(child.name());
    char childPath[kSdPathCap];
    const int n = snprintf(childPath, sizeof(childPath), "%s/%s", path, childName);
    child.close();
    if (n <= 0 || (size_t)n >= sizeof(childPath) || !removePathRecursiveLocked(childPath)) {
      ok = false;
      break;
    }
  }
  dir.close();
  hal::bus::unlockSpi();
  return ok;
}

static void abortSdUpload(const char* reason) {
  if (s_sdUpload.file) {
    hal::bus::lockSpi();
    s_sdUpload.file.close();
    SD.remove(s_sdUpload.path);
    hal::bus::unlockSpi();
  }
  Serial.printf("SDPUT ERR %s\n", reason ? reason : "unknown");
  s_sdUpload = SdUploadState();
}

static void finishSdUpload() {
  hal::bus::lockSpi();
  s_sdUpload.file.close();
  hal::bus::unlockSpi();
  const uint32_t finalCrc = ~s_sdUpload.crc;
  if (finalCrc != s_sdUpload.expectedCrc) {
    hal::bus::lockSpi();
    SD.remove(s_sdUpload.path);
    hal::bus::unlockSpi();
    Serial.printf("SDPUT ERR crc expected=%08lx got=%08lx path=%s\n",
                  (unsigned long)s_sdUpload.expectedCrc,
                  (unsigned long)finalCrc,
                  s_sdUpload.path);
  } else {
    Serial.printf("SDPUT OK %s %lu\n", s_sdUpload.path, (unsigned long)s_sdUpload.received);
  }
  s_sdUpload = SdUploadState();
}

static void pumpSdUpload() {
  uint16_t budget = kSdRxBudgetPerService;
  while (s_sdUpload.active && budget > 0 && Serial.available() > 0) {
    const uint32_t remaining = s_sdUpload.expectedSize - s_sdUpload.received;
    const size_t toRead = (remaining < sizeof(s_sdRxBuf)) ? (size_t)remaining : sizeof(s_sdRxBuf);
    const size_t cappedRead = (toRead < budget) ? toRead : budget;
    size_t n = 0;
    while (n < cappedRead && Serial.available() > 0) {
      const int byte = Serial.read();
      if (byte < 0) break;
      s_sdRxBuf[n++] = (uint8_t)byte;
    }
    if (n == 0) return;

    hal::bus::lockSpi();
    const size_t written = s_sdUpload.file.write(s_sdRxBuf, n);
    hal::bus::unlockSpi();
    if (written != n) {
      abortSdUpload("write");
      return;
    }

    s_sdUpload.crc = crc32UpdateRaw(s_sdUpload.crc, s_sdRxBuf, n);
    s_sdUpload.received += (uint32_t)n;
    budget = (budget > n) ? (uint16_t)(budget - n) : 0;
    if (s_sdUpload.received >= s_sdUpload.expectedSize) {
      finishSdUpload();
    } else {
      Serial.printf("SDPUT PART %lu\n", (unsigned long)s_sdUpload.received);
    }
  }
}

static bool startSdUpload(const char* path, uint32_t size, uint32_t crc) {
  if (!hal::sd::mounted()) {
    Serial.println("SDPUT ERR no_sd");
    return true;
  }
  if (!validSdPath(path) || size == 0) {
    Serial.println("SDPUT ERR args");
    return true;
  }
  if (!services::uploads::ensureInit()) {Serial.println("SDPUT ERR queue_unavailable");return true;}
  std::unique_ptr<UploadQueue::FileMutation> mutation(new(std::nothrow) UploadQueue::FileMutation(services::uploads::queue(),path,false));
  if (!mutation || !mutation->ready()) {Serial.println("SDPUT ERR queued_media_or_busy");return true;}
  if (!ensureParentDirs(path)) {
    Serial.println("SDPUT ERR mkdir");
    return true;
  }

  hal::bus::lockSpi();
  SD.remove(path);
  File file = SD.open(path, FILE_WRITE);
  hal::bus::unlockSpi();
  if (!file) {
    Serial.println("SDPUT ERR open");
    return true;
  }

  s_sdUpload = SdUploadState();
  s_sdUpload.mutation=std::move(mutation);
  s_sdUpload.active = true;
  copyLine(s_sdUpload.path, sizeof(s_sdUpload.path), path);
  s_sdUpload.expectedSize = size;
  s_sdUpload.expectedCrc = crc;
  s_sdUpload.crc = 0xFFFFFFFFu;
  s_sdUpload.file = file;
  Serial.printf("SDPUT READY %s %lu\n", s_sdUpload.path, (unsigned long)s_sdUpload.expectedSize);
  return true;
}

static bool handleSdCommand(const char* rawLine) {
  char raw[kLineCap];
  copyLine(raw, sizeof(raw), rawLine);
  char* line = trimInPlace(raw);

  if (startsWithWord(line,"sddelete")) {
    line=trimInPlace(line+8);
    if(!validSdPath(line)){Serial.println("SDDELETE ERR args");return true;}
    services::media_delete::Result result;
    const bool ok=services::media_delete::deleteFile(line,&result);
    Serial.printf("SDDELETE %s removed=%d missing=%d queue=%d path=%s\n",ok?"OK":"ERR",result.removed,result.missing,result.queueRemoved,line);
    return true;
  }
  if (startsWithWord(line, "sdclear")) {
    line = trimInPlace(line + 7);
    if (!validSdPath(line)) {
      Serial.println("SDCLEAR ERR args");
      return true;
    }
    if (!services::uploads::ensureInit()) {Serial.println("SDCLEAR ERR queue_unavailable");return true;}
    UploadQueue::FileMutation mutation(services::uploads::queue(),line,true);
    if (!mutation.ready()) {Serial.println("SDCLEAR ERR queued_media_or_busy");return true;}
    const bool ok = clearDirContents(line);
    Serial.printf("SDCLEAR %s %s\n", ok ? "OK" : "ERR", line);
    return true;
  }

  if (startsWithWord(line, "sdget")) {
    line = trimInPlace(line + 5);
    if (!validSdPath(line)) {
      Serial.println("SDGET ERR reason=usage");
      return true;
    }
    return startSdDownload(line);
  }

  if (!startsWithWord(line, "sdput")) return false;
  line = trimInPlace(line + 5);

  char sizeText[16];
  char crcText[12];
  if (!nextToken(line, sizeText, sizeof(sizeText)) || !nextToken(line, crcText, sizeof(crcText))) {
    Serial.println("SDPUT ERR usage");
    return true;
  }
  line = trimInPlace(line);
  char* end = nullptr;
  const unsigned long size = strtoul(sizeText, &end, 10);
  if (!end || *end != '\0' || size == 0 || size > 16UL * 1024UL * 1024UL) {
    Serial.println("SDPUT ERR size");
    return true;
  }
  end = nullptr;
  const unsigned long crc = strtoul(crcText, &end, 16);
  if (!end || *end != '\0') {
    Serial.println("SDPUT ERR crc_arg");
    return true;
  }
  return startSdUpload(line, (uint32_t)size, (uint32_t)crc);
}

static bool handleProvisionCommand(const char* rawLine) {
  char raw[kLineCap];
  copyLine(raw, sizeof(raw), rawLine);
  char* line = trimInPlace(raw);
  if (!startsWithWord(line, "provision")) return false;
  line = trimInPlace(line + 9);

  if (startsWithWord(line, "begin")) {
    s_provisioningRequested = true;
    s_provisioningComplete = false;
    Serial.println("PROVISION OK begin");
    return true;
  }
  if (startsWithWord(line, "done")) {
    s_provisioningComplete = true;
    Serial.println("PROVISION OK done");
    return true;
  }
  if (startsWithWord(line, "status") || !line[0]) {
    Serial.printf("PROVISION STATUS requested=%u complete=%u upload=%u\n",
                  s_provisioningRequested ? 1 : 0,
                  s_provisioningComplete ? 1 : 0,
                  s_sdUpload.active ? 1 : 0);
    return true;
  }

  Serial.println("PROVISION ERR usage");
  return true;
}

static const char* wifiModeName(wifi_mode_t mode) {
  switch (mode) {
    case WIFI_MODE_NULL:
      return "null";
    case WIFI_MODE_STA:
      return "sta";
    case WIFI_MODE_AP:
      return "ap";
    case WIFI_MODE_APSTA:
      return "apsta";
    default:
      return "unknown";
  }
}

static void printWifiUsage() {
  Serial.println("[wifi-dev] usage: wifi status | list | add SSID PASS | add JSON | connect");
}

static bool ensureStaCapableMode() {
  const wifi_mode_t mode = hal::wifi::getMode();
  if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) return true;
  const wifi_mode_t target = mode == WIFI_MODE_AP ? WIFI_MODE_APSTA : WIFI_MODE_STA;
  const bool ok = hal::wifi::setMode(target);
  hal::wifi::applyModemSleepPolicy();
  return ok;
}

static void printWifiStatus() {
  const esp_ip4_addr_t ip = hal::wifi::getSTAIP();
  Serial.printf("[wifi-dev] mode=%s connected=%u connecting=%u ssid=\"%s\" ip=" IPSTR "\n",
                wifiModeName(hal::wifi::getMode()),
                hal::wifi::connected() ? 1 : 0,
                hal::wifi::connecting() ? 1 : 0,
                hal::wifi::getSTASSID(),
                IP2STR(&ip));
#ifdef MENU_APP
  Serial.printf("[wifi-icon] tier=%d ms=%lu\n",
      NavHandler::instance().chrome().displayedWifiTier(),(unsigned long)millis());
#endif
}

static void printWifiList() {
  const int count = core::secrets::wifiNetworkCount();
  Serial.printf("[wifi-dev] saved_networks=%d\n", count);
  for (int i = 0; i < count; i++) {
    Serial.printf("[wifi-dev] %d ssid=\"%s\"\n", i, core::secrets::wifiNetworkSSID(i));
  }
}

static bool connectSavedWifi() {
  hal::wifi::reloadCredentialsFromSecrets();
  if (!ensureStaCapableMode()) return false;
  return hal::wifi::connect();
}

static bool handleWifiCommand(const char* rawLine) {
  char raw[kLineCap];
  copyLine(raw, sizeof(raw), rawLine);
  char* line = trimInPlace(raw);
  if (!startsWithWord(line, "wifi")) return false;

  line = trimInPlace(line + 4);
  char cmd[20];
  if (!nextToken(line, cmd, sizeof(cmd)) || strcasecmp(cmd, "status") == 0) {
    printWifiStatus();
    return true;
  }

  if (strcasecmp(cmd, "list") == 0) {
    printWifiList();
    return true;
  }

#ifdef MENU_APP
  if (strcasecmp(cmd, "qropen") == 0) {
    services::notifyUserActivity();
    if (NavHandler::instance().top() != &apps::wifi_setup::app)
      NavHandler::instance().push(&apps::wifi_setup::app);
    Serial.println("[wifi-dev] qropen");
    return true;
  }
  if (strcasecmp(cmd, "qrguardtest") == 0) {
    Serial.printf("[wifi-dev] qrguardtest pass=%u\n", services::qr::testStoppedSubmissionForDevelopment());
    return true;
  }
  if (strcasecmp(cmd, "qrselftest") == 0) {
    Serial.printf("[wifi-dev] qrselftest pass=%u\n",services::qr::testWorkerDecodeForDevelopment());
    return true;
  }
  if (strcasecmp(cmd, "qrfail") == 0) {
    char which[16];
    unsigned stage = 4;
    if (nextToken(line, which, sizeof(which))) {
      if (!strcmp(which, "off")) stage = 0;
      if (!strcmp(which, "jobqueue")) stage = 1;
      if (!strcmp(which, "resultqueue")) stage = 2;
      if (!strcmp(which, "task")) stage = 3;
    }
    const bool accepted = services::qr::failNextWorkerStartForDevelopment(stage);
    Serial.printf("[wifi-dev] qrfail stage=%u accepted=%u (one-shot; idle scanner only)\n", stage, accepted);
    return true;
  }
  if (strcasecmp(cmd, "qrstopdelay") == 0) {
    char value[16];
    char* end = nullptr;
    const bool supplied = nextToken(line, value, sizeof(value));
    const unsigned long ms = supplied ? strtoul(value, &end, 10) : 10001;
    const bool accepted = supplied && end != value && !*end && ms <= 10000 &&
      services::qr::delayNextWorkerStopForDevelopment(ms);
    Serial.printf("[wifi-dev] qrstopdelay accepted=%u\n", accepted);
    return true;
  }
  if (strcasecmp(cmd, "qrstatus") == 0) {
    auto& session = services::wifi_qr::Session::instance();
    Serial.printf("[wifi-dev] cleanup pending=%u ioHeld=%u agentHeld=%u\n",
                  session.closing(), session.backgroundHeld(), session.transportHeld());
    services::qr::logDiagnostics();
    return true;
  }
  if (strcasecmp(cmd, "qrpayload") == 0) {
    const bool accepted=apps::wifi_setup::debugPayload(trimInPlace(line));
    memset(raw,0,sizeof(raw));
    Serial.printf("[wifi-dev] qrpayload accepted=%u\n",accepted?1:0);return true;
  }
  if (strcasecmp(cmd, "qrtest") == 0) {
    char which[16];
    if (!nextToken(line, which, sizeof(which)) ||
        (strcmp(which, "saved") != 0 && strcmp(which, "account") != 0 && strcmp(which, "missing") != 0)) {
      Serial.println("[wifi-dev] usage: wifi qrtest saved|account|missing (scanner must be open)");
      return true;
    }
    const bool accepted = apps::wifi_setup::debugJoin(strcmp(which, "missing") != 0, strcmp(which, "account") == 0);
    Serial.printf("[wifi-dev] qrtest accepted=%u\n", accepted ? 1 : 0);
    return true;
  }
#endif

  if (strcasecmp(cmd, "connect") == 0) {
    const bool ok = connectSavedWifi();
    Serial.printf("[wifi-dev] connect started=%u\n", ok ? 1 : 0);
    printWifiStatus();
    return true;
  }

  if (strcasecmp(cmd, "add") == 0) {
    // JSON uses the same validated parser as QR/portal setup and allows spaces.
    if (*trimInPlace(line) == '{') {
      services::wifi_provision::Credentials credentials{};
      const bool ok = core::wifi_qr::parsePayload(line, &credentials) &&
                      services::wifi_provision::saveAndConnect(credentials);
      memset(&credentials, 0, sizeof(credentials));
      memset(raw, 0, sizeof(raw));
      Serial.printf("[wifi-dev] provision accepted=%u\n", ok ? 1u : 0u);
      return true;
    }
    char ssid[33];
    char password[65];
    if (!nextToken(line, ssid, sizeof(ssid)) || !nextToken(line, password, sizeof(password))) {
      printWifiUsage();
      return true;
    }

    const bool saved = core::secrets::addWifiNetwork(ssid, password);
    if (saved) {
      (void)core::secrets::promoteWifiNetworkToFront(ssid);
    }
    const bool started = saved && connectSavedWifi();
    Serial.printf("[wifi-dev] add ssid=\"%s\" saved=%u connect_started=%u\n",
                  ssid,
                  saved ? 1 : 0,
                  started ? 1 : 0);
    printWifiStatus();
    return true;
  }

  printWifiUsage();
  return true;
}

static void printBackendUsage() {
  Serial.println("[backend-dev] usage: backend status | set URL | mode dev|prod");
}

static bool handleBackendCommand(const char* rawLine) {
  char raw[kLineCap];
  copyLine(raw, sizeof(raw), rawLine);
  char* line = trimInPlace(raw);
  if (!startsWithWord(line, "backend")) return false;

  line = trimInPlace(line + 7);
  char cmd[20];
  if (!nextToken(line, cmd, sizeof(cmd)) || strcasecmp(cmd, "status") == 0) {
    Serial.printf("[backend-dev] url=%s dev=%u\n",
                  core::secrets::backendConfig().url,
                  core::secrets::isDevMode() ? 1 : 0);
    return true;
  }

  if (strcasecmp(cmd, "set") == 0) {
    char url[192];
    if (!nextToken(line, url, sizeof(url))) {
      printBackendUsage();
      return true;
    }
    const bool ok = services::agent_transport::setBackendUrl(url);
    Serial.printf("[backend-dev] set ok=%u url=%s\n",
                  ok ? 1 : 0,
                  core::secrets::backendConfig().url);
    return true;
  }

  if (strcasecmp(cmd, "mode") == 0) {
    char mode[8];
    if (!nextToken(line, mode, sizeof(mode)) || (strcasecmp(mode, "dev") && strcasecmp(mode, "prod"))) {
      printBackendUsage();
      return true;
    }
    services::agent_transport::selectBackendMode(strcasecmp(mode, "dev") == 0);
    Serial.printf("[backend-dev] mode dev=%u url=%s\n",
                  core::secrets::isDevMode() ? 1 : 0,
                  core::secrets::backendConfig().url);
    return true;
  }

  printBackendUsage();
  return true;
}

static void printApiUsage() {
  Serial.println("[api-dev] usage: api status | fingerprint | set KEY");
}

static bool handleApiCommand(const char* rawLine) {
  char raw[kLineCap];
  copyLine(raw, sizeof(raw), rawLine);
  char* line = trimInPlace(raw);
  if (!startsWithWord(line, "api")) return false;

  line = trimInPlace(line + 3);
  char cmd[20];
  if (!nextToken(line, cmd, sizeof(cmd)) || strcasecmp(cmd, "status") == 0) {
    const auto config = core::secrets::backendConfig();
    const char* key = config.apiKey;
    Serial.printf("[api-dev] set=%u len=%u\n",
                  key && key[0] ? 1 : 0,
                  key ? (unsigned)strlen(key) : 0u);
    return true;
  }

  if (strcasecmp(cmd, "fingerprint") == 0) {
    const auto config=core::secrets::backendConfig();const char* key=config.apiKey;
    uint8_t digest[32];
    if(!key||!key[0]||mbedtls_sha256_ret(reinterpret_cast<const uint8_t*>(key),strlen(key),digest,0)){
      Serial.println("[api-dev] fingerprint unavailable");return true;
    }
    Serial.print("[api-dev] sha256=");for(uint8_t byte:digest)Serial.printf("%02x",byte);Serial.println();return true;
  }
  if (strcasecmp(cmd, "set") == 0) {
    char key[96];
    if (!nextToken(line, key, sizeof(key))) {
      printApiUsage();
      return true;
    }
    const bool ok = core::secrets::setApiKey(key);
    Serial.printf("[api-dev] set ok=%u len=%u\n",
                  ok ? 1 : 0,
                  (unsigned)strlen(core::secrets::backendConfig().apiKey));
    return true;
  }

  printApiUsage();
  return true;
}

static bool handleFactoryCommand(const char* rawLine) {
  char raw[kLineCap];copyLine(raw,sizeof(raw),rawLine);char* line=trimInPlace(raw);
  if(!startsWithWord(line,"factory"))return false;
  line=trimInPlace(line+7);
  if(!strcmp(line,"status")){apps::factory_test::printStatus();return true;}
  if(!strcmp(line,"audio record")||!strcmp(line,"audio play")){
    bool ok=apps::factory_test::stationAudio(!strcmp(line,"audio record"));
    Serial.println(ok?"FACTORY OK audio":"FACTORY ERR audio-state");return true;
  }
  if(!strcmp(line,"restart")){apps::factory_test::restart();Serial.println("FACTORY OK restart");return true;}
  if(startsWithWord(line,"run")){
    char* end=nullptr;const char* value=trimInPlace(line+3);unsigned long check=strtoul(value,&end,10);
    bool ok=end!=value&&!*end&&check<services::factory_test::kCount&&apps::factory_test::stationRun(unsigned(check));
    Serial.println(ok?"FACTORY OK run":"FACTORY ERR run");return true;
  }
  if(startsWithWord(line,"result")){
    char* end=nullptr;const char* value=trimInPlace(line+6);unsigned long check=strtoul(value,&end,10);
    const char* judgment=trimInPlace(end);
    bool valid=end!=value&&check<services::factory_test::kCount;
    bool ok=valid&&(!strcmp(judgment,"pass CONFIRM")||!strcmp(judgment,"fail CONFIRM"))&&apps::factory_test::stationRecord(unsigned(check),!strcmp(judgment,"pass CONFIRM"));
    Serial.println(ok?"FACTORY OK result":"FACTORY ERR result");return true;
  }
  if(!strcmp(line,"confirm pass")||!strcmp(line,"confirm fail")){
    bool ok=apps::factory_test::stationConfirm(!strcmp(line,"confirm pass"));
    Serial.println(ok?"FACTORY OK confirm":"FACTORY ERR confirm");return true;
  }
  if(startsWithWord(line,"wifi")){
    line=trimInPlace(line+4);char ssidHex[65]={},passHex[127]={},ssid[33]={},password[64]={};
    auto decode=[](const char* input,char* output,size_t cap){
      size_t n=strlen(input);if(!n||n%2||n/2>=cap)return false;
      for(size_t i=0;i<n;i+=2){unsigned value=0;for(unsigned j=0;j<2;++j){char c=input[i+j];if(c>='0'&&c<='9')value=value*16+c-'0';else if(c>='a'&&c<='f')value=value*16+c-'a'+10;else return false;}if(!value)return false;output[i/2]=char(value);}return true;
    };
    bool ok=nextToken(line,ssidHex,sizeof(ssidHex))&&nextToken(line,passHex,sizeof(passHex))&&decode(ssidHex,ssid,sizeof(ssid))&&decode(passHex,password,sizeof(password));
    if(ok)ok=services::factory_test::configureWifi(ssid,password);
    memset(raw,0,sizeof(raw));memset(passHex,0,sizeof(passHex));memset(password,0,sizeof(password));
    Serial.println(ok?"FACTORY OK wifi":"FACTORY ERR wifi");return true;
  }
  Serial.println("FACTORY ERR usage: status | restart | wifi SSID_HEX PASSWORD_HEX");return true;
}

static bool handleManufacturingCommand(const char* rawLine) {
  char raw[kLineCap];copyLine(raw,sizeof(raw),rawLine);char* line=trimInPlace(raw);
  if(!startsWithWord(line,"station"))return false;
  line=trimInPlace(line+7);
  if(!strcmp(line,"status")){services::manufacturing::printStatus();services::device_handoff::printStatus();return true;}
  if(!strncmp(line,"manual ",7)){
    unsigned step=0;char value[12]={},confirm[12]={},extra=0;
    bool valid=sscanf(line+7,"%u %11s %11s %c",&step,value,confirm,&extra)==3&&step>=1&&step<=5&&!strcmp(confirm,"CONFIRM")&&(!strcmp(value,"complete")||!strcmp(value,"clear"));
    bool ok=valid&&services::manufacturing::setManualStep(step,!strcmp(value,"complete"));
    Serial.println(ok?"MFG OK manual":"MFG ERR manual");return true;
  }
  if(!strcmp(line,"inventory")){services::manufacturing::printInventory();return true;}
  if(!strncmp(line,"verify-image ",13)){unsigned long n=0;char expected[65]={},extra=0;bool ok=sscanf(line+13,"%lu %64s %c",&n,expected,&extra)==2&&services::manufacturing::verifyImage(n,expected);Serial.println(ok?"MFG OK image-verified":"MFG ERR image-mismatch");return true;}
  if(!strncmp(line,"image ",6)){char* end=nullptr;unsigned long n=strtoul(line+6,&end,10);if(!end||*end||!services::manufacturing::printImageHash(n))Serial.println("MFG ERR image");return true;}
  if(!strcmp(line,"unlink CONFIRM")||!strcmp(line,"unlink confirm")){
    bool ok=core::secrets::setApiKey("")&&core::secrets::setDeviceToken("");
    Serial.println(ok?"MFG OK unlink":"MFG ERR unlink");return true;
  }
  if(!strcmp(line,"scrub CONFIRM")||!strcmp(line,"scrub confirm")){
    if(!services::manufacturing::accepted()&&!services::manufacturing::manualStep(2))Serial.println("STATION SCRUB ERR tests-required");
    else if(!control::scrubDeviceForHandoff())Serial.println("STATION SCRUB ERR admission");return true;
  }
  if(!strcmp(line,"tutorial-reset CONFIRM")||!strcmp(line,"tutorial-reset confirm")){
    bool ok=services::manufacturing::resetTutorial();Serial.println(ok?"MFG OK tutorial-reset":"MFG ERR tutorial-reset");
    if(ok){Serial.flush();ESP.restart();}return true;
  }
  if(!strcmp(line,"bench-image CONFIRM")){
    Serial.println(services::manufacturing::markBench()?"MFG OK bench-image":"MFG ERR image-required");return true;
  }
  if(!strcmp(line,"arm-candidate CONFIRM")){
    if(!services::manufacturing::readyToArm(false,true))Serial.println("MFG ERR candidate-shipping-gates");
    else {Serial.println("MFG OK arm-request");if(!control::enterShipping(true))Serial.println("MFG ERR arm-admission");}return true;
  }
  if(!strcmp(line,"arm-test CONFIRM")){
    if(!services::manufacturing::readyToArm(true))Serial.println("MFG ERR test-shipping-gates");
    else {Serial.println("MFG OK arm-request");if(!control::enterShipping(true))Serial.println("MFG ERR arm-admission");}return true;
  }
  if(!strcmp(line,"arm CONFIRM")||!strcmp(line,"arm confirm")){
    if(!services::manufacturing::readyToArm())Serial.println("MFG ERR shipping-gates");
    else {Serial.println("MFG OK arm-request");if(!control::enterShipping(true))Serial.println("MFG ERR arm-admission");}return true;
  }
  Serial.println("MFG ERR unknown-command");return true;
}

static void printOtaAuthUsage() {
  Serial.println("OTA AUTH ERR usage: ota auth status | set TOKEN | clear");
}

// Bench-only workflow: use the normal reservation, signed manifest and download
// worker, then discard the candidate. Never call finishFlash or change boot slots.
static services::firmware_update::Session s_otaProbeSession;
static uint8_t s_otaProbeState = 0; // 0 idle, 1 reserve/check, 2 download
static uint32_t s_otaProbeStart = 0, s_otaProbeProgress = 0;
static const esp_partition_t* s_otaProbeBoot = nullptr;

static void closeOtaProbe(const char* result) {
  s_otaProbeSession.close();
  const auto* boot = esp_ota_get_boot_partition();
  Serial.printf("OTA PROBE %s boot_unchanged=%u elapsed_ms=%lu\n", result,
                boot && s_otaProbeBoot && boot->address == s_otaProbeBoot->address,
                (unsigned long)(millis() - s_otaProbeStart));
  s_otaProbeState = 0;
}

static void pumpOtaProbe(uint32_t nowMs) {
  if (!s_otaProbeState) return;
  if ((uint32_t)(nowMs - s_otaProbeStart) > 180000u) {
    closeOtaProbe("TIMEOUT"); return;
  }
  if (s_otaProbeState == 1) {
    if (services::firmware_update::noticeBusy()) return;
    if (s_otaProbeSession.reserveNetwork() != services::firmware_update::Session::Result::Ready) {
      closeOtaProbe("BUSY"); return;
    }
    services::ota_client::ReleaseInfo release{};
    if (!services::ota_client::fetchLatestRelease(services::ota_client::releaseChannel(), &release)) {
      Serial.printf("OTA PROBE error=%s\n", services::ota_client::lastError());
      closeOtaProbe("FAILED"); return;
    }
    Serial.printf("OTA PROBE manifest channel=%s tag=%s bytes=%d sha256=",
                  services::ota_client::releaseChannelName(release.channel), release.tag, release.size);
    for (uint8_t byte : release.sha256) Serial.printf("%02x", byte);
    Serial.println();
    if (!services::ota_client::beginFlash(release)) {
      Serial.printf("OTA PROBE error=%s\n", services::ota_client::lastError());
      closeOtaProbe("FAILED"); return;
    }
    s_otaProbeState = 2;
  }
  int written = 0, total = 0;
  const bool running = services::ota_client::pumpFlash(&written, &total);
  if (!running || (uint32_t)(nowMs - s_otaProbeProgress) >= 10000u) {
    Serial.printf("OTA PROBE progress=%d/%d\n", written, total);
    s_otaProbeProgress = nowMs;
  }
  if (!running) {
    const bool ok = total > 0 && written == total && !services::ota_client::lastError()[0];
    if (!ok) Serial.printf("OTA PROBE error=%s\n", services::ota_client::lastError());
    closeOtaProbe(ok ? "VERIFIED" : "FAILED");
  }
}

static bool handleOtaAuthCommand(const char* rawLine) {
  char raw[kLineCap];
  copyLine(raw, sizeof(raw), rawLine);
  char* line = trimInPlace(raw);
  if (!startsWithWord(line, "ota")) return false;

  line = trimInPlace(line + 3);
  if (startsWithWord(line, "channel")) {
    const char* name = trimInPlace(line + 7);
    using namespace services::ota_client;
    ReleaseChannel requested;
    if (!strcmp(name, "public")) requested = ReleaseChannel::Public;
    else if (!strcmp(name, "private")) requested = ReleaseChannel::Private;
    else if (!strcmp(name, "staging")) requested = ReleaseChannel::Staging;
    else { Serial.println("OTA CHANNEL ERR use public|private|staging"); return true; }
    if (s_otaProbeState || services::firmware_update::noticeBusy()) {
      Serial.println("OTA CHANNEL ERR update-busy"); return true;
    }
    const bool ok = setReleaseChannel(requested);
    Serial.printf("OTA CHANNEL %s selected=%s reason=%s\n", ok ? "OK" : "ERR",
                  releaseChannelName(releaseChannel()), ok ? "" : lastError());
    return true;
  }
  if (!strcmp(line, "verify-download")) {
    if (s_otaProbeState) { Serial.println("OTA PROBE BUSY"); return true; }
    s_otaProbeBoot = esp_ota_get_boot_partition();
    s_otaProbeStart = s_otaProbeProgress = millis();
    s_otaProbeState = 1;
    Serial.println("OTA PROBE START download-only");
    return true;
  }
  if (!strcmp(line, "cancel-download")) {
    if (s_otaProbeState) closeOtaProbe("CANCELLED");
    return true;
  }
  if (!strcmp(line, "policy-test")) {
    if (!core::release_policy::productionOnly || s_otaProbeState || services::firmware_update::noticeBusy()) {
      Serial.println("OTA POLICY SKIP requires idle production build"); return true;
    }
    using namespace services::ota_client;
    ReleaseInfo staging{}; staging.channel = ReleaseChannel::Staging;
    const bool selection = setReleaseChannel(ReleaseChannel::Staging);
    const bool manifest = fetchLatestRelease(ReleaseChannel::Staging, &staging);
    const bool download = beginFlash(staging);
    Serial.printf("OTA POLICY staging_select=%u staging_manifest=%u staging_download=%u channel=%s\n",
                  selection, manifest, download, releaseChannelName(releaseChannel()));
    return true;
  }
  if (startsWithWord(line, "status")) {
    Serial.printf("OTA STATUS version=%s build=%s hardware=%s layout=%s channel=%s confirmation=required auth=%u\n",
                  OTA_FIRMWARE_VERSION,
                  services::ota_client::buildKindName(),
                  OTA_HARDWARE_ID,
                  OTA_LAYOUT_ID,
                  services::ota_client::releaseChannelName(services::ota_client::releaseChannel()),
                  services::ota_client::hasAuthToken() ? 1u : 0u);
    Serial.printf("OTA POLICY production_only=%u\n", core::release_policy::productionOnly ? 1u : 0u);
    return true;
  }
  if (!startsWithWord(line, "auth")) return false;
  line = trimInPlace(line + 4);

  char cmd[20];
  if (!nextToken(line, cmd, sizeof(cmd)) || strcasecmp(cmd, "status") == 0) {
    Serial.printf("OTA AUTH STATUS set=%u\n",
                  services::ota_client::hasAuthToken() ? 1 : 0);
    return true;
  }

  if (strcasecmp(cmd, "set") == 0) {
    char token[160];
    if (!nextToken(line, token, sizeof(token))) {
      printOtaAuthUsage();
      return true;
    }
    const bool ok = services::ota_client::setAuthToken(token);
    Serial.printf("OTA AUTH %s set len=%u\n", ok ? "OK" : "ERR",
                  ok ? (unsigned)strlen(token) : 0u);
    memset(token, 0, sizeof(token));
    memset(raw, 0, sizeof(raw));
    return true;
  }

  if (strcasecmp(cmd, "clear") == 0) {
    const bool ok = services::ota_client::setAuthToken("");
    Serial.printf("OTA AUTH %s clear\n", ok ? "OK" : "ERR");
    return true;
  }

  printOtaAuthUsage();
  return true;
}

static bool handlePreviewCommand(const char* rawLine) {
  char raw[kLineCap];
  copyLine(raw, sizeof(raw), rawLine);
  char* line = trimInPlace(raw);
  if (!startsWithWord(line, "preview")) return false;

  line = trimInPlace(line + 7);
  if (!line[0] || startsWithWord(line, "apps")) {
    tooling::preview::dumpApps(Serial);
    return true;
  }

  char cmd[20];
  if (!nextToken(line, cmd, sizeof(cmd))) {
    tooling::preview::dumpApps(Serial);
    return true;
  }

  if (strcasecmp(cmd, "screens") == 0) {
    char appId[32];
    if (!nextToken(line, appId, sizeof(appId))) {
      Serial.println("[preview] usage: preview screens APP");
      return true;
    }
    tooling::preview::dumpScreens(Serial, appId);
    return true;
  }

  if (strcasecmp(cmd, "inputs") == 0) {
    char appId[32];
    char screenId[32];
    if (!nextToken(line, appId, sizeof(appId))) {
      Serial.println("[preview] usage: preview inputs APP [SCREEN]");
      return true;
    }
    if (!nextToken(line, screenId, sizeof(screenId))) screenId[0] = '\0';
    tooling::preview::dumpInputs(Serial, appId, screenId);
    return true;
  }

  const bool shouldRender = strcasecmp(cmd, "show") == 0;
  if (strcasecmp(cmd, "scene") == 0 || shouldRender) {
    char appId[32];
    char screenId[32];
    if (!nextToken(line, appId, sizeof(appId))) {
      Serial.println("[preview] usage: preview scene APP [SCREEN]");
      return true;
    }
    if (!nextToken(line, screenId, sizeof(screenId))) screenId[0] = '\0';

    ui::scene::Scene& scene = ui::scene::scratchScene();
    const bool ok = tooling::preview::buildScene(appId, screenId, scene,
                                            hal::display::canvas().width(),
                                            hal::display::canvas().height());
    if (!ok) {
      Serial.printf("[preview] error=\"unknown preview\" app=%s screen=%s\n", appId, screenId);
      return true;
    }
    ui::scene::setCurrentScene(&scene);
    Serial.printf("[preview] scene app=%s screen=%s render=%u\n",
                  appId, screenId[0] ? screenId : "(initial)", shouldRender ? 1 : 0);
    ui::scene::dumpCurrentScene(Serial);
    tooling::preview::dumpInputs(Serial, appId, screenId);
    if (shouldRender) {
      renderCurrentScene();
      NavHandler::instance().chrome().invalidate();
      NavHandler::instance().chrome().paint(hal::display::tft());
      hal::display::markAllDirty();
    }
    return true;
  }

  Serial.println("[preview] usage: preview apps | screens APP | inputs APP [SCREEN] | scene APP [SCREEN] | show APP [SCREEN]");
  return true;
}

static void printMp3VolumeCurves() {
  auto sp = services::audio::media_volume::speakerProfile();
  auto hp = services::audio::media_volume::headphoneProfile();
  const int ui = services::audio::media_volume::volume();
  const int scaled = services::audio::media_volume::scaledVolumeBasisPoints();
  Serial.printf("[mp3-vol] shared media ui=%d scaled=%d.%02d%%\n",
                ui, scaled / 100, scaled % 100);
  Serial.printf("[mp3-vol] speaker min=%u.%02u%% max=%u.%02u%% steps=%u\n",
                sp.minGainBasisPoints / 100, sp.minGainBasisPoints % 100,
                sp.maxGainBasisPoints / 100, sp.maxGainBasisPoints % 100,
                sp.steps);
  Serial.printf("[mp3-vol] headphone min=%u.%02u%% max=%u.%02u%% steps=%u (headphones only; both uses speaker profile)\n",
                hp.minGainBasisPoints / 100, hp.minGainBasisPoints % 100,
                hp.maxGainBasisPoints / 100, hp.maxGainBasisPoints % 100,
                hp.steps);
  Serial.println("[mp3-vol] ui=0 -> mute; ui>0 maps to STEPS equal-dB gains between min and max");
}

static bool handleMp3VolumeCommand(char* line) {
  if (strcmp(line, "mp3 gain") == 0 || strcmp(line, "music gain") == 0) {
    int gainBp = services::audio::media_volume::gainOverrideBasisPoints();
    if (gainBp >= 0) {
      Serial.printf("[mp3-vol] gain override=%d.%02d%%\n", gainBp / 100, gainBp % 100);
    } else {
      Serial.println("[mp3-vol] gain override=off");
    }
    return true;
  }
  if (strcmp(line, "mp3 gain off") == 0 || strcmp(line, "music gain off") == 0) {
    services::audio::media_volume::setGainOverrideBasisPoints(-1);
    Serial.println("[mp3-vol] gain override off; using route volume profile");
    return true;
  }
  float directGain = -1.0f;
  if (sscanf(line, "mp3 gain %f", &directGain) == 1 ||
      sscanf(line, "music gain %f", &directGain) == 1) {
    if (directGain < 0.0f) directGain = 0.0f;
    if (directGain > 100.0f) directGain = 100.0f;
    int gainBp = (int)(directGain * 100.0f + 0.5f);
    services::audio::media_volume::setGainOverrideBasisPoints(gainBp);
    gainBp = services::audio::media_volume::gainOverrideBasisPoints();
    Serial.printf("[mp3-vol] gain override=%d.%02d%% (use 'mp3 gain off' to restore profile)\n",
                  gainBp / 100, gainBp % 100);
    return true;
  }

  if (strcmp(line, "mp3 volume") == 0 || strcmp(line, "music volume") == 0) {
    printMp3VolumeCurves();
    return true;
  }
  if (strcmp(line, "mp3 volume save") == 0 || strcmp(line, "music volume save") == 0) {
    services::audio::media_volume::saveProfiles();
    printMp3VolumeCurves();
    return true;
  }
  if (strcmp(line, "mp3 volume defaults") == 0 || strcmp(line, "music volume defaults") == 0) {
    services::audio::media_volume::setSpeakerProfile({100, 10000, 16});
    services::audio::media_volume::setHeadphoneProfile({100, 3000, 16});
    Serial.println("[mp3-vol] restored default live profiles (use 'mp3 volume save' to persist)");
    printMp3VolumeCurves();
    return true;
  }

  char route[16] = {};
  float minPct = -1.0f;
  float maxPct = -1.0f;
  int steps = -1;
  if (sscanf(line, "mp3 volume %15s %f %f %d", route, &minPct, &maxPct, &steps) != 4 &&
      sscanf(line, "music volume %15s %f %f %d", route, &minPct, &maxPct, &steps) != 4) {
    return false;
  }

  if (minPct < 0.0f) minPct = 0.0f;
  if (maxPct < 0.0f) maxPct = 0.0f;
  if (minPct > 100.0f) minPct = 100.0f;
  if (maxPct > 100.0f) maxPct = 100.0f;
  if (steps < 1) steps = 1;
  if (steps > 100) steps = 100;

  services::audio::media_volume::VolumeProfile profile{};
  profile.minGainBasisPoints = (uint16_t)(minPct * 100.0f + 0.5f);
  profile.maxGainBasisPoints = (uint16_t)(maxPct * 100.0f + 0.5f);
  profile.steps = (uint8_t)steps;

  if (strcmp(route, "speaker") == 0 || strcmp(route, "sp") == 0) {
    services::audio::media_volume::setSpeakerProfile(profile);
    Serial.println("[mp3-vol] speaker profile updated live");
  } else if (strcmp(route, "headphone") == 0 || strcmp(route, "headphones") == 0 ||
             strcmp(route, "hp") == 0 || strcmp(route, "aux") == 0) {
    services::audio::media_volume::setHeadphoneProfile(profile);
    Serial.println("[mp3-vol] headphone profile updated live");
  } else {
    Serial.println("[mp3-vol] route must be speaker or headphone");
    return true;
  }
  printMp3VolumeCurves();
  return true;
}

static bool handleSfxVolumeCommand(char* line) {
  if (strcmp(line, "sfx volume") == 0 || strcmp(line, "sound volume") == 0) {
    const int ui = sfx::volume();
    const int gainBp = sfx::scaledVolumeBasisPoints();
    Serial.printf("[sfx-vol] ui=%d scaled=%d.%02d%%\n", ui, gainBp / 100, gainBp % 100);
    return true;
  }
  if (strcmp(line, "sfx volume save") == 0 || strcmp(line, "sound volume save") == 0) {
    sfx::saveVolume();
    const int ui = sfx::volume();
    const int gainBp = sfx::scaledVolumeBasisPoints();
    Serial.printf("[sfx-vol] ui=%d scaled=%d.%02d%%\n", ui, gainBp / 100, gainBp % 100);
    return true;
  }

  int vol = -1;
  if (sscanf(line, "sfx volume %d", &vol) != 1 &&
      sscanf(line, "sound volume %d", &vol) != 1) {
    return false;
  }
  sfx::setVolume(vol);
  vol = sfx::volume();
  const int gainBp = sfx::scaledVolumeBasisPoints();
  Serial.printf("[sfx-vol] ui=%d scaled=%d.%02d%% (use 'sfx volume save' to persist)\n",
                vol, gainBp / 100, gainBp % 100);
  return true;
}

#ifdef MENU_APP
static void printDoodleTuning() {
  const auto& s_rollTuning = services::doodle_controls::tuning();
  Serial.printf("[doodle] tune speed=%.1f zero=%.1f soft=%.1f max_roll=%.1f soft_fraction=%.3f sign=%.0f\n",
             s_rollTuning.maxSpeedPxPerSec,
             s_rollTuning.zeroDeadZoneDeg,
             s_rollTuning.softDeadZoneDeg,
             s_rollTuning.maxRollDeg,
             s_rollTuning.softZoneSpeedFraction,
             s_rollTuning.sign);
}

#endif

static bool handleDoodleCommand(char* line) {
#ifndef MENU_APP
  (void)line;
  return false;
#else
  if (strcmp(line,"trex status")==0) {apps::trex::printStatus(Serial);return true;}
  if (strcmp(line,"trex test physics")==0) {apps::trex::testPhysics(Serial);return true;}
  if (strcmp(line, "doodle status") == 0) {
    apps::doodle_jump::printStatus(Serial);
    return true;
  }
  if (strcmp(line,"flappy status")==0) {
    apps::dumb_flappy_bird::printStatus(Serial);return true;
  }
  if (strcmp(line,"flappy test noise")==0) {
    apps::dumb_flappy_bird::testNoise(Serial);return true;
  }
  if (strcmp(line, "doodle test fall") == 0) {
    apps::doodle_jump::testFall();
    Serial.println("[doodle] test fall requested");
    return true;
  }
  if (strcmp(line, "doodle test clipping") == 0) {
    apps::doodle_jump::testClipping();
    return true;
  }
  if (strcmp(line, "doodle tune") == 0 || strcmp(line, "doodle controls") == 0 ||
      strcmp(line, "doodle") == 0) {
    printDoodleTuning();
    return true;
  }
  if (strcmp(line, "doodle defaults") == 0 || strcmp(line, "doodle reset") == 0) {
    services::doodle_controls::reset();
    hal::display::markAllDirty();
    printDoodleTuning();
    return true;
  }
  if (strcmp(line, "doodle save") == 0) {
    Serial.printf("[doodle] save=%s\n",
                  services::doodle_controls::save() ? "ok" : "failed");
    printDoodleTuning();
    return true;
  }

  float speed = 0.0f;
  if (sscanf(line, "doodle speed %f", &speed) == 1) {
    if (!services::doodle_controls::setMaxSpeed(speed)) {
      Serial.println("[doodle] ERR speed_range=20..800");
      return true;
    }
    hal::display::markAllDirty();
    printDoodleTuning();
    return true;
  }

  float zero = 0.0f;
  float soft = 0.0f;
  if (sscanf(line, "doodle dz %f %f", &zero, &soft) == 2 ||
      sscanf(line, "doodle deadzone %f %f", &zero, &soft) == 2) {
    if (!services::doodle_controls::setDeadZones(zero, soft)) {
      Serial.println("[doodle] ERR deadzone_range zero=0..20 soft>=zero+0.5 soft<40");
      return true;
    }
    hal::display::markAllDirty();
    printDoodleTuning();
    return true;
  }

  float fraction = 0.0f;
  if (sscanf(line, "doodle soft %f", &fraction) == 1) {
    if (!services::doodle_controls::setSoftZoneFraction(fraction)) {
      Serial.println("[doodle] ERR soft_fraction_range=0.00..0.75");
      return true;
    }
    hal::display::markAllDirty();
    printDoodleTuning();
    return true;
  }

  float sign = 0.0f;
  if (sscanf(line, "doodle sign %f", &sign) == 1) {
    if (!services::doodle_controls::setSign(sign)) {
      Serial.println("[doodle] ERR sign=-1_or_1");
      return true;
    }
    hal::display::markAllDirty();
    printDoodleTuning();
    return true;
  }

  float tuneSpeed = 0.0f;
  float tuneZero = 0.0f;
  float tuneSoft = 0.0f;
  float tuneFraction = 0.0f;
  if (sscanf(line, "doodle tune %f %f %f %f",
             &tuneSpeed, &tuneZero, &tuneSoft, &tuneFraction) == 4) {
    if (!services::doodle_controls::setTuning(tuneSpeed, tuneZero, tuneSoft,
                                             tuneFraction)) {
      Serial.println("[doodle] ERR tune_format='doodle tune SPEED ZERO SOFT FRACTION'");
      return true;
    }
    hal::display::markAllDirty();
    printDoodleTuning();
    return true;
  }

  if (strncmp(line, "doodle ", 7) == 0) {
    Serial.println("[doodle] ERR commands=tune|speed|dz|soft|sign|save|defaults");
    return true;
  }
  return false;
#endif
}

static void handleLine(char* line, uint32_t nowMs) {
  // Admit before the generic device API: it also exposes mutations and input
  // injection, neither of which may bypass the explicit USB service session.
  if(!strcmp(line,"station access status")){
    auto& access=services::manufacturing_access::session();
    Serial.printf("MFG ACCESS active=%u pending=%u\n",access.active(),access.pending());return;
  }
  if(!strcmp(line,"station access request")){
    Serial.println(apps::factory_test::requestServiceAccess()?"MFG OK access-request":"MFG ERR access-unavailable");return;
  }
  if(!strcmp(line,"station access close")){
    services::manufacturing_access::session().close();
    s_tail=s_head;NavHandler::instance().popToRoot();
    Serial.println("MFG OK access-closed");return;
  }
  if(core::release_policy::productionOnly&&!services::manufacturing_access::session().active()){
    const bool readOnly=core::manufacturingReadOnlyCommand(line);
    if(!readOnly){Serial.println("MFG ERR service-access-required");return;}
  }
  if(tooling::handleDeviceApi(line,Serial))return;
  if(!strcmp(line,"encoder trace start") || !strcmp(line,"encoder trace stop") || !strncmp(line,"encoder trace read ",19)) {
    if(!strcmp(line,"encoder trace start"))hal::input::encoderTraceStart();
    if(!strcmp(line,"encoder trace stop"))hal::input::encoderTraceStop();
    const unsigned offset=!strncmp(line,"encoder trace read ",19)?strtoul(line+19,nullptr,10):0;
    static const uint32_t traceBoot=esp_random();
    const auto page=hal::input::encoderTracePage(offset);
    Serial.printf("[encoder] {\"ok\":true,\"version\":1,\"trace\":true,\"capacity\":%lu,\"overflowReason\":%u,\"storageReady\":%s,\"boot\":%lu,\"generation\":%lu,\"count\":%lu,\"offset\":%lu,\"active\":%s,\"overflow\":%s,\"records\":[",
      (unsigned long)page.capacity,page.overflowReason,page.storageReady?"true":"false",(unsigned long)traceBoot,(unsigned long)page.generation,(unsigned long)page.count,(unsigned long)page.offset,page.active?"true":"false",page.overflow?"true":"false");
    for(unsigned i=0;i<page.size;++i){const auto& r=page.records[i];
      Serial.printf("%s[%lu,%u,%lu,%ld,%ld,%ld,%ld,%ld]",i?",":"",(unsigned long)r.us,(unsigned)r.kind,(unsigned long)r.event,(long)r.raw,(long)r.a,(long)r.b,(long)r.c,(long)r.d);
    }
    Serial.println("]}");return;
  }
  if(!strcmp(line,"encoder status")) {
    static const uint32_t boot=esp_random();
    const auto r=hal::input::rotarySnapshot();
    Serial.printf("[encoder] {\"ok\":true,\"version\":1,\"boot\":%lu,\"uptimeMs\":%lu,\"forward\":%lu,\"reverse\":%lu,\"invalid\":%lu,\"resyncs\":%lu,\"scrollForward\":%lu,\"scrollReverse\":%lu,\"state\":%u,\"partial\":%d,\"ready\":%s,\"transitionsPerScroll\":%d,\"decoder\":\"early-three-confirm-four\",\"triggerTransitions\":3,\"restState\":2,\"anchored\":%s}\n",
      (unsigned long)boot,(unsigned long)millis(),(unsigned long)r.forward,(unsigned long)r.reverse,
      (unsigned long)r.invalid,(unsigned long)r.resyncs,(unsigned long)r.scrollForward,
      (unsigned long)r.scrollReverse,r.state,r.partial,r.ready?"true":"false",core::rotary::Decoder::kTransitionsPerScroll,r.anchored?"true":"false");
    return;
  }

  if(!strcmp(line,"net confirm-onboarding")){services::net_poll::requestOnboardingRegistration();Serial.println("[net-dev] onboarding confirmation queued");return;}
  if(!strcmp(line,"net retry-test")){services::net_poll::printRegistrationRetryCheck(Serial);return;}
  if(!strcmp(line,"net status")){services::net_poll::printStatus(Serial);return;}
#ifdef MENU_APP
  if (strcmp(line, "tutorial restart") == 0) {
    Serial.printf("[tutorial] restart=%s\n", apps::tutorial::restartForTesting() ? "ok" : "failed");
    return;
  }
  if (strncmp(line, "tutorial seek ", 14) == 0) {
    unsigned page=0,ms=0;
    const bool ok=sscanf(line+14,"%u %u",&page,&ms)==2 && apps::tutorial::debugSeek(page,ms);
    Serial.printf("[tutorial] seek=%s\n",ok?"ok":"failed");return;
  }
  if (strcmp(line,"tutorial play")==0) {apps::tutorial::debugPlay();return;}
  if (strcmp(line, "tutorial audio") == 0) {
    const auto a=sfx::typewriterStats();
    Serial.printf("[tutorial-audio] played=%lu avg=%lu max=%lu notes=%lu,%lu,%lu,%lu,%lu\n",
      (unsigned long)a.played,(unsigned long)(a.played>1?a.totalGapMs/(a.played-1):0),(unsigned long)a.maxGapMs,
      (unsigned long)a.noteCounts[0],(unsigned long)a.noteCounts[1],(unsigned long)a.noteCounts[2],(unsigned long)a.noteCounts[3],(unsigned long)a.noteCounts[4]);return;
  }
  if (strcmp(line, "tutorial status") == 0) {
    apps::tutorial::printStatus();
    return;
  }
  if (strcmp(line, "tutorial reboot") == 0) {
    Serial.println("[tutorial] rebooting; saved progress unchanged");
    Serial.flush();
    ESP.restart();
    return;
  }
#endif
  char raw[kLineCap];
  copyLine(raw, sizeof(raw), line);
  if(strncmp(raw,"recipe feed ",12)==0) {
    Serial.printf("[recipe-test] accepted=%u\n",services::automation::queueRecipeSnapshotJson(raw+12));
    return;
  }
  if(strncmp(raw,"recipe clock ",13)==0) {
    // Bench-only: accept the host's current Unix time when SNTP is unavailable.
    // Never overwrite an already synchronized clock.
    const time_t epoch=(time_t)strtoll(raw+13,nullptr,10);
    if(!services::automation::wallClockSynced() && epoch>1700000000) {
      timeval tv{epoch,0}; settimeofday(&tv,nullptr);
    }
    Serial.printf("[recipe-test] epoch=%ld\n",(long)time(nullptr));
    return;
  }
  if(strcmp(raw,"recipe on")==0 || strcmp(raw,"recipe off")==0) {
    Serial.printf("[recipe-test] saved=%u\n",services::automation::setRecipeAlertsEnabled(strcmp(raw,"recipe on")==0));
    return;
  }
  if(strcmp(raw,"recipe status")==0) {
    Serial.printf("[recipe-test] enabled=%u pending=%u local=%d ringing=%u recipe=%u epoch=%ld message=%s\n",
      services::automation::recipeAlertsEnabled(),services::automation::pendingRecipeAlertsCount(),
      services::automation::pendingAutomationsCount(),services::automation::isAlertActive(),
      services::automation::activeAlertIsRecipe(),(long)time(nullptr),services::automation::activeAlertMsg());
    return;
  }
  if (handleProvisionCommand(raw)) return;
  if (tooling::vibration_commands::handleCommand(raw,
      services::automation::wantsInputCapture() || apps::pomodoro::isAlerting())) return;
  if (handleSdCommand(raw)) return;
  if (handleSceneCommand(raw)) return;
  if (handlePreviewCommand(raw)) return;
  if (handleWifiCommand(raw)) {memset(raw,0,sizeof(raw));return;}
  if (handleFactoryCommand(raw)) return;
  if (handleManufacturingCommand(raw)) return;
  if (handleBackendCommand(raw)) return;
  if (handleApiCommand(raw)) return;
  if (handleOtaAuthCommand(raw)) return;

  lowerTrim(line);
  if (!line[0]) return;
  if (ui::home_eyes::handleDebugCommand(line)) return;
#ifdef MENU_APP
  if (apps::safe_area::handleCommand(line)) return;
#endif
  int morningSound = -1;
  if (sscanf(line, "morning sound %d", &morningSound) == 1) {
    const SfxDef* def = morningSound >= 0 && morningSound < sfx::alarms::kCount
        ? &sfx::alarms::preset((uint8_t)morningSound).ring : nullptr;
    if (!def) {
      Serial.println("[morning-studio] ERR sound must be 0..3");
    } else {
      Serial.printf("[morning-studio] OK sound=%d\n", morningSound);
      Serial.flush();
      sfx::play(*def);
    }
    return;
  }
  if (sscanf(line, "morning test %d", &morningSound) == 1) {
    const bool valid = morningSound >= 0 && morningSound <= 3;
    const bool ok = valid && services::automation::previewMorningAlarm(
        (services::automation::AlertSound)morningSound);
    Serial.printf("[morning-studio] %s loop=%d\n",
                  ok ? "OK" : "ERR", morningSound);
    return;
  }
  if (strcmp(line, "morning stop") == 0) {
    services::automation::dismissActiveAlert();
    Serial.println("[morning-studio] OK stopped");
    return;
  }
  if (strcmp(line, "morning open") == 0) {
    if (!services::automation::wantsInputCapture()) {
      services::notifyUserActivity();
      hal::display::exitOptimizedRender();
      if (NavHandler::instance().top() != &apps::alarm_clock::app) {
        NavHandler::instance().push(&apps::alarm_clock::app);
      }
    }
    return;
  }
#ifdef MENU_APP
  if (strcmp(line, "waveform test") == 0) {
    Serial.printf("[waveform-test] %s chunk-invariance, peak-hold, silence, full-scale, reset\n",
        core::audio::checkRecordingWaveform()?"PASS":"FAIL");
    return;
  }
  if (strncmp(line, "recorder ui ", 12) == 0) {
    Serial.printf("[recorder-ui] command=%s\n",
        apps::voice_recorder::developmentUi(line+12)?"ok":"unavailable");
    return;
  }
  if (strncmp(line, "agent ui ", 9) == 0) {
    if(services::automation::wantsInputCapture())return;
    services::notifyUserActivity();
    Serial.printf("[agent-ui] command=%s\n",apps::agent::developmentUi(line+9)?"ok":"unavailable");
    return;
  }
  if (strncmp(line, "morning ui ", 11) == 0) {
    Serial.printf("[morning-ui] command=%s\n",
        tooling::morningUiCommand(line+11)?"ok":"unavailable");
    return;
  }
  if (strcmp(line, "morning routine") == 0) {
    if (!services::automation::wantsInputCapture()) {
      services::notifyUserActivity();
      hal::display::exitOptimizedRender();
      NavHandler::instance().popToRoot();
      NavHandler::instance().push(&apps::morning_routine::app);
      Serial.println("[morning] routine=open");
    } else {
      Serial.println("[morning] routine=busy");
    }
    return;
  }
  if (strncmp(line, "morning quote alarm ", 20) == 0) {
    // Separate from the persisted daily Morning Alarm; never rewrite its time.
    constexpr uint32_t testId = 0xA10BFF01u;
    const char* argument = line + 20;
    if (strcmp(argument, "cancel") == 0) {
      services::automation::cancelPendingWithServerId(testId);
      Serial.println("[morning-quote-test] cancelled pending test alarm");
      return;
    }
    char* end = nullptr;
    const unsigned long seconds = strtoul(argument, &end, 10);
    if (!argument[0] || *end || seconds < 120 || seconds > 900) {
      Serial.println("[morning-quote-test] error=use 120-900 seconds (recommended 180)");
      return;
    }
    if (!services::automation::wallClockSynced() || !hal::wifi::connected() ||
        services::automation::wantsInputCapture() || apps::morning_routine::isActive() ||
        services::morning_quote::fetching()) {
      Serial.println("[morning-quote-test] error=need synced clock, WiFi, idle Home, and no quote fetch");
      return;
    }
    const time_t fire = time(nullptr) + (time_t)seconds;
    // This short bench test inspects today's cache. Avoid ambiguous midnight tests.
    if (services::morning_quote::localDayKey(fire) !=
        services::morning_quote::localDayKey(time(nullptr))) {
      Serial.println("[morning-quote-test] error=test would cross local midnight; retry after midnight");
      return;
    }
    if (!services::morning_quote::missCacheForDevelopment()) {
      Serial.println("[morning-quote-test] error=quote worker busy");
      return;
    }
    services::automation::cancelPendingWithServerId(testId);
    const uint32_t settings = core::store::getU32("morning", "settings", 7u);
    if (!services::automation::ingestScheduledWithSound(services::automation::Type::Red,
        fire, "Morning quote test", testId,
        (services::automation::AlertSound)((settings >> 16) & 3),
        services::automation::AlertKind::Morning)) {
      services::morning_quote::begin();
      Serial.println("[morning-quote-test] error=alarm queue full");
      return;
    }
    services::morning_quote::requestForEpoch(fire, true);
    struct tm local{};
    localtime_r(&fire, &local);
    Serial.printf("[morning-quote-test] armed seconds=%lu local=%04d-%02d-%02d %02d:%02d:%02d saved=%08lx\n",
        seconds, local.tm_year+1900, local.tm_mon+1, local.tm_mday,
        local.tm_hour, local.tm_min, local.tm_sec, (unsigned long)settings);
    return;
  }
  if (strcmp(line, "morning quote retry") == 0) {
    services::morning_quote::requestToday(true);
    Serial.println("[morning] quote=requested");
    return;
  }
  if (strcmp(line, "morning quote seed") == 0) {
    const bool ok = services::morning_quote::seedTodayForDevelopment();
    Serial.printf("[morning] quote seed=%s\n", ok ? "ready" : "failed");
    return;
  }
  if (strcmp(line, "morning quote status") == 0) {
    core::morning_quote_record::Record quote{};
    const bool ready = services::morning_quote::getForToday(&quote);
    Serial.printf("[morning] quote status=%s ready=%u day=%lu text=\"%s\" by=\"%s\"\n",
                  services::morning_quote::statusText(), ready,
                  (unsigned long)(ready ? quote.dayKey : 0),
                  ready ? quote.quote : "", ready ? quote.attribution : "");
    return;
  }
#endif
  if (strcmp(line, "morning test") == 0) {
    const bool ok = services::automation::previewMorningAlarm(
        services::alarms::loadMorning().sound);
    Serial.printf("[morning] test=%s\n", ok ? "started" : "busy");
    return;
  }
  if (strcmp(line, "morning status") == 0) {
    const uint32_t settings = core::store::getU32("morning", "settings", 7u);
    Serial.printf("[morning] phase=%u remaining_ms=%lu ringing=%u sound=%u saved=%08lx synced=%u pending=%u\n",
        (unsigned)services::automation::morningAlarmPhase(),
        (unsigned long)services::automation::morningAlarmSnoozeRemainingMs(),
        services::automation::isAlertActive(), (unsigned)services::automation::activeAlertSound(),
        (unsigned long)settings, services::automation::wallClockSynced(),
        services::automation::hasPendingAutomationWithServerId(0xA10B0001u));
    return;
  }
  if (strcmp(line, "morning frame") == 0) {
    // A diagnostic dump needs longer than the normal 5 ms logging budget.
    // Keep each row in one checked write without printf's heap allocation.
    TFT_eSprite& frame = hal::display::canvas();
    if(NavHandler::instance().top()==&apps::camera_menu::app)
      Serial.printf("[morning-frame] camera-composite=%u\n",apps::camera_menu::copyPreviewForDevelopment(frame));
    if(NavHandler::instance().top()==&apps::videos::app) {
      const bool copied=apps::videos::copyPreviewForDevelopment(frame);
      Serial.printf("[morning-frame] video-composite=%u\n",copied);
      if (!copied) {Serial.println("[morning-frame] aborted");return;}
    }
    static constexpr char hex[] = "0123456789abcdef";
    char row[1152]; // prefix + at most 280 RGB565 pixels + newline
    if (frame.width() <= 0 || frame.width() > 280) return;
#if defined(ARDUINO_USB_CDC_ON_BOOT)
    Serial.setTxTimeoutMs(100);
#endif
    const uint32_t began = millis();
    bool complete = true;
    Serial.printf("[morning-frame] size=%d,%d\n", frame.width(), frame.height());
    for (int y = 0; y < frame.height(); ++y) {
      if (uint32_t(millis() - began) >= 4000u) { complete = false; break; }
      const int prefix = snprintf(row, sizeof(row), "[morning-frame] row=%d ", y);
      for (int x = 0; x < frame.width(); ++x) {
        const uint16_t pixel = frame.readPixel(x, y);
        for (int digit = 0; digit < 4; ++digit) row[prefix + x * 4 + digit] = hex[(pixel >> (12 - 4 * digit)) & 15];
      }
      const size_t length = prefix + frame.width() * 4;
      row[length] = '\n';
      if (Serial.write(reinterpret_cast<const uint8_t*>(row), length + 1) != length + 1) {
        complete = false; break;
      }
      // Give the USB endpoint time to drain between ~1.1 KiB rows. Continuous
      // bursts lost bytes during live microphone capture despite full write
      // receipts. Keep the overall deadline above, including this pacing.
      delay(2);
    }
    Serial.println(complete ? "[morning-frame] done" : "[morning-frame] aborted");
#if defined(ARDUINO_USB_CDC_ON_BOOT)
    Serial.setTxTimeoutMs(5);
#endif
    return;
  }
  if (strcmp(line, "alarm test") == 0 || strncmp(line, "alarm test ", 11) == 0) {
    const int sound = !strcmp(line,"alarm test") ? 3 : atoi(line+11);
    const bool accepted = !services::automation::wantsInputCapture() &&
        sound>=0 && sound<=3 &&
        services::automation::ingestScheduledWithSound(services::automation::Type::Red,
          time(nullptr)+1,"UI test alarm",0xC0D30002u,(services::automation::AlertSound)sound);
    Serial.printf("[alarm-test] accepted=%u\n",accepted);
    return;
  }
  if (strcmp(line, "alarm audio status") == 0) { ui::printAlertAudioStatus(); return; }
  if (strcmp(line, "alarm audio reset") == 0) {
    // Exercise the same ownership/codec reset as an interrupted application's
    // playback restoration. No preferences, schedules or input are changed.
    if (services::automation::isAlertActive()) {
      hal::audio::restoreForUiPlayback();
      ::sfx::setActiveAudioGroup(nullptr);
      Serial.println("[alarm-audio-test] reset=1");
    } else Serial.println("[alarm-audio-test] reset=0");
    return;
  }
  if (strcmp(line, "nav shutdown-test") == 0) {
    // Exercise terminal software cleanup, then restore the root for USB testing.
    // Does not change rails, radio power, wake sources or saved sleep settings.
    auto& nav = NavHandler::instance();
    App* root = nav.root();
    if (!root) { Serial.println("[nav-shutdown-test] unavailable=1"); return; }
    nav.shutdown();
    Serial.printf("[nav-shutdown-test] depth=%d overlay=%u\n",
                  nav.depth(), nav.systemScreenActive());
    nav.init(root);
    Serial.printf("[nav-shutdown-test] restored=%d\n", nav.depth());
    return;
  }
  if (strcmp(line, "shipping status") == 0) {
    Serial.printf("[shipping] state=%s\n",services::shipping::name(services::shipping::state()));
    return;
  }
  if (strcmp(line, "shipping enter confirm") == 0) {
    Serial.println("[shipping] request=enter");
    if(!control::enterShipping()) Serial.println("[shipping] refused=busy-or-storage");
    return;
  }
  if (strcmp(line, "ota prompt-test") == 0) {
    services::testUpdatePrompt();
    Serial.println("[ota-prompt-test] queued=1");
    return;
  }
  if (strcmp(line, "runtime status") == 0) {
    const auto policy=NavHandler::instance().runtimePolicy();
    Serial.printf("[runtime-policy] steps=%u idle_return=%u battery_sleep=%u activity=%u depth=%d power_sleep=%u\n",
      policy.allowStepSampling,policy.allowIdleReturn,policy.allowBatterySleep,
      policy.reportActivity,NavHandler::instance().depth(),policy.allowPowerSleep);
    const auto background = core::health::backgroundPolicy();
    Serial.printf("[background-policy] uploads=%u automatic_update=%u\n",
                  background.allowUploads, background.allowAutomaticUpdate);
    return;
  }
  if (strcmp(line, "alarm status") == 0) {
    Serial.printf("[alarm-test] ringing=%u snoozed=%u remaining=%lu capture=%u\n",
      services::automation::isAlertActive(),services::automation::showingSnooze(),
      (unsigned long)services::automation::snoozeRemainingSeconds(),services::automation::wantsInputCapture());
    return;
  }
  if (strcmp(line, "morning eyes") == 0) {
    const auto phase = services::automation::morningAlarmPhase();
    const uint32_t elapsed = millis() - services::automation::morningAlarmPhaseStartedMs();
    const auto pose = ui::morning_eyes::sample(phase, elapsed);
    Serial.printf("[morning-eyes-status] phase=%u expression=%s height_pct=%u angle=%d elapsed_ms=%lu\n",
        (unsigned)phase, ui::chibi_eyes::name(pose.expression), pose.heightPercent,
        pose.rotationDegrees, (unsigned long)elapsed);
    return;
  }
  if (handleMp3VolumeCommand(line)) return;
  if (handleSfxVolumeCommand(line)) return;
  if (handleDoodleCommand(line)) return;
  if (handleMicTuningCommand(line)) return;

  if (strcmp(line, "fleet status") == 0) {
    Serial.printf("FLEET STATUS device=%s firmware=%s registered=%u polling=%u\n", core::device::id(), core::device::firmwareVersion(), core::secrets::hasDeviceToken() ? 1u : 0u, services::net_poll::running() ? 1u : 0u);
    LOGV("fleet", "Diagnostic status requested; registered=%u", core::secrets::hasDeviceToken() ? 1u : 0u);
    return;
  }
  if (strcmp(line, "fleet poll restart") == 0) {
    services::net_poll::stop(0);
    Serial.printf("FLEET POLL restart=%u\n", services::net_poll::start() ? 1u : 0u);
    return;
  }
  if (strcmp(line, "fleet sync") == 0) { services::device_fs::requestSync(); return; }
  if (strcmp(line, "?") == 0 || strcmp(line, "help") == 0) {
    printHelp();
    return;
  }
  if (strcmp(line, "queue controltest") == 0) {
    Serial.printf("[queue-test] controls pass=%u\n",UploadQueue::testControlsForDevelopment());
    return;
  }
#ifdef MENU_APP
  if (strcmp(line, "queue preview on") == 0 || strcmp(line,"queue preview off")==0) {
    UploadQueue* preview = strcmp(line,"queue preview on")==0 ? UploadQueue::controlPreviewForDevelopment() : nullptr;
    apps::settings_menu::setUploadPreviewForDevelopment(preview);
    Serial.printf("[queue-test] preview=%u isolated=1 noSD=1 noHTTP=1\n",preview!=nullptr);
    return;
  }
#endif
  if (strcmp(line, "queue status") == 0 || strcmp(line, "upload queue status") == 0) {
    if (!services::uploads::ensureInit()) {
      Serial.println("QUEUE STATUS unavailable");
      return;
    }
    services::uploads::queue().dumpDebugState();
    return;
  }
  if (strncmp(line, "queue enqueue ", 14) == 0) {
    char type[8] = {}, path[96] = {}, extra = 0;
    const bool valid = sscanf(line + 14, "%7s %95s %c", type, path, &extra) == 2 &&
        (((!strcmp(type, "image") || !strcmp(type, "audio")) && !strncmp(path, "/upload_queue/test-", 19)) ||
         (!strcmp(type,"video") && !strncmp(path,"/videos/test-",13))) && !strstr(path, "..");
    if (!valid || !services::uploads::ensureInit()) {
      Serial.println("QUEUE ENQUEUE ERR args_or_storage");
      return;
    }
    // Development adapter only: normal queue persistence, admission and worker.
    // Keep synthetic files on SD for integrity checks and retry/reboot fixtures.
    const bool ok = services::uploads::queue().enqueue(type, path,
        !strcmp(type, "image") ? "image/jpeg" : !strcmp(type,"video") ? "video/x-msvideo" : "audio/wav", false);
    Serial.printf("QUEUE ENQUEUE %s\n", ok ? "OK" : "ERR");
    return;
  }
  if (strcmp(line, "queue clear") == 0 || strcmp(line, "upload queue clear") == 0) {
    if (!services::background_network::suspendForPeripheralIo(5000)) {
      Serial.println("QUEUE CLEAR ERR busy");
      return;
    }
    services::uploads::queue().cancelAll();
    services::background_network::resumeAfterPeripheralIo();
    Serial.println("QUEUE CLEAR OK");
    return;
  }
  if (strcmp(line, "a") == 0 || strcmp(line, "at") == 0 || strcmp(line, "tap a") == 0) {
    scheduleTap('a', nowMs);
    Serial.println("[dev-input] tap A");
    return;
  }
  if (strcmp(line, "b") == 0 || strcmp(line, "bt") == 0 || strcmp(line, "tap b") == 0) {
    scheduleTap('b', nowMs);
    Serial.println("[dev-input] tap B");
    return;
  }
  if (strcmp(line, "c") == 0 || strcmp(line, "ct") == 0 || strcmp(line, "tap c") == 0) {
    scheduleTap('c', nowMs);
    Serial.println("[dev-input] tap C");
    return;
  }
  if (strcmp(line, "ap") == 0 || strcmp(line, "press a") == 0) {
    schedule(EventType::ButtonA_Press, 0, nowMs + 5);
    Serial.println("[dev-input] press A");
    return;
  }
  if (strcmp(line, "ar") == 0 || strcmp(line, "release a") == 0) {
    schedule(EventType::ButtonA_Release, 0, nowMs + 5);
    Serial.println("[dev-input] release A");
    return;
  }
  if (strcmp(line, "bp") == 0 || strcmp(line, "press b") == 0) {
    schedule(EventType::ButtonB_Press, 0, nowMs + 5);
    Serial.println("[dev-input] press B");
    return;
  }
  if (strcmp(line, "br") == 0 || strcmp(line, "release b") == 0) {
    schedule(EventType::ButtonB_Release, 0, nowMs + 5);
    Serial.println("[dev-input] release B");
    return;
  }
  if(strcmp(line,"pomodoro open")==0) {
    if(NavHandler::instance().top()!=&apps::pomodoro::app)
      NavHandler::instance().push(&apps::pomodoro::app);
    return;
  }
  if(strcmp(line,"pomodoro status")==0) { apps::pomodoro::printStatus(); return; }
  if(strncmp(line,"pomodoro test ",14)==0) {
    const uint32_t seconds=strtoul(line+14,nullptr,10);
    Serial.printf("[pomodoro-test] accepted=%u seconds=%lu\n",
      apps::pomodoro::shortenForTest(seconds),(unsigned long)seconds);
    return;
  }
  if (strcmp(line, "power boot") == 0) {
    Serial.printf("[power-boot] deep=%u timer=%u\n",
        esp_reset_reason() == ESP_RST_DEEPSLEEP,
        esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);
    return;
  }
  if (strcmp(line, "power shutdown-test") == 0) {
    services::testPowerSwitchShutdown();
    return;
  }
  if (strcmp(line,"power self-test")==0) {
    using core::power_button::Gesture;
    using core::power_button::Action;
    unsigned pass=0,fail=0;
    auto check=[&](bool ok) { if(ok) ++pass; else ++fail; };
    Gesture g;
    check(g.update(false,0)==Action::None);
    check(g.update(true,10)==Action::None);
    check(g.update(false,80)==Action::Click);
    check(g.update(false,90)==Action::None);
    check(g.update(true,100)==Action::None);
    check(g.update(true,100+core::power_button::kHoldMs-1)==Action::None);
    check(g.update(true,100+core::power_button::kHoldMs)==Action::Sleep);
    check(g.update(true,5000)==Action::None);
    check(g.update(false,5100)==Action::None);
    g.suppress();
    check(g.update(true,6000)==Action::None);
    check(g.update(true,9000)==Action::None);
    check(g.update(false,9100)==Action::None);
    check(g.update(true,0xffffff00u)==Action::None);
    check(g.update(true,uint32_t(0xffffff00u+core::power_button::kHoldMs-1))==Action::None);
    check(g.update(true,uint32_t(0xffffff00u+core::power_button::kHoldMs))==Action::Sleep);
    for(unsigned mask=0;mask<8;++mask)
      check(core::power_button::wakeRequested(mask&1,mask&2,mask&4)==bool(mask&4));
    Serial.printf("[power-test] pass=%u fail=%u hold_ms=%lu wake=C\n",pass,fail,(unsigned long)core::power_button::kHoldMs);
    return;
  }
  if(strncmp(line,"power test-wake ",16)==0) {
    unsigned long delayMs=strtoul(line+16,nullptr,10);
    if(delayMs>=3000 && delayMs<=15000) {
      services::armSleepTestWake(delayMs);
      Serial.printf("[power-test] automatic wake armed in %lu ms\n",delayMs);
    }
    return;
  }
  if (strcmp(line, "cp") == 0 || strcmp(line, "press c") == 0) {
    schedule(EventType::ButtonC_Press, 0, nowMs + 5);
    Serial.println("[dev-input] press C");
    return;
  }
  if (strcmp(line, "cr") == 0 || strcmp(line, "release c") == 0) {
    schedule(EventType::ButtonC_Release, 0, nowMs + 5);
    Serial.println("[dev-input] release C");
    return;
  }
  if (strcmp(line, "ab") == 0 || strcmp(line, "chord") == 0) {
    schedule(EventType::Chord_AB, 0, nowMs + 5);
    Serial.println("[dev-input] chord A+B");
    return;
  }
  if (strcmp(line, "up") == 0) {
    schedule(EventType::Scroll, -1, nowMs + 5);
    Serial.println("[dev-input] scroll -1");
    return;
  }
  if (strcmp(line, "down") == 0 || strcmp(line, "menu") == 0) {
    schedule(EventType::Scroll, 1, nowMs + 5);
    Serial.println("[dev-input] scroll +1");
    return;
  }
  if (strncmp(line, "rot ", 4) == 0 || strncmp(line, "scroll ", 7) == 0) {
    const char* nText = strchr(line, ' ');
    int delta = nText ? atoi(nText + 1) : 0;
    if (delta > 24) delta = 24;
    if (delta < -24) delta = -24;
    if (delta != 0) {
      schedule(EventType::Scroll, (int16_t)delta, nowMs + 5);
      Serial.printf("[dev-input] scroll %+d\n", delta);
    }
    return;
  }
  if (strcmp(line, "home") == 0) {
    NavHandler::instance().popToRoot();
    Serial.println("[dev-input] popToRoot");
    return;
  }
  if (strcmp(line, "redraw") == 0) {
    hal::display::markAllDirty();
    Serial.println("[dev-input] redraw");
    return;
  }
  if (strcmp(line, "backlight on") == 0 || strcmp(line, "bl on") == 0) {
    hal::display::setBacklight(true);
    Serial.println("[display-dev] DISP.BL=1 (Q2 backlight sink enabled)");
    return;
  }
  if (strcmp(line, "backlight off") == 0 || strcmp(line, "bl off") == 0) {
    hal::display::setBacklight(false);
    Serial.println("[display-dev] DISP.BL=0 (Q2 backlight sink disabled)");
    return;
  }
  if (strcmp(line, "display on") == 0) {
    hal::display::setEnabled(true);
    Serial.println("[display-dev] DISP.EN=1");
    return;
  }
  if (strcmp(line, "display off") == 0) {
    hal::display::setEnabled(false);
    Serial.println("[display-dev] DISP.EN=0");
    return;
  }
  if (strcmp(line, "display status") == 0) {
    Serial.printf("[display-dev] EN=%d BL=%d RST_N=%d CS=%d TE=%d edges=%lu\n",
                  hal::io::displayEnableReadback() ? 1 : 0,
                  hal::io::backlightReadback() ? 1 : 0,
                  digitalRead(LCD_RESET_PIN),
                  hal::display::chipSelectLevel() ? 1 : 0,
                  digitalRead(LCD_TEAR_PIN),
                  (unsigned long)hal::display::teEdgeCount());
    return;
  }
  if (strcmp(line, "display recover") == 0) {
    Serial.println("[display-dev] cold recovery starting");
    hal::display::coldResetAndInit();
    TFT_eSPI& panel = hal::display::directTft();
    panel.fillScreen(TFT_RED);
    delay(250);
    panel.fillScreen(TFT_GREEN);
    delay(250);
    panel.fillScreen(TFT_BLUE);
    delay(250);
    panel.fillScreen(TFT_WHITE);
    hal::display::setBacklight(true);
    delay(120);
    Serial.printf("[display-dev] recovery complete, TE_edges=%lu\n",
                  (unsigned long)hal::display::teEdgeCount());
    return;
  }
  if (strcmp(line, "display bitbang") == 0) {
    Serial.println("[display-dev] raw bit-bang test starting");
    runRawDisplayTest();
    return;
  }
  if (strcmp(line, "camera test") == 0) {
    Serial.println("[camera-dev] QVGA JPEG probe starting");
    const bool initOk = hal::camera::init(HAL_CAMERA_FRAMESIZE_QVGA,
                                          HAL_CAMERA_PIXFORMAT_JPEG);
    bool frameOk = false;
    if (initOk) {
      hal::camera::CameraFrame frame{};
      frameOk = hal::camera::getFrame(&frame);
      if (frameOk) {
        Serial.printf("[camera-dev] frame %ux%u format=%d bytes=%u\n",
                      frame.width, frame.height, frame.format,
                      (unsigned)frame.len);
        hal::camera::returnFrame();
      } else {
        Serial.println("[camera-dev] frame acquisition failed");
      }
    }
    hal::camera::deinit();
    Serial.printf("[camera-dev] result init=%d frame=%d\n", initOk ? 1 : 0,
                  frameOk ? 1 : 0);
    return;
  }
  if (strcmp(line, "camera full") == 0) {
    (void)runCameraFullTest();
    return;
  }
  if (strncmp(line, "camera qsweep ", 14) == 0) {
    (void)runCameraQualitySweep(line + 14);
    return;
  }
#ifdef MENU_APP
  if (strcmp(line, "hardware open") == 0) {
    services::notifyUserActivity();
    if (NavHandler::instance().top() != NavHandler::instance().root()) {
      Serial.println("[hardware-test] ERR return_home_first");
      return;
    }
    NavHandler::instance().push(&apps::hardware_test::app);
    apps::hardware_test::dumpDiagnosticStatus(Serial);
    return;
  }
  if (strcmp(line, "hardware status") == 0) {
    apps::hardware_test::dumpDiagnosticStatus(Serial);
    return;
  }
  if (strncmp(line, "hardware step ", 14) == 0) {
    services::notifyUserActivity();
    bool selected = false;
    if (strncmp(line + 14, "recording ", 10) == 0) {
      unsigned long index = 0; char extra = 0;
      if (sscanf(line + 24, "%lu %c", &index, &extra) == 1 && index > 0)
        selected = apps::hardware_test::selectDiagnosticStep("recording", (uint32_t)index);
    } else selected = apps::hardware_test::selectDiagnosticStep(line + 14);
    Serial.printf("[hardware-test] selected=%d\n", selected);
    apps::hardware_test::dumpDiagnosticStatus(Serial);
    return;
  }
  if (strcmp(line, "photos status") == 0) {
    services::notifyUserActivity();
    apps::photos::dumpStatus(Serial);
    return;
  }
  if (strcmp(line, "videos open") == 0) {
    services::notifyUserActivity();
    NavHandler::instance().open(&apps::videos::app);
    return;
  }
  if (strcmp(line, "videos files") == 0) {
    // Read-only local retrieval aid. Never sync or modify recorded media.
    constexpr int capacity = 192, pathBytes = 64;
    auto* paths = static_cast<char*>(heap_caps_malloc(capacity * pathBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!paths) { Serial.println("[video-files] error=memory"); return; }
    const int count = hal::sd::listFiles("/videos", "", paths, capacity, pathBytes);
    for (int i = 0; i < count; ++i) {
      size_t bytes = 0; uint32_t modified = 0;
      const char* path = paths + i * pathBytes;
      if (hal::sd::fileInfo(path, bytes, modified))
        Serial.printf("[video-file] path=%s bytes=%u modified=%lu\n", path, (unsigned)bytes, (unsigned long)modified);
    }
    heap_caps_free(paths);
    Serial.printf("[video-files] end count=%d capped=%d\n", count, count == capacity);
    return;
  }
  if (strcmp(line, "videos status") == 0) {
    apps::videos::dumpStatus(Serial);
    return;
  }
  if (strcmp(line, "camera open") == 0) {
    services::notifyUserActivity();
    if (NavHandler::instance().top() != &apps::camera_menu::app) {
      NavHandler::instance().push(&apps::camera_menu::app);
    }
    Serial.println("[camera-settle] OPEN");
    return;
  }
  if (strcmp(line, "camera audio-test") == 0) {
    services::notifyUserActivity();
    Serial.printf("[camera-audio] accepted=%d\n", apps::camera_menu::runAudioTrial());
    return;
  }
  if (strncmp(line, "camera settle ", 14) == 0) {
    services::notifyUserActivity();
    unsigned long runId = 0;
    unsigned repeat = 0;
    int frames = 0;
    char extra = 0;
    if (s_sdUpload.active || s_sdDownload.active ||
        (s_micTuneRecord && s_micTuneRecord->active)) {
      Serial.println("[camera-settle] ERR busy");
    } else if (sscanf(line + 14, "%lu %d %u %c", &runId, &frames, &repeat, &extra) != 3) {
      Serial.println("[camera-settle] ERR usage: camera settle RUN FRAMES REPEAT");
    } else {
      (void)apps::camera_menu::runSettleTrial((uint32_t)runId, frames, repeat);
    }
    return;
  }
#endif
  if (strcmp(line, "camera vga") == 0) {
    (void)runCameraResolutionTest(FRAMESIZE_VGA, 640, 480, "VGA");
    return;
  }
  if (strcmp(line, "camera hd") == 0) {
    (void)runCameraResolutionTest(FRAMESIZE_HD, 1280, 720, "HD");
    return;
  }
  if (strcmp(line, "camera 3mp") == 0) {
    (void)runCameraResolutionTest(FRAMESIZE_QXGA, 2048, 1536, "3MP");
    return;
  }
  if (strncmp(line, "battery icon ", 13) == 0 || strncmp(line, "battery demo ", 13) == 0) {
    const bool withAudio=strncmp(line,"battery demo ",13)==0;
    if(strcmp(line+13,"status")==0) { ui::battery::printPreviewStatus(); return; }
    if(strcmp(line+13,"off")==0) ui::battery::setPreview(-1,false);
    else {
      int percent=-1,external=-1,phase=-1; char extra=0;
      const int count=sscanf(line+13,"%d %d %d %c",&percent,&external,&phase,&extra);
      if((count!=2 && count!=3) || (percent!=255 && (percent<0 || percent>100)) || external<0 || external>2 || phase < -1 || phase>1) {
        Serial.println("[battery-icon] usage: battery icon PERCENT EXTERNAL [PHASE] | off | status"); return;
      }
      ui::battery::setPreview(percent,external,phase,withAudio);
    }
    NavHandler::instance().chrome().invalidate();
    services::notifyUserActivity();
    Serial.println("[battery-icon] ok (preview expires in 120s)");
    return;
  }
  if (strcmp(line, "battery test") == 0) {
    (void)runBatteryTest();
    return;
  }
  if (handleBatteryCommand(line)) return;
#if defined(KAMI_GAMES) && KAMI_GAMES
  if (strcmp(line, "tennis") == 0 || strncmp(line, "tennis ", 7) == 0) { apps::tennis::devCommand(line + 6, Serial); return; }
  if (strcmp(line, "bowl") == 0 || strncmp(line, "bowl ", 5) == 0) { apps::bowling::devCommand(line + 4, Serial); return; }
  if (strcmp(line, "golf") == 0 || strncmp(line, "golf ", 5) == 0) { apps::golf::devCommand(line + 4, Serial); return; }
#endif
  if (strcmp(line, "mic test") == 0) {
    (void)runMicTest();
    return;
  }
  if (strcmp(line, "imu test") == 0) {
    (void)runImuTest();
    return;
  }
  if (strcmp(line, "vibration test") == 0) {
    runVibrationTest();
    return;
  }
  if (strncmp(line, "input buttons ", 14) == 0) {
    unsigned mask = 0; char extra = 0;
    if (sscanf(line + 14, "%u %c", &mask, &extra) != 1 || mask > 3) {
      Serial.println("[dev-input] usage: input buttons 0..3 (A=1 B=2)");
      return;
    }
    hal::input::setTestButtons((uint8_t)mask);
    services::notifyUserActivity();
    Serial.printf("[dev-input] buttons=%u\n", mask);
    return;
  }
  if (strcmp(line, "input status") == 0) {
    printInputStatus();
    return;
  }
  if (strcmp(line, "ee inputs") == 0) {
    runEeInputTest();
    return;
  }
  if (strcmp(line, "imu int test") == 0) {
    runImuInterruptTest();
    return;
  }
  if (strcmp(line, "ee outputs") == 0) {
    runEeOutputTest();
    return;
  }
  if (strcmp(line, "io status") == 0) {
    hal::io::dumpExpanderState("dev");
    return;
  }
  if (strcmp(line, "io reinit") == 0) {
    const bool ok = hal::io::reinitAfterI2CRestart();
    Serial.printf("[io-dev] reconnect=%d\n", ok ? 1 : 0);
    hal::io::dumpExpanderState("post-reconnect");
    return;
  }
  if (strcmp(line, "tasks") == 0) {
    hal::tasks::dumpStackWatermarks();
    return;
  }
  if (strcmp(line, "audio test") == 0 || strcmp(line, "speaker test") == 0) {
    runAudioTest();
    return;
  }
  if (strcmp(line, "audio output") == 0) {
    printOutputRoute();
    return;
  }
  if (strncmp(line, "audio output ", 13) == 0) {
    const char* mode = line + 13;
    hal::audio::OutputRoute route;
    if (strcmp(mode, "auto") == 0) route = hal::audio::OutputRoute::Auto;
    else if (strcmp(mode, "speaker") == 0) route = hal::audio::OutputRoute::SpeakerOnly;
    else if (strcmp(mode, "aux") == 0) route = hal::audio::OutputRoute::HeadphonesOnly;
    else if (strcmp(mode, "both") == 0) route = hal::audio::OutputRoute::Both;
    else {
      Serial.println("[audio-output] expected auto|speaker|aux|both");
      return;
    }
    hal::audio::outputRouteSaveToNvs(route);
    printOutputRoute();
    return;
  }
  if (strcmp(line, "headphone audio") == 0 || strcmp(line, "headphone tone") == 0) {
    runAudioTest(hal::audio::OutputRoute::HeadphonesOnly);
    return;
  }
  if (strcmp(line, "headphone test") == 0 || strcmp(line, "jack test") == 0) {
    runHeadphoneTest();
    return;
  }
  if (tooling::handleTlsCommand(line)) return;
  if (strcmp(line,"widget timer-test")==0) {tooling::probeLuaTimer(Serial);return;}
  if (strcmp(line,"widget runtime-test")==0) {tooling::probeLuaRuntime(Serial);return;}
  if (strcmp(line, "display stats") == 0) {
    const auto p=NavHandler::instance().chrome().paintStats();
    const auto f=hal::display::flushStats();
    Serial.printf("[display-stats] paints=%lu full=%lu clock=%lu wifi=%lu battery=%lu flushes=%lu pixels=%lu time=%s\n",
      (unsigned long)p.paints,(unsigned long)p.full,(unsigned long)p.clock,
      (unsigned long)p.wifi,(unsigned long)p.battery,
      (unsigned long)f.frames,(unsigned long)f.pixels,
      NavHandler::instance().chrome().displayedClock());
    return;
  }
  if (strcmp(line, "widget stats") == 0) {
    const auto draw = widgets::drawStats();
    const auto flush = hal::display::flushStats();
    Serial.printf("[widget-stats] draws=%lu draw_us=%lu flushes=%lu pixels=%lu flush_us=%lu\n",
                  (unsigned long)draw.frames, (unsigned long)draw.elapsedUs,
                  (unsigned long)flush.frames, (unsigned long)flush.pixels,
                  (unsigned long)flush.elapsedUs);
    return;
  }
  if (strcmp(line, "sd status") == 0) {
    Serial.printf("[sd-status] detected=%d mounted=%d upload_queue_ready=%d\n",
                  hal::io::sdDetect(), hal::sd::mounted(),
                  services::uploads::readyQueue() != nullptr);
    return;
  }
  if (strcmp(line, "sd test") == 0) {
    (void)runSdTest();
    return;
  }
  if (strcmp(line, "music status") == 0) {
    apps::mp3_menu::dumpStatus(Serial);
    return;
  }
  if (strncmp(line, "playback delay ", 15) == 0) {
    char* end = nullptr;
    const unsigned long ms = strtoul(line + 15, &end, 10);
    const bool valid = end != line + 15 && *end == '\0' && ms <= 5000;
    const bool armed = valid && services::audio::PlaybackWorker::armFinishDelayForTest(ms);
    Serial.printf("[playback-delay] accepted=%u ms=%lu\n", armed, ms);
    return;
  }
  if (strcmp(line, "playback status") == 0) {
    Serial.printf("[playback-status] delayed=%u pending=%u retirement=%u codec_generation=%u tx_generation=%u\n",
        services::audio::PlaybackWorker::finishDelayActiveForTest(),
        services::audio::PlaybackWorker::hasPendingCleanup(), hal::audio::retirementPending(),
        (unsigned)hal::audio::codecConfigGeneration(), (unsigned)hal::audio::txGeneration());
    return;
  }
  if (strcmp(line, "music test") == 0 || strcmp(line, "mp3 test") == 0) {
    runMusicTest();
    return;
  }
  uint16_t directColor = 0;
  bool directFill = true;
  if (strcmp(line, "display red") == 0) directColor = TFT_RED;
  else if (strcmp(line, "display green") == 0) directColor = TFT_GREEN;
  else if (strcmp(line, "display blue") == 0) directColor = TFT_BLUE;
  else if (strcmp(line, "display white") == 0) directColor = TFT_WHITE;
  else if (strcmp(line, "display black") == 0) directColor = TFT_BLACK;
  else directFill = false;
  if (directFill) {
    hal::bus::lockSpi();
    TFT_eSPI& panel = hal::display::directTft();
    panel.startWrite();
    panel.fillScreen(directColor);
    panel.endWrite();
    hal::bus::unlockSpi();
    Serial.printf("[display-dev] direct fill 0x%04X\n", directColor);
    return;
  }
  if (strcmp(line, "mem") == 0 || strcmp(line, "heap") == 0) {
    // Internal DRAM (incl. DMA-capable largest block) is what hardware AES /
    // TLS needs; PSRAM figures are for the large app buffers.
    core::memory::logSnapshot("manual");
    Serial.printf("[mem] dmaLargest=%u\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return;
  }

  Serial.printf("[dev-input] unknown: %s\n", line);
  printHelp();
}

static void readSerial(uint32_t nowMs) {
  while (Serial.available() > 0) {
    if (s_sdUpload.active) {
      pumpSdUpload();
      if (s_sdUpload.active) return;
      continue;
    }
    const char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (!s_lineOverflow) {
        s_line[s_lineLen] = '\0';
        handleLine(s_line, nowMs);
      }
      memset(s_line, 0, sizeof(s_line)); // Provisioning commands must not linger in the input buffer.
      s_lineLen = 0;
      s_lineOverflow = false;
      continue;
    }
    if (s_lineOverflow) continue;
    if (s_lineLen + 1 < kLineCap) {
      s_line[s_lineLen++] = ch;
    } else {
      // Discard the entire overlong line, including any command-like suffix.
      s_lineOverflow = true;
      Serial.println("[dev-input] line too long");
    }
  }
}

static void emitDue(uint32_t nowMs) {
  while (s_tail != s_head) {
    ScheduledEvent& event = s_events[s_tail];
    if ((int32_t)(nowMs - event.dueMs) < 0) return;
    hal::input::injectEvent(event.type, event.delta);
    s_tail = (uint8_t)((s_tail + 1) % kScheduleCap);
  }
}

}  // namespace

void begin() {
  Serial.println("[dev-input] enabled. Type 'help' for commands.");
}

void service(uint32_t nowMs) {
  auto& access=services::manufacturing_access::session();
  if(access.active()&&!hal::power::externalPowerPresent(hal::power::read())){
    access.close();s_tail=s_head;
    if(s_sdUpload.active)abortSdUpload("service-closed");
    NavHandler::instance().popToRoot();
  }
  pumpOtaProbe(nowMs);
  pumpMicTuneRecording(nowMs);
  pumpSdDownload();
  readSerial(nowMs);
  // Repeat cumulative progress while waiting for the host. A lost USB status
  // line must not deadlock an otherwise intact stop-and-wait file transfer.
  static uint32_t lastUploadStatusMs = 0;
  if (s_sdUpload.active && (uint32_t)(nowMs - lastUploadStatusMs) >= 1000u) {
    lastUploadStatusMs = nowMs;
    Serial.printf("SDPUT PART %lu\n", (unsigned long)s_sdUpload.received);
  }
  emitDue(nowMs);
}

void resetProvisioningState() {
  s_provisioningRequested = false;
  s_provisioningComplete = false;
  s_lineLen = 0;
  s_lineOverflow = false;
}

bool provisioningRequested() {
  return s_provisioningRequested;
}

bool provisioningComplete() {
  return s_provisioningComplete && !s_sdUpload.active;
}

}  // namespace dev_serial_input
}  // namespace tooling

#else

namespace tooling {
namespace dev_serial_input {
void begin() {}
void service(uint32_t) {}
void resetProvisioningState() {}
bool provisioningRequested() { return false; }
bool provisioningComplete() { return false; }
}  // namespace dev_serial_input
}  // namespace tooling

#endif
