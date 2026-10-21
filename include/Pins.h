#ifndef PINS_H
#define PINS_H

// ============================================================================
// Board rev 4 pin map (KiCad rev 4). TI TCA6424A 24-bit I2C expander @ 0x22 on
// the core I2C bus (GPIO15/16). Buttons, IMU/audio INT and most power enables
// live on the expander, not on ESP GPIOs — see the IOX_* block and the
// tca6424a driver. Rotary is direct ESP GPIOs (43/44) — see below. Values
// below are AS-DRAWN on rev 4; macro NAMES are kept stable so existing
// consumers still resolve.
// ============================================================================

// ==================== ESP32-S3 Direct Pins ====================

// CORE I2C — TCA6424A expander (0x22) + BQ27220 fuel gauge (0x55).
#define SDA_PIN 15 //correct
#define SCL_PIN 16 //correct

// ACCESSORY I2C — LSM6DSL IMU, TLV320 codec, OV5640 SCCB + STEMMA QT.
#define ACC_SDA_PIN 4 //correct
#define ACC_SCL_PIN 5 //correct

// I2S Audio Pins (unchanged)
#define I2S_BCLK_PIN 6 //correct
#define I2S_DIN_PIN 7 //correct
#define I2S_WS_PIN 8 //correct
#define I2S_DOUT_PIN 9 //correct
// rev4: audio ~RESET moved to expander P12 (GPIO21 is now CAM.Y7). See hal::io::setAudioReset().
#define AUDIO_RESET_PIN (-1)
// rev4: audio INT moved to expander P06 (was GPIO45, now expander ~RESET line).
#define AUDIO_INT_PIN (-1)

// SPI Pins (Shared) (unchanged)
#define SPI_MISO 13 //correct
#define SPI_SCK 12 //correct
#define SPI_MOSI 11 //correct

// Display Pins
// rev4: backlight (DISP.BL) and enable (DISP.EN) moved to the expander — GPIO16
// is now SCL and GPIO46 is now DC. Both are -1 here; hal_display routes them
// through hal::io::setBacklight()/setDisplayEnable() when the pin is -1.
#define LCD_BACKLIGHT_PIN (-1)
#define LCD_TEAR_PIN 2 //DISP.TED
#define LCD_DC_PIN 46 //DISP.DC
#define LCD_CS_PIN 10 //DISP.CS
#define LCD_RESET_PIN 45 // rev4 display bodge: J5 pin 4 -> ESP32-S3 GPIO45
#define LCD_ENABLE_PIN (-1)

// SD Card Pins (unchanged)
#define SD_CS 18 //SD.CS

// Camera DVP Pins
// rev4: XCLK is an EXTERNAL 24 MHz oscillator — the ESP does NOT generate XCLK.
#define CAMERA_XCLK_PIN (-1)
#define CAMERA_XCLK_FREQ_HZ 24000000
#define CAMERA_Y9_PIN 3 //CORRECT
#define CAMERA_HREF_PIN 17 //CORRECT
#define CAMERA_Y8_PIN 14 //CORRECT
#define CAMERA_Y7_PIN 21 //CORRECT
#define CAMERA_PCLK_PIN 47 //CORRECT
#define CAMERA_Y6_PIN 38 //CORRECT
#define CAMERA_Y2_PIN 39 //CORRECT
#define CAMERA_Y5_PIN 40 //CORRECT
#define CAMERA_Y3_PIN 41 //CORRECT
#define CAMERA_Y4_PIN 42 //CORRECT
#define CAMERA_VSYNC_PIN 48 //CORRECT

// Rotary Encoder — rev4 (corrected): direct ESP32 GPIOs, NOT on the expander.
// Confirmed against the schematic: silkscreen TXD0/RXD0 are repurposed as
// ROT_A/ROT_B. This claims the UART0 console pins; that's fine here because
// USB-CDC (see ARDUINO_USB_CDC_ON_BOOT in platformio.ini) is the active
// Serial console, not UART0.
#define ROTARY_A_PIN 43   // GPIO43 (silkscreen TXD0) — ROT_A
#define ROTARY_B_PIN 44   // GPIO44 (silkscreen RXD0) — ROT_B

// IMU Pin — rev4: IMU INT1 is on the expander (P05). No ESP pin, so ext0
// deep-sleep wake is unavailable (guarded in services.cpp).
#define IMU_INT1_PIN (-1)

// IO Expander Control Pins
// #define IO_EXPANDER_INT_A_PIN 1
#define IO_EXPANDER_INT_B_PIN 1 //CORRECT
/**
 * Optional ESP32 GPIO for a **dedicated** “A+B chord” interrupt (active low).
 * Use -1 on stock boards: chord is decoded in firmware by polling.
 */
#ifndef IO_CHORD_COMBO_INT_PIN
#define IO_CHORD_COMBO_INT_PIN (-1)
#endif
/** rev4: TCA6424A ~RESET is pulled up to +3V3 by R55; ESP32 GPIO45 is NC in
 * Aaron's aaron_dev KiCad source (7c077b9), so firmware must not drive it. */
#define IO_EXPANDER_RESET_PIN -1

// Boot Pin
#define BOOT_PIN 0 //CORRECT

// ==================== Battery sense (VBAT ADC) ====================
//
// ESP32-S3 ADC1 channels = GPIO 1..10, ADC2 = GPIO 11..20. On the Origami
// pin map, GPIO 1 (=ADC1_CH0) is reused as the MCP23017 shared interrupt
// line `IO_EXPANDER_INT_B_PIN`. The legacy hardware revision tapped a 1:1
// VBAT divider onto the same pin — `hal::power::read()` runs a one-shot
// auto-probe at boot to detect whether that tap is present:
//   * pause MCP INT, switch GPIO 1 to plain INPUT (no pull)
//   * sample ADC and multiply by `BAT_ADC_DIVIDER`
//   * if the value lands in the LiPo divider range (≈2.8–4.4 V) → enable
//     periodic battery sensing (every 5 s, multiplexed with the IRQ)
//   * otherwise log the result and stay in "no telemetry" mode (status bar
//     draws an empty battery outline rather than lying)
//
// To force-disable the probe (if your board reroutes GPIO 1 to something
// else and the dance is dangerous) set `-DBAT_ADC_PIN=-1`.
#ifndef BAT_ADC_PIN
// Most Origami boards do NOT route VBAT to an ESP32 ADC pin (no fuel gauge + no divider),
// so percentage cannot be computed in firmware. Leave this disabled by default.
// If you add/verify a divider, override at build time (e.g. `-DBAT_ADC_PIN=7`) and set
// `BAT_ADC_DIVIDER` to match the resistor ratio.
#define BAT_ADC_PIN (-1)
#endif
#ifndef BAT_ADC_DIVIDER
#define BAT_ADC_DIVIDER 2.0f   // 100k/100k tap → ADC sees VBAT/2
#endif
// Optional clamp: if your divider taps to a higher midpoint, raise this so
// the ADC attenuation can be set wide enough to see VBAT_MAX without clipping.
#ifndef BAT_ADC_ATTENUATION_DB
#define BAT_ADC_ATTENUATION_DB 11
#endif

// ==================== IO Expander Pins (TCA6424A @ 0x22) ====================
// TI TCA6424A: 24 GPIOs across 3 ports. Numbering used by the tca6424a driver:
//   port0 = P00..P07 =  0.. 7
//   port1 = P10..P17 =  8..15
//   port2 = P20..P27 = 16..23
// The TCA6424A has NO internal pull resistors and NO interrupt-capture (INTCAP)
// register — inputs must be externally biased and buttons/rotary are polled.

// --- Port 0 (P00..P07) ---
// P00 (0), P01 (1): NOT rotary anymore — rotary moved to direct ESP GPIOs.
// See ROTARY_A_PIN / ROTARY_B_PIN above (GPIO43/44). Unused/no-connect here.
#define IOX_REGULATOR_2V8_EN 2    // [P02 CORRECT] output — EN.2V8_CAM
#define IOX_VIBRATION_MOTOR 3     // [P03 CORRECT] output — vibration motor
#define IOX_STEMMA_POWER_EN 4     // [P04 CORRECT] output — STEMMA QT 3V3 rail enable
#define IOX_IMU_INT1 5            // [P05 CORRECT] input  — LSM6DSL INT1 (no ESP pin on rev4)
#define IOX_AUDIO_INT 6           // [P06 CORRECT] input  — TLV320 audio INT / jack sense
#define IOX_REGULATOR_1V8_EN 7    // [P07 CORRECT] output — EN.1V8_CAM
// P10 (8), P11 (9): no-connect on rev4 — left as (default) inputs, unused.
// --- Port 1 (P10..P17) ---
#define IOX_AUDIO_RESET 10        // P12 = linear index 8+2 — AUDIO.~RESET (active-low; idle HIGH)
#define IOX_PWRSW 11              // P13 = linear index 8+3 — power switch input
#define IOX_BUTTON_C 12           // P14 = linear index 8+4 — button C input
#define IOX_BUTTON_A 13           // P15 = linear index 8+5 — button A input
#define IOX_BUTTON_B 14           // P16 = linear index 8+6 — button B input
#define IOX_DISPLAY_BL 15         // P17 = linear index 8+7 — DISP.BL output
// --- Port 2 (P20..P27) ---
// P20 (16): no-connect on rev4.
/** BQ24040 ~PG status output — open-drain, active LOW = "USB/adapter present". No internal
 * pull on the TCA6424A; the board provides the external pull-up. */
#define IOX_CHARGER_PG 17         // P21 = linear index 16+1
/** BQ24040 ~CHG status output — open-drain, active LOW = "actively charging". */
#define IOX_CHARGER_CHG 18        // P22 = linear index 16+2
#define IOX_EN_VSYS 19            // P23 = linear index 16+3 — VSYS load-switch enable
#define IOX_DISPLAY_EN 20         // P24 = linear index 16+4 — DISP.EN
#define IOX_CAMERA_RESET 21       // P25 = linear index 16+5 — CAM.~RESET (active-low)
#define IOX_CAMERA_PWDN 22        // P26 = linear index 16+6 — CAM.PWDN (active-high)
#define IOX_SD_DETECT 23          // P27 = linear index 16+7 — SD card detect input

// Legacy aliases for backwards compatibility
#define BUTTON_A_PIN IOX_BUTTON_A
#define BUTTON_B_PIN IOX_BUTTON_B
#define BUTTON_C_PIN IOX_BUTTON_C
#define VIBRATION_PIN IOX_VIBRATION_MOTOR

#endif  // PINS_H
