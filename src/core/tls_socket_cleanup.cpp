#include <WiFiClientSecure.h>
#include <esp_arduino_version.h>

// Arduino-ESP32 2.0.17 stop_ssl_socket() clears its entire context, including
// socket, to zero. WiFiClientSecure::stop() followed by its destructor then
// calls close(0), closing an unrelated VFS file (often the live WAV). Keep this
// compatibility fix in the project, not in a machine-global PlatformIO package.
// The linker wraps the exact C++ symbol called by WiFiClientSecure::stop().
// Revisit the wrapper and linker flag together when upgrading Arduino.
static_assert(ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(2, 0, 17),
              "Revalidate TLS socket cleanup against the upgraded Arduino core");

extern "C" void __real__Z15stop_ssl_socketP17sslclient_contextPKcS2_S2_(
    sslclient_context*, const char*, const char*, const char*);

extern "C" void __wrap__Z15stop_ssl_socketP17sslclient_contextPKcS2_S2_(
    sslclient_context* client, const char* ca, const char* cert, const char* key) {
  __real__Z15stop_ssl_socketP17sslclient_contextPKcS2_S2_(client, ca, cert, key);
  client->socket = -1;
}
