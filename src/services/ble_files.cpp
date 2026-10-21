#include "services/ble_files.h"
#include <Arduino.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "hal/hal_bus.h"

#ifndef BLE_FILE_IDLE_CLOSE_MS
#define BLE_FILE_IDLE_CLOSE_MS 5000u
#endif

namespace services { namespace ble_files {
namespace {
enum class FileState : uint8_t { Idle, OpenPending, Active, ClosePending };

SemaphoreHandle_t s_fileMtx = nullptr;
FileState s_fileState = FileState::Idle;
char      s_filePath[128] = {};
File      s_file;
uint32_t  s_fileLastActivityMs = 0;

struct FileChunk {
  uint8_t  data[256];
  uint16_t len;
};
QueueHandle_t s_chunkQ = nullptr;

bool start() {
  s_fileMtx = xSemaphoreCreateMutex();
  s_chunkQ = xQueueCreate(8, sizeof(FileChunk));
  return s_fileMtx && s_chunkQ;
}

void stop() {
  if (s_file) {
    hal::bus::lockSpi();
    s_file.close();
    hal::bus::unlockSpi();
  }
  if (s_chunkQ) {
    vQueueDelete(s_chunkQ);
    s_chunkQ = nullptr;
  }
  if (s_fileMtx) {
    vSemaphoreDelete(s_fileMtx);
    s_fileMtx = nullptr;
  }
  s_fileState = FileState::Idle;
  s_filePath[0] = '\0';
  s_fileLastActivityMs = 0;
}

void receive(hal::ble::FileWrite kind, const uint8_t* bytes, size_t length) {
  switch (kind) {
    case hal::ble::FileWrite::Begin: {
      if (length == 0) return;
      if (xSemaphoreTake(s_fileMtx, pdMS_TO_TICKS(30)) != pdTRUE) return;
      if (s_fileState == FileState::Idle) {
        size_t len = length;
        if (len >= sizeof(s_filePath)) len = sizeof(s_filePath) - 1;
        memcpy(s_filePath, bytes, len);
        s_filePath[len] = '\0';
        s_fileState = FileState::OpenPending;
        Serial.printf("[ble] file begin: %s\n", s_filePath);
      }
      xSemaphoreGive(s_fileMtx);
      break;
    }
    case hal::ble::FileWrite::Data: {
      if (length == 0) return;
      FileChunk chunk;
      chunk.len = length;
      if (chunk.len > sizeof(chunk.data)) chunk.len = sizeof(chunk.data);
      memcpy(chunk.data, bytes, chunk.len);
      xQueueSend(s_chunkQ, &chunk, pdMS_TO_TICKS(100));
      break;
    }
    case hal::ble::FileWrite::End: {
      if (xSemaphoreTake(s_fileMtx, pdMS_TO_TICKS(30)) != pdTRUE) return;
      if (s_fileState == FileState::Active) {
        s_fileState = FileState::ClosePending;
        Serial.printf("[ble] file end requested: %s\n", s_filePath);
      }
      xSemaphoreGive(s_fileMtx);
      break;
    }
  }
}

void drainChunksToFile() {
  FileChunk chunk;
  while (xQueueReceive(s_chunkQ, &chunk, 0) == pdTRUE) {
    if (s_file) {
      hal::bus::lockSpi();
      s_file.write(chunk.data, chunk.len);
      hal::bus::unlockSpi();
      s_fileLastActivityMs = millis();
    }
  }
}

void processFileTransfer(uint32_t now) {
  if (xSemaphoreTake(s_fileMtx, pdMS_TO_TICKS(5)) != pdTRUE) return;
  FileState state = s_fileState;
  xSemaphoreGive(s_fileMtx);

  switch (state) {
    case FileState::OpenPending: {
      hal::bus::lockSpi();
      s_file = SD.open(s_filePath, FILE_WRITE);
      hal::bus::unlockSpi();

      xSemaphoreTake(s_fileMtx, portMAX_DELAY);
      if (s_file) {
        s_fileState = FileState::Active;
        s_fileLastActivityMs = now;
        Serial.printf("[ble] file opened: %s\n", s_filePath);
      } else {
        s_fileState = FileState::Idle;
        Serial.printf("[ble] file open failed: %s\n", s_filePath);
      }
      xSemaphoreGive(s_fileMtx);
      break;
    }
    case FileState::Active: {
      drainChunksToFile();
      if ((uint32_t)(now - s_fileLastActivityMs) > BLE_FILE_IDLE_CLOSE_MS) {
        hal::bus::lockSpi();
        s_file.close();
        hal::bus::unlockSpi();
        xSemaphoreTake(s_fileMtx, portMAX_DELAY);
        s_fileState = FileState::Idle;
        xSemaphoreGive(s_fileMtx);
        Serial.printf("[ble] file auto-closed (idle): %s\n", s_filePath);
      }
      break;
    }
    case FileState::ClosePending: {
      drainChunksToFile();
      hal::bus::lockSpi();
      s_file.close();
      hal::bus::unlockSpi();
      xSemaphoreTake(s_fileMtx, portMAX_DELAY);
      s_fileState = FileState::Idle;
      xSemaphoreGive(s_fileMtx);
      Serial.printf("[ble] file closed: %s\n", s_filePath);
      break;
    }
    default:
      break;
  }
}

}  // namespace
const hal::ble::FileReceiver receiver{start, stop, receive, processFileTransfer};
} }  // namespace services::ble_files
