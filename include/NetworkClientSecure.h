#pragma once

#include <Arduino.h>
#include <Client.h>

#if __has_include(<WiFiClientSecure.h>)
#include <WiFiClientSecure.h>
using NetworkClientSecure = WiFiClientSecure;
#else
class NetworkClientSecure : public Client {
 public:
  int connect(IPAddress ip, uint16_t port) override { (void)ip; (void)port; return 0; }
  int connect(const char* host, uint16_t port) override { (void)host; (void)port; return 0; }
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) override { return 0; }
  int available() override { return 0; }
  int read() override { return -1; }
  int read(uint8_t*, size_t) override { return 0; }
  int peek() override { return -1; }
  void flush() override {}
  void stop() override {}
  uint8_t connected() override { return 0; }
  operator bool() override { return false; }
};
#endif
