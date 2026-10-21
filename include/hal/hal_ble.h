#pragma once

#include <stdint.h>
#include <stddef.h>

namespace hal {
namespace ble {

enum class FileWrite : uint8_t { Begin, Data, End };

// BLE task delivers borrowed bytes synchronously; poll runs on the UI task.
// start allocates receiver resources before NimBLE starts. stop runs after
// NimBLE stops, or to roll back a failed start (and must tolerate partial init).
// Existing caller serialization between radio lifecycle and UI polling applies.
struct FileReceiver {
  bool (*start)();
  void (*stop)();
  void (*receive)(FileWrite kind, const uint8_t* bytes, size_t length);
  void (*poll)(uint32_t now);
};
// Configure exactly once during single-threaded boot, before any radio use.
// The receiver and its callbacks must remain valid for the device lifetime.
void configureFileReceiver(const FileReceiver& receiver);

bool init(const char* device_name);
void deinit();

bool started();
bool clientConnected();

// Called synchronously on the UI task after releasing the inbox mutex. The
// NUL-terminated payload is borrowed for the call only. A null handler leaves
// the pending message queued; file service still runs. Radio policy is unchanged.
using NotificationHandler = void (*)(const char* json);
void service(uint32_t millis_now, NotificationHandler onNotification);

void prepareForSleep();
void restoreAfterSleep();

}  // namespace ble
}  // namespace hal
