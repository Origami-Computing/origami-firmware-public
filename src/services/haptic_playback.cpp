#include "services/haptic_playback.h"
#include "hal/hal_io.h"
#include "hal/hal_tasks.h"
#include <Arduino.h>
#include <freertos/semphr.h>

namespace services { namespace haptic_playback {
namespace {
// UI creates this worker once, on first audition/navigation feedback. No task
// or timer allocation per pulse. At 1 kHz an alternating pattern costs at most
// 1000 three-byte I2C writes/s; ordinary 0/100% pulses only write their edges.
static_assert(configTICK_RATE_HZ == 1000, "Haptic slots require a 1 ms RTOS tick");
StaticSemaphore_t storage;
SemaphoreHandle_t mutex=nullptr;
TaskHandle_t worker=nullptr;
core::haptic::Effect effect;
Status value;
bool output=false, pendingOff=false, looping=false;
uint32_t leaseAt=0,loopMaximumMs=0;
uint32_t started=0, last=0, accounted=0;

void account(uint32_t now) {
  if(output)value.highUs+=now-accounted;
  value.elapsedUs=now-started;
  accounted=now;
}
bool write(bool on) {
  const bool ok=hal::io::writeVibration(on);
  account(micros());
  if(!ok) {++value.errors;return false;}
  if(on!=output)++value.transitions;
  output=on;
  return true;
}
void finish() {
  value.playing=false;
  pendingOff=!write(false);
}
void run(void*) {
  for(;;) {
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    TickType_t wake=xTaskGetTickCount();
    for(;;) {
      xSemaphoreTake(mutex,portMAX_DELAY);
      if(value.playing) {
        const uint32_t now=micros(), gap=now-last;
        if(gap>value.maxGapUs)value.maxGapUs=gap;
        last=now;
        const uint32_t elapsed=(now-started)/1000;
        if((looping && (uint32_t(now-leaseAt)>=100000 || elapsed>=loopMaximumMs)) ||
           (!looping && elapsed>=core::haptic::duration(effect)))finish();
        else {
          const bool on=core::haptic::driveAt(effect,looping ? elapsed%core::haptic::duration(effect) : elapsed);
          if(on!=output && !write(on))finish();
        }
      } else if(pendingOff)pendingOff=!write(false);
      const bool active=value.playing || pendingOff;
      xSemaphoreGive(mutex);
      if(!active)break;
      // Skip missed slots, never replay a burst of stale edges after contention.
      const TickType_t now=xTaskGetTickCount();
      if(now-wake>1)wake=now;
      vTaskDelayUntil(&wake,1);
    }
  }
}
bool init() {
  if(!mutex)mutex=xSemaphoreCreateMutexStatic(&storage);
  if(!mutex)return false;
  if(!worker)worker=hal::tasks::spawn(hal::tasks::Id::Haptic,run);
  return worker!=nullptr;
}
}
static bool startMode(const core::haptic::Effect& next,bool loop,uint32_t maximumMs=0) {
  if(loop && (!maximumMs || maximumMs>10000))return false;
  if(!core::haptic::valid(next) || !init())return false;
  xSemaphoreTake(mutex,portMAX_DELAY);
  if(value.playing || pendingOff)finish();
  if(pendingOff) {xSemaphoreGive(mutex);return false;}
  effect=next;value=Status{};output=false;looping=loop;loopMaximumMs=maximumMs;leaseAt=micros();
  started=last=accounted=micros();
  value.playing=true;
  const bool ok=write(core::haptic::driveAt(effect,0));
  if(!ok)finish();
  xSemaphoreGive(mutex);
  xTaskNotifyGive(worker);
  return ok;
}
bool start(const core::haptic::Effect& next) {return startMode(next,false);}
bool startLoop(const core::haptic::Effect& next,uint32_t maximumMs) {return startMode(next,true,maximumMs);}
void renewLoop() {
  if(!mutex)return;
  xSemaphoreTake(mutex,portMAX_DELAY);
  if(value.playing && looping)leaseAt=micros();
  xSemaphoreGive(mutex);
}
void stop(bool handingOff) {
  if(!mutex)return;
  xSemaphoreTake(mutex,portMAX_DELAY);
  if(value.playing || pendingOff)finish();
  // Only a handoff cancels OFF retries: they must not overwrite the next
  // owner. An ordinary stop retains cleanup if the bus is temporarily busy.
  if(handingOff)pendingOff=false;
  const bool retry=pendingOff;
  xSemaphoreGive(mutex);
  if(retry)xTaskNotifyGive(worker);
}
Status status() {
  if(!mutex)return Status{};
  xSemaphoreTake(mutex,portMAX_DELAY);
  Status result=value;
  if(value.playing) {
    const uint32_t now=micros();
    result.elapsedUs=now-started;
    if(output)result.highUs+=now-accounted;
  }
  xSemaphoreGive(mutex);
  return result;
}
} }
