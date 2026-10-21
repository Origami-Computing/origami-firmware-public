#include "hal/hal_ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>


#define BLE_SVC_NOTIFY_UUID     "e6a10001-7f8d-4e3b-9c21-5a2b8d1f0e3c"
#define BLE_CHR_NOTIFY_WRITE    "e6a10002-7f8d-4e3b-9c21-5a2b8d1f0e3c"

#define BLE_SVC_FILE_UUID       "e6a10010-7f8d-4e3b-9c21-5a2b8d1f0e3c"
#define BLE_CHR_FILE_BEGIN      "e6a10011-7f8d-4e3b-9c21-5a2b8d1f0e3c"
#define BLE_CHR_FILE_DATA       "e6a10012-7f8d-4e3b-9c21-5a2b8d1f0e3c"
#define BLE_CHR_FILE_END        "e6a10013-7f8d-4e3b-9c21-5a2b8d1f0e3c"

namespace hal {
namespace ble {

namespace {

NimBLEServer* s_server = nullptr;
volatile uint32_t s_connCount = 0;
bool s_started = false;

// ---- notification buffer (BLE task → main loop) ----
SemaphoreHandle_t s_notifMtx = nullptr;
bool  s_notifPending = false;
char  s_notifJson[512];

const FileReceiver* s_fileReceiver = nullptr;

// ---- BLE callbacks ----

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    s_connCount++;
    Serial.printf("[ble] client connected (%u)\n", (unsigned)s_connCount);
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    if (s_connCount > 0) s_connCount--;
    Serial.printf("[ble] client disconnected (%u)\n", (unsigned)s_connCount);
    NimBLEDevice::getAdvertising()->start();
  }
};

class NotifyWriteCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    NimBLEAttValue val = chr->getValue();
    if (val.length() == 0) return;
    if (xSemaphoreTake(s_notifMtx, pdMS_TO_TICKS(30)) != pdTRUE) return;
    if (!s_notifPending) {
      size_t len = val.length();
      if (len >= sizeof(s_notifJson)) len = sizeof(s_notifJson) - 1;
      memcpy(s_notifJson, val.data(), len);
      s_notifJson[len] = '\0';
      s_notifPending = true;
    }
    xSemaphoreGive(s_notifMtx);
  }
};

class FileWriteCB : public NimBLECharacteristicCallbacks {
 public:
  explicit FileWriteCB(FileWrite kind) : kind_(kind) {}
 private:
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo&) override {
    if (kind_ == FileWrite::End) {
      s_fileReceiver->receive(kind_, nullptr, 0);
      return;
    }
    NimBLEAttValue val = chr->getValue();
    s_fileReceiver->receive(kind_, val.data(), val.length());
  }
  FileWrite kind_;
};

// ---- main-loop processing ----

void processNotification(NotificationHandler onNotification) {
  if (!onNotification) return;
  if (xSemaphoreTake(s_notifMtx, pdMS_TO_TICKS(5)) != pdTRUE) return;
  if (!s_notifPending) { xSemaphoreGive(s_notifMtx); return; }
  char json[512];
  memcpy(json, s_notifJson, sizeof(json));
  s_notifPending = false;
  xSemaphoreGive(s_notifMtx);

  onNotification(json);
}

// Caller must have stopped BLE callbacks, or not started NimBLE yet.
void releaseSessionResources() {
  s_fileReceiver->stop();
  if (s_notifMtx) {
    vSemaphoreDelete(s_notifMtx);
    s_notifMtx = nullptr;
  }
}

}  // namespace

void configureFileReceiver(const FileReceiver& receiver) {
  // Boot composition only: the receiver outlives every radio session.
  s_fileReceiver = &receiver;
}

bool init(const char* device_name) {
  if (s_started) return true;

  if (!s_fileReceiver) {
    Serial.println("[ble] file receiver not configured");
    return false;
  }
  s_notifMtx = xSemaphoreCreateMutex();
  const bool fileReady = s_fileReceiver->start();
  if (!s_notifMtx || !fileReady) {
    Serial.println("[ble] sync primitives alloc failed");
    releaseSessionResources();
    return false;
  }

  NimBLEDevice::init(device_name);
  NimBLEDevice::setMTU(256);

  s_server = NimBLEDevice::createServer();
  s_server->setCallbacks(new ServerCB());

  // notification service
  NimBLEService* notifSvc = s_server->createService(BLE_SVC_NOTIFY_UUID);
  NimBLECharacteristic* notifWr = notifSvc->createCharacteristic(
      BLE_CHR_NOTIFY_WRITE, NIMBLE_PROPERTY::WRITE);
  notifWr->setCallbacks(new NotifyWriteCB());

  // file transfer service
  NimBLEService* fileSvc = s_server->createService(BLE_SVC_FILE_UUID);

  NimBLECharacteristic* fBegin = fileSvc->createCharacteristic(
      BLE_CHR_FILE_BEGIN, NIMBLE_PROPERTY::WRITE);
  fBegin->setCallbacks(new FileWriteCB(FileWrite::Begin));

  NimBLECharacteristic* fData = fileSvc->createCharacteristic(
      BLE_CHR_FILE_DATA, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  fData->setCallbacks(new FileWriteCB(FileWrite::Data));

  NimBLECharacteristic* fEnd = fileSvc->createCharacteristic(
      BLE_CHR_FILE_END, NIMBLE_PROPERTY::WRITE);
  fEnd->setCallbacks(new FileWriteCB(FileWrite::End));

  s_server->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(device_name);
  adv->addServiceUUID(notifSvc->getUUID());
  adv->start();

  s_started = true;
  Serial.printf("[ble] server started: \"%s\"\n", device_name);
  return true;
}

void deinit() {
  if (!s_started) return;
  NimBLEDevice::deinit(true);

  releaseSessionResources();

  s_server = nullptr;
  s_notifPending = false;
  s_started = false;
  s_connCount = 0;
  Serial.println("[ble] server stopped");
}

bool started()         { return s_started; }
bool clientConnected() { return s_connCount > 0; }

void service(uint32_t millis_now, NotificationHandler onNotification) {
  if (!s_started) return;
  processNotification(onNotification);
  s_fileReceiver->poll(millis_now);
}

void prepareForSleep() {
  if (!s_started) return;
  NimBLEDevice::getAdvertising()->stop();
}

void restoreAfterSleep() {
  if (!s_started) return;
  NimBLEDevice::getAdvertising()->start();
}

}  // namespace ble
}  // namespace hal
