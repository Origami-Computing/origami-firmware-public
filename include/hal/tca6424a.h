#pragma once

#include <Arduino.h>
#include <Wire.h>

// ============================================================================
// TI TCA6424A — 24-bit I2C GPIO expander (rev4 board, replaces the MCP23017).
//
// 3 ports of 8 bits. Pin numbering used throughout the firmware:
//   port0 = P00..P07 =  0.. 7
//   port1 = P10..P17 =  8..15
//   port2 = P20..P27 = 16..23
//
// Registers (command byte; auto-increment bit 0x80 is NOT used here — we address
// one port register per transaction):
//   Input    0x00 / 0x01 / 0x02   (read-only)
//   Output   0x04 / 0x05 / 0x06
//   Polarity 0x08 / 0x09 / 0x0A   (input inversion; left at 0)
//   Config   0x0C / 0x0D / 0x0E   (1 = input [POR default], 0 = output)
//
// The TCA6424A has NO internal pull resistors and NO interrupt-capture register,
// so INPUT_PULLUP is treated as INPUT and button/rotary state must be polled.
//
// RAM shadows of the 3 output and 3 config registers back read-modify-write of
// single pins without a wasteful read-back over I2C.
// ============================================================================

class TCA6424A {
 public:
  bool begin(TwoWire& w, uint8_t addr = 0x22) {
    m_wire = &w;
    m_addr = addr;

    // Verify the device ACKs at addr before trusting any register state.
    m_wire->beginTransmission(m_addr);
    if (m_wire->endTransmission(true) != 0) {
      return false;
    }

    // POR defaults: all inputs (config=0xFF), outputs latch 0xFF, polarity 0.
    for (uint8_t port = 0; port < 3; port++) {
      m_config[port] = 0xFF;
      m_output[port] = 0xFF;
      writeReg((uint8_t)(kRegPolarity + port), 0x00);
      writeReg((uint8_t)(kRegOutput + port), m_output[port]);
      writeReg((uint8_t)(kRegConfig + port), m_config[port]);
    }
    return true;
  }

  // Reattach after Wire.end()/Wire.begin() without replacing the logical GPIO
  // state. The expander normally remains powered across an MCU I2C restart; if
  // it did brown out, replaying the shadows also restores the intended rails.
  bool reconnect(TwoWire& w, uint8_t addr = 0x22) {
    m_wire = &w;
    m_addr = addr;
    m_wire->beginTransmission(m_addr);
    if (m_wire->endTransmission(true) != 0) return false;
    for (uint8_t port = 0; port < 3; port++) {
      if (!writeReg((uint8_t)(kRegPolarity + port), 0x00)) return false;
      // Load output latches before changing direction to avoid rail glitches.
      if (!writeReg((uint8_t)(kRegOutput + port), m_output[port])) return false;
      if (!writeReg((uint8_t)(kRegConfig + port), m_config[port])) return false;
    }
    return true;
  }

  // mode: INPUT / OUTPUT. INPUT_PULLUP is accepted but treated as INPUT — the
  // TCA6424A has no internal pulls (rely on external bias on the board).
  void pinMode(uint8_t pin, uint8_t mode) {
    if (pin > 23) return;
    const uint8_t port = (uint8_t)(pin >> 3);
    const uint8_t bit  = (uint8_t)(pin & 7);
    if (mode == OUTPUT) {
      m_config[port] &= (uint8_t)~(1u << bit);  // 0 = output
    } else {
      m_config[port] |= (uint8_t)(1u << bit);   // 1 = input
    }
    writeReg((uint8_t)(kRegConfig + port), m_config[port]);
  }

  bool digitalWrite(uint8_t pin, uint8_t val) {
    if (pin > 23) return false;
    const uint8_t port = (uint8_t)(pin >> 3);
    const uint8_t bit  = (uint8_t)(pin & 7);
    if (val) {
      m_output[port] |= (uint8_t)(1u << bit);
    } else {
      m_output[port] &= (uint8_t)~(1u << bit);
    }
    return writeReg((uint8_t)(kRegOutput + port), m_output[port]);
  }

  int digitalRead(uint8_t pin) {
    if (pin > 23) return LOW;
    const uint8_t port = (uint8_t)(pin >> 3);
    const uint8_t bit  = (uint8_t)(pin & 7);
    const uint8_t v = readPort(port);
    return ((v >> bit) & 1u) ? HIGH : LOW;
  }

  // port: 0..2 -> live Input register read (P00..P07 = bits 0..7, etc.).
  bool readPort(uint8_t port, uint8_t* value, uint8_t* txError = nullptr,
                 uint8_t* rxCount = nullptr) {
    if (value) *value = 0xFF;
    if (txError) *txError = 0xFF;
    if (rxCount) *rxCount = 0;
    if (port > 2 || !m_wire || !value) return false;

    m_wire->beginTransmission(m_addr);
    m_wire->write((uint8_t)(kRegInput + port));
    const uint8_t tx = m_wire->endTransmission(false);
    if (txError) *txError = tx;
    if (tx != 0) return false;

    const uint8_t received = (uint8_t)m_wire->requestFrom((int)m_addr, 1);
    if (rxCount) *rxCount = received;
    if (received != 1) return false;
    *value = (uint8_t)m_wire->read();
    return true;
  }

  uint8_t readPort(uint8_t port) {
    uint8_t value = 0xFF;
    readPort(port, &value);
    return value;
  }

 private:
  static constexpr uint8_t kRegInput    = 0x00;
  static constexpr uint8_t kRegOutput   = 0x04;
  static constexpr uint8_t kRegPolarity = 0x08;
  static constexpr uint8_t kRegConfig   = 0x0C;

  bool writeReg(uint8_t reg, uint8_t val) {
    if (!m_wire) return false;
    m_wire->beginTransmission(m_addr);
    m_wire->write(reg);
    m_wire->write(val);
    return m_wire->endTransmission(true) == 0;
  }

  TwoWire* m_wire = nullptr;
  uint8_t  m_addr = 0x22;
  uint8_t  m_config[3] = {0xFF, 0xFF, 0xFF};
  uint8_t  m_output[3] = {0xFF, 0xFF, 0xFF};
};
