#pragma once

#include <Arduino.h>
#include "Pins.h"
#include "hal/tca6424a.h"

namespace hal {
namespace io {

bool init();
bool begin();
/** Re-establish the TCA6424A after I2C bus has been restarted (e.g. Wire.end()/Wire.begin()). Call after Wire.begin(), before using buttons. */
bool reinitAfterI2CRestart();

void configureDefaultPins();
/** True if a button edge occurred since last clear (based on the expander ~INT → GPIO1 ISR). Clears the flag when read. */
bool buttonInterruptPending();

// rev4: the TCA6424A has no PMIC strap pin — no-op, kept for call-site stability.
void setPmicHighPower();

/**
 * rev4: charger status pins (BQ24040 ~PG / ~CHG) are inputs on the expander with
 * board-side external pulls. Configures them as inputs.
 */
void configureChargerStatusPins();

/**
 * BQ24040 input-current-limit selector. rev4: ISET2 is hard-strapped by R30 on the
 * board — there is no expander pin for it, so this is a no-op kept for call-site
 * stability.
 */
enum class InputCurrentMode : uint8_t { Usb500mA = 0, Adapter1A, Usb100mA };
void setInputCurrentMode(InputCurrentMode mode);

/** PG raw level — LOW = asserted = "USB / adapter input voltage present". */
bool chargerPgActiveLow();
/** CHG raw level — LOW = asserted = "charger actively pushing current to the cell". */
bool chargerChgActiveLow();
/** Legacy alias used by older call sites — returns PG ("input present"). */
bool chargerStatsActiveLow();

/** Raw 8-bit read of expander port 2 (IOX pins 16..23) — diagnostic helper. */
uint8_t readMcpPortB();
/** Raw 8-bit read of expander port 1 (IOX pins 8..15) — diagnostic helper. */
uint8_t readMcpPortA();
/** One-shot diagnostic dump of expander charger-related registers/pins. */
void dumpChargerPinState(const char* tag);
/** Dump live TCA input, output-latch, and direction registers. */
void dumpExpanderState(const char* tag);
/** Atomically read all three live TCA input ports. */
bool readExpanderInputs(uint8_t* port0, uint8_t* port1, uint8_t* port2);
/** Read one live TCA input port while preserving I2C transaction status. */
bool readExpanderPort(uint8_t port, uint8_t* value, uint8_t* txError,
                      uint8_t* rxCount);

/** Temporarily detach the expander ~INT GPIO interrupt (for ADC mux / diagnostics). */
void pauseButtonInterrupt();
/** Restore the expander ~INT GPIO interrupt. */
void resumeButtonInterrupt();
void enableRegulators(bool enable2v8, bool enable1v8);

/** Enable/disable the Stemma QT 3.3V rail (controlled via expander IOX_STEMMA_POWER_EN). */
void setStemmaPower(bool on);

/** rev4: enables the VSYS system load switch (powers codec, SD, 2V8 rail). */
void setEnVsys(bool on);

// rev4: no SD_EN pin — SD is powered from VSYS. Kept as no-op / true for call-site stability.
void setSdEnable(bool on);
bool sdDetect();
bool sdEnableReadback();

// Both take the shared I2C mutex; writeVibration reports bus/write failure.
bool writeVibration(bool on);
void setVibration(bool on);
/** rev4: display RST is not on the expander — no-op, kept for call-site stability. */
void setDisplayReset(bool level);
/** rev4: display enable via expander IOX_DISPLAY_EN. */
void setDisplayEnable(bool on);
/** rev4: display backlight on/off via expander IOX_DISPLAY_BL (no PWM dimming). */
void setBacklight(bool on);
/** Live expander-pin readbacks for display power diagnostics. */
bool displayEnableReadback();
bool backlightReadback();
/** rev4: TLV320 ~RESET via expander IOX_AUDIO_RESET (active-low). asserted=true → hold in reset. */
void setAudioReset(bool asserted);

void setCameraReset(bool level);
void setCameraPwdn(bool level);
void setCamera1v8Enable(bool on);
/** One-shot live readback of camera controls; caller must own the I2C mutex. */
void dumpCameraPinState(const char* tag);

bool readButtonA();
bool readButtonB();
bool readButtonC();
/** True while rev4 SW1 / PWRSW is physically held. */
bool readPowerSwitch();
// Distinguishes a released switch from an unavailable I2C sample.
bool readPowerSwitchLevel(bool& down);
/** One I2C transaction window: read A/B/C twice each (debounced). Use for chord / edge pairing. */
bool readButtonsSnapshot(bool* outA, bool* outB, bool* outC = nullptr);

/**
 * Returns true once when an A+B chord is detected. rev4: poll-only (the TCA6424A
 * has no INTCAP), strictly edge-based — holding the chord does not re-fire.
 */
bool takeChordAB();

}  // namespace io
}  // namespace hal
