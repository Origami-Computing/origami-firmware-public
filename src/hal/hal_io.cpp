#include "hal/hal_io.h"
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include "hal/hal_bus.h"

namespace hal {
namespace io {

static constexpr uint8_t kTcaAddr = 0x22;  // rev4: TCA6424A (was MCP23017 @ 0x20)
static TCA6424A s_tca;
static bool s_initialized = false;

static volatile bool s_buttonIntPending = false;
static void IRAM_ATTR buttonIntIsr() {
  s_buttonIntPending = true;
}

// The TCA6424A shares the core I2C bus (GPIO15/16) with the fuel gauge. A brief rail sag during
// bringup inrush can make a single begin() time out (i2cWriteReadNonStop ->
// ESP_ERR_TIMEOUT 263) even when the part is healthy. Retry a few times with a
// short settle so one transient glitch does not brick boot — bringupMenuHardware()
// halts forever on a false return.
static bool tcaBeginWithRetry(uint8_t addr) {
  constexpr int kMaxAttempts = 5;
  for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
    if (s_tca.begin(Wire, addr)) {
      if (attempt > 1) {
        Serial.printf("[io] TCA6424A @0x%02X up after %d attempts\n", addr, attempt);
      }
      return true;
    }
    Serial.printf("[io] TCA6424A @0x%02X I2C attempt %d/%d failed — retrying\n",
                  addr, attempt, kMaxAttempts);
    delay(20);
  }
  return false;
}

bool init() {
  if (s_initialized) {
    return true;
  }
  if (!tcaBeginWithRetry(kTcaAddr)) {
    return false;
  }
  s_initialized = true;
  return true;
}

bool begin() {
  if (s_initialized) {
    return true;
  }
  // Legacy boards may route ~RESET to an ESP GPIO. On rev4 it is hardware-pulled
  // up by R55 and IO_EXPANDER_RESET_PIN is -1.
#if IO_EXPANDER_RESET_PIN >= 0
  pinMode(IO_EXPANDER_RESET_PIN, OUTPUT);
  digitalWrite(IO_EXPANDER_RESET_PIN, LOW);
  delay(10);
  digitalWrite(IO_EXPANDER_RESET_PIN, HIGH);
  delay(100);
#endif

  hal::bus::lockI2c();
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);

  bool ok = init();
  hal::bus::unlockI2c();
  return ok;
}

bool reinitAfterI2CRestart() {
  hal::bus::lockI2c();
  bool ok = false;
  for (int attempt = 1; attempt <= 5; ++attempt) {
    if (s_tca.reconnect(Wire, kTcaAddr)) {
      ok = true;
      break;
    }
    Serial.printf("[io] TCA6424A reconnect attempt %d/5 failed\n", attempt);
    delay(20);
  }
  if (!ok) {
    hal::bus::unlockI2c();
    return false;
  }
  hal::bus::unlockI2c();
  return true;
}

namespace {

void configureChargerStatusPinsUnlocked() {
  // rev4: BQ24040 ~PG / ~CHG are open-drain status outputs on the expander with
  // board-side external pull-ups (the TCA6424A has no internal pulls). Configure
  // them as plain inputs and read as active-low.
  s_tca.pinMode(IOX_CHARGER_PG, INPUT);
  s_tca.pinMode(IOX_CHARGER_CHG, INPUT);
  // TODO(rev4): ISET2 (input current limit) is hard-strapped by R30 on rev4 —
  // no expander pin, nothing to drive here.
}

}  // namespace

void configureDefaultPins() {
  // --- Inputs (no internal pulls on TCA6424A — external bias on the board) ---
  s_tca.pinMode(IOX_BUTTON_A, INPUT);
  s_tca.pinMode(IOX_BUTTON_B, INPUT);
  s_tca.pinMode(IOX_BUTTON_C, INPUT);
  s_tca.pinMode(IOX_SD_DETECT, INPUT);
  s_tca.pinMode(IOX_PWRSW, INPUT);
  s_tca.pinMode(IOX_IMU_INT1, INPUT);
  s_tca.pinMode(IOX_AUDIO_INT, INPUT);

  // --- Outputs — default-safe states ---
  // Camera regulators OFF; camera held in reset + powered down.
  s_tca.pinMode(IOX_REGULATOR_2V8_EN, OUTPUT);
  s_tca.pinMode(IOX_REGULATOR_1V8_EN, OUTPUT);
  s_tca.digitalWrite(IOX_REGULATOR_2V8_EN, LOW);
  s_tca.digitalWrite(IOX_REGULATOR_1V8_EN, LOW);
  s_tca.pinMode(IOX_CAMERA_RESET, OUTPUT);
  s_tca.pinMode(IOX_CAMERA_PWDN, OUTPUT);
  s_tca.digitalWrite(IOX_CAMERA_PWDN, HIGH);  // active-high: powered down
  s_tca.digitalWrite(IOX_CAMERA_RESET, LOW);  // active-low: in reset

  // Vibration motor off.
  s_tca.pinMode(IOX_VIBRATION_MOTOR, OUTPUT);
  s_tca.digitalWrite(IOX_VIBRATION_MOTOR, LOW);

  // Stemma QT rail ON by default so any integrated I2C peripherals show up in scans.
  s_tca.pinMode(IOX_STEMMA_POWER_EN, OUTPUT);
  s_tca.digitalWrite(IOX_STEMMA_POWER_EN, HIGH);

  // Audio codec ~RESET deasserted (idle HIGH).
  s_tca.pinMode(IOX_AUDIO_RESET, OUTPUT);
  s_tca.digitalWrite(IOX_AUDIO_RESET, HIGH);

  // Display enable + backlight controlled explicitly by hal_display; start OFF.
  s_tca.pinMode(IOX_DISPLAY_EN, OUTPUT);
  s_tca.digitalWrite(IOX_DISPLAY_EN, LOW);
  s_tca.pinMode(IOX_DISPLAY_BL, OUTPUT);
  s_tca.digitalWrite(IOX_DISPLAY_BL, LOW);

  // VSYS load switch OFF initially — board_bringup enables it explicitly.
  s_tca.pinMode(IOX_EN_VSYS, OUTPUT);
  s_tca.digitalWrite(IOX_EN_VSYS, LOW);

  configureChargerStatusPinsUnlocked();

  // rev4: buttons are polled (TCA6424A has no interrupt-on-change capture). We
  // still arm the shared ~INT → GPIO1 edge ISR as a cheap "something changed"
  // hint, but chord/edge logic does NOT depend on it.
  pinMode(IO_EXPANDER_INT_B_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(IO_EXPANDER_INT_B_PIN), buttonIntIsr, FALLING);
}

bool buttonInterruptPending() {
  if (!s_buttonIntPending) return false;
  noInterrupts();
  bool p = s_buttonIntPending;
  s_buttonIntPending = false;
  interrupts();
  return p;
}

void setPmicHighPower() {
  // rev4: no PMIC strap pin on the TCA6424A. No-op.
  // TODO(rev4): confirm no high-power strap is needed (BQ24040 self-enables above UVLO).
}

void configureChargerStatusPins() {
  hal::bus::lockI2c();
  configureChargerStatusPinsUnlocked();
  hal::bus::unlockI2c();
}

bool chargerStatsActiveLow() {
  return chargerPgActiveLow();
}

void setInputCurrentMode(InputCurrentMode mode) {
  // rev4: ISET2 is hard-strapped by R30 on the board — no software control. No-op.
  // TODO(rev4): input current limit is fixed in hardware; verify R30 value gives the intended limit.
  (void)mode;
}

bool chargerPgActiveLow() {
  if (!s_initialized) return false;
  hal::bus::lockI2c();
  bool low = s_tca.digitalRead(IOX_CHARGER_PG) == LOW;
  hal::bus::unlockI2c();
  return low;
}

bool chargerChgActiveLow() {
  if (!s_initialized) return false;
  hal::bus::lockI2c();
  bool low = s_tca.digitalRead(IOX_CHARGER_CHG) == LOW;
  hal::bus::unlockI2c();
  return low;
}

uint8_t readMcpPortB() {
  hal::bus::lockI2c();
  uint8_t v = s_tca.readPort(2);  // rev4: charger/SD-detect/display live on port 2
  hal::bus::unlockI2c();
  return v;
}

uint8_t readMcpPortA() {
  hal::bus::lockI2c();
  uint8_t v = s_tca.readPort(1);  // rev4: buttons / audio reset / backlight live on port 1
  hal::bus::unlockI2c();
  return v;
}

void dumpChargerPinState(const char* tag) {
  if (!tag) tag = "";
  hal::bus::lockI2c();
  const uint8_t p0 = s_tca.readPort(0);
  const uint8_t p1 = s_tca.readPort(1);
  const uint8_t p2 = s_tca.readPort(2);
  hal::bus::unlockI2c();
  Serial.printf("[tca %s] IN port0/1/2 = 0x%02X/0x%02X/0x%02X (PG=%d CHG=%d)\n",
                tag, p0, p1, p2,
                (int)((p2 >> (IOX_CHARGER_PG & 7)) & 1u),
                (int)((p2 >> (IOX_CHARGER_CHG & 7)) & 1u));
}

void dumpExpanderState(const char* tag) {
  if (!tag) tag = "";
  hal::bus::lockI2c();
  auto readReg = [](uint8_t reg) -> int {
    Wire.beginTransmission(kTcaAddr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((int)kTcaAddr, 1, (int)true) != 1) return -1;
    return Wire.read();
  };
  int in[3], out[3], cfg[3];
  for (uint8_t p = 0; p < 3; ++p) {
    in[p] = readReg((uint8_t)(0x00 + p));
    out[p] = readReg((uint8_t)(0x04 + p));
    cfg[p] = readReg((uint8_t)(0x0C + p));
  }
  hal::bus::unlockI2c();
  Serial.printf("[tca %s] IN=%02X/%02X/%02X OUT=%02X/%02X/%02X CFG=%02X/%02X/%02X\n",
                tag, in[0], in[1], in[2], out[0], out[1], out[2],
                cfg[0], cfg[1], cfg[2]);
}

bool readExpanderInputs(uint8_t* port0, uint8_t* port1, uint8_t* port2) {
  if (!s_initialized || !port0 || !port1 || !port2) return false;
  if (!hal::bus::lockI2c(pdMS_TO_TICKS(100))) return false;
  *port0 = s_tca.readPort(0);
  *port1 = s_tca.readPort(1);
  *port2 = s_tca.readPort(2);
  hal::bus::unlockI2c();
  return true;
}

bool readExpanderPort(uint8_t port, uint8_t* value, uint8_t* txError,
                      uint8_t* rxCount) {
  if (!s_initialized || !value || !txError || !rxCount) return false;
  if (!hal::bus::lockI2c(pdMS_TO_TICKS(100))) {
    *value = 0xFF;
    *txError = 0xFE;  // Diagnostic sentinel: I2C mutex acquisition timed out.
    *rxCount = 0;
    return false;
  }
  const bool ok = s_tca.readPort(port, value, txError, rxCount);
  hal::bus::unlockI2c();
  return ok;
}

void pauseButtonInterrupt() {
  detachInterrupt(digitalPinToInterrupt(IO_EXPANDER_INT_B_PIN));
  pinMode(IO_EXPANDER_INT_B_PIN, INPUT);
}

void resumeButtonInterrupt() {
  pinMode(IO_EXPANDER_INT_B_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(IO_EXPANDER_INT_B_PIN), buttonIntIsr, FALLING);
}

void enableRegulators(bool enable2v8, bool enable1v8) {
  s_tca.pinMode(IOX_REGULATOR_2V8_EN, OUTPUT);
  s_tca.pinMode(IOX_REGULATOR_1V8_EN, OUTPUT);
  s_tca.digitalWrite(IOX_REGULATOR_2V8_EN, enable2v8 ? HIGH : LOW);
  s_tca.digitalWrite(IOX_REGULATOR_1V8_EN, enable1v8 ? HIGH : LOW);
}

void setStemmaPower(bool on) {
  hal::bus::lockI2c();
  s_tca.pinMode(IOX_STEMMA_POWER_EN, OUTPUT);
  s_tca.digitalWrite(IOX_STEMMA_POWER_EN, on ? HIGH : LOW);
  hal::bus::unlockI2c();
}

void setEnVsys(bool on) {
  hal::bus::lockI2c();
  s_tca.pinMode(IOX_EN_VSYS, OUTPUT);
  s_tca.digitalWrite(IOX_EN_VSYS, on ? HIGH : LOW);
  hal::bus::unlockI2c();
}

void setSdEnable(bool on) {
  // rev4: no SD_EN pin — SD is powered from VSYS. No-op.
  // TODO(rev4): SD power gated by IOX_EN_VSYS (whole system rail), not a dedicated SD switch.
  (void)on;
}

bool sdDetect() {
  return s_tca.digitalRead(IOX_SD_DETECT) == LOW;
}

bool sdEnableReadback() {
  // rev4: no SD_EN pin — report enabled (SD tracks VSYS).
  return true;
}

bool writeVibration(bool on) {
  if (!s_initialized || !hal::bus::lockI2c(pdMS_TO_TICKS(10))) return false;
  const bool ok = s_tca.digitalWrite(IOX_VIBRATION_MOTOR, on ? HIGH : LOW);
  hal::bus::unlockI2c();
  return ok;
}
void setVibration(bool on) { (void)writeVibration(on); }

void setDisplayReset(bool level) {
  // rev4: display RST is not on the expander (panel RST tied on PCB / TFT_RST=-1). No-op.
  // TODO(rev4): confirm the ST7789 reset is strapped on the board; kept for call-site stability.
  (void)level;
}

void setDisplayEnable(bool on) {
  hal::bus::lockI2c();
  s_tca.pinMode(IOX_DISPLAY_EN, OUTPUT);
  s_tca.digitalWrite(IOX_DISPLAY_EN, on ? HIGH : LOW);
  hal::bus::unlockI2c();
}

void setBacklight(bool on) {
  hal::bus::lockI2c();
  s_tca.pinMode(IOX_DISPLAY_BL, OUTPUT);
  s_tca.digitalWrite(IOX_DISPLAY_BL, on ? HIGH : LOW);
  hal::bus::unlockI2c();
}

bool displayEnableReadback() {
  hal::bus::lockI2c();
  const bool high = s_tca.digitalRead(IOX_DISPLAY_EN) == HIGH;
  hal::bus::unlockI2c();
  return high;
}

bool backlightReadback() {
  hal::bus::lockI2c();
  const bool high = s_tca.digitalRead(IOX_DISPLAY_BL) == HIGH;
  hal::bus::unlockI2c();
  return high;
}

void setAudioReset(bool asserted) {
  // AUDIO.~RESET is active-low: asserted (hold in reset) → drive LOW.
  hal::bus::lockI2c();
  s_tca.pinMode(IOX_AUDIO_RESET, OUTPUT);
  s_tca.digitalWrite(IOX_AUDIO_RESET, asserted ? LOW : HIGH);
  hal::bus::unlockI2c();
}

void setCameraReset(bool level) {
  s_tca.digitalWrite(IOX_CAMERA_RESET, level ? HIGH : LOW);
}

void setCameraPwdn(bool level) {
  s_tca.digitalWrite(IOX_CAMERA_PWDN, level ? HIGH : LOW);
}

void setCamera1v8Enable(bool on) {
  // rev4: the camera 1V8 rail is the IOX_REGULATOR_1V8_EN (EN.1V8_CAM) enable.
  s_tca.digitalWrite(IOX_REGULATOR_1V8_EN, on ? HIGH : LOW);
}

void dumpCameraPinState(const char* tag) {
  const int en2v8 = s_tca.digitalRead(IOX_REGULATOR_2V8_EN);
  const int en1v8 = s_tca.digitalRead(IOX_REGULATOR_1V8_EN);
  const int reset = s_tca.digitalRead(IOX_CAMERA_RESET);
  const int pwdn = s_tca.digitalRead(IOX_CAMERA_PWDN);
  Serial.printf("[camera-power] %s 2V8_EN=%d 1V8_EN=%d RESET_N=%d PWDN=%d\n",
                tag ? tag : "", en2v8, en1v8, reset, pwdn);
}

bool readButtonA() {
  if (!s_initialized) return false;
  if (!hal::bus::lockI2c(pdMS_TO_TICKS(25))) return false;
  bool r  = s_tca.digitalRead(IOX_BUTTON_A) == LOW;
  bool r2 = s_tca.digitalRead(IOX_BUTTON_A) == LOW;
  hal::bus::unlockI2c();
  return r && r2;
}

bool readButtonB() {
  if (!s_initialized) return false;
  if (!hal::bus::lockI2c(pdMS_TO_TICKS(25))) return false;
  bool r  = s_tca.digitalRead(IOX_BUTTON_B) == LOW;
  bool r2 = s_tca.digitalRead(IOX_BUTTON_B) == LOW;
  hal::bus::unlockI2c();
  return r && r2;
}

bool readButtonC() {
  if (!s_initialized) return false;
  if (!hal::bus::lockI2c(pdMS_TO_TICKS(25))) return false;
  bool r  = s_tca.digitalRead(IOX_BUTTON_C) == LOW;
  bool r2 = s_tca.digitalRead(IOX_BUTTON_C) == LOW;
  hal::bus::unlockI2c();
  return r && r2;
}

bool readPowerSwitch() {
  bool down=false;
  return readPowerSwitchLevel(down) && down;
}

bool readPowerSwitchLevel(bool& down) {
  if (!s_initialized) return false;
  if (!hal::bus::lockI2c(pdMS_TO_TICKS(25))) return false;
  bool r  = s_tca.digitalRead(IOX_PWRSW) == LOW;
  bool r2 = s_tca.digitalRead(IOX_PWRSW) == LOW;
  hal::bus::unlockI2c();
  if(r!=r2)return false;
  down=r;
  return true;
}

bool readButtonsSnapshot(bool* outA, bool* outB, bool* outC) {
  *outA = false;
  *outB = false;
  if (outC) *outC = false;
  if (!s_initialized) return false;
  if (!hal::bus::lockI2c(pdMS_TO_TICKS(25))) return false;
  // Buttons A/B/C all live on port 1 — one register read captures both edges
  // atomically and twice for debounce.
  const uint8_t p1a = s_tca.readPort(1);
  const uint8_t p1b = s_tca.readPort(1);
  hal::bus::unlockI2c();
  const uint8_t aBit = (uint8_t)(IOX_BUTTON_A & 7);
  const uint8_t bBit = (uint8_t)(IOX_BUTTON_B & 7);
  const uint8_t cBit = (uint8_t)(IOX_BUTTON_C & 7);
  const bool a1 = ((p1a >> aBit) & 1u) == 0u;
  const bool b1 = ((p1a >> bBit) & 1u) == 0u;
  const bool c1 = ((p1a >> cBit) & 1u) == 0u;
  const bool a2 = ((p1b >> aBit) & 1u) == 0u;
  const bool b2 = ((p1b >> bBit) & 1u) == 0u;
  const bool c2 = ((p1b >> cBit) & 1u) == 0u;
  *outA = a1 && a2;
  *outB = b1 && b2;
  if (outC) *outC = c1 && c2;
  return true;
}

bool takeChordAB() {
  // Global A+B chord detector. rev4: poll-ONLY (the TCA6424A has no INTCAP).
  // STRICTLY edge-based: fires exactly once when both A and B reach the pressed
  // state within a small window. Holding the chord down does NOT re-fire. The
  // latch is cleared only when both buttons are released.
  static bool     s_prevA       = false;
  static bool     s_prevB       = false;
  static uint32_t s_downMsA     = 0;
  static uint32_t s_downMsB     = 0;
  static uint32_t s_lastChordMs = 0;
  static bool     s_armed       = true;  // false while either button is still down
  // Time tolerance between the two press edges before they're no longer
  // considered "the same chord". 300 ms is comfortable for a sloppy two-thumb
  // press without being so wide that an intentional B-after-A delayed action
  // accidentally fires the chord.
  static constexpr uint32_t kWindowMs   = 300u;
  static constexpr uint32_t kSameLoopMs = 8u;  // poll de-dup window

  const uint32_t now = millis();

  auto fire = [&](bool aNow, bool bNow) -> bool {
    if (!s_armed) return false;
    if ((uint32_t)(now - s_lastChordMs) <= kSameLoopMs) return false;
    s_lastChordMs = now;
    s_armed       = false;  // re-armed only after both buttons release
    s_prevA       = aNow;
    s_prevB       = bNow;
    return true;
  };

  // Polling path — strictly edge-based.
  bool a = false, b = false;
  (void)readButtonsSnapshot(&a, &b);

  const bool aDown = a && !s_prevA;
  const bool bDown = b && !s_prevB;
  if (aDown) s_downMsA = now;
  if (bDown) s_downMsB = now;

  const bool edgeAB =
      (aDown && (b || (now - s_downMsB <= kWindowMs))) ||
      (bDown && (a || (now - s_downMsA <= kWindowMs)));
  const bool edgeEnterBoth = (a && b) && !(s_prevA && s_prevB);

  s_prevA = a;
  s_prevB = b;

  // Re-arm the chord only after both buttons go fully back up.
  if (!a && !b) s_armed = true;

  if ((edgeAB || edgeEnterBoth) && fire(a, b)) return true;
  return false;
}

}  // namespace io
}  // namespace hal
