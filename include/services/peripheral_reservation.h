#pragma once
#include "services/agent_coordination.h"
#include <stdint.h>

namespace services { namespace background_network {

// UI-task reservation using the existing peripheral/network policy. The
// holder may keep it through file finalization and queue insertion.
class PeripheralReservation {
public:
  enum class Result { Acquired, PeripheralBusy, AgentBusy, Pending };
  ~PeripheralReservation() { release(); }
  PeripheralReservation() = default;
  PeripheralReservation(const PeripheralReservation&) = delete;
  PeripheralReservation& operator=(const PeripheralReservation&) = delete;
  // Repeated acquisition is idempotent only for the same transport policy.
  Result acquire(bool pauseAgentTransport = false, uint32_t timeoutMs = 5000);
  // UI-task preview admission. Retains the request across Pending results;
  // release cancels it. Do not use hardware until Acquired. No worker is added.
  Result pollAcquire(bool pauseAgentTransport = false);
  void release();
  bool held() const { return held_; }
private:
  bool held_ = false;
  bool requested_ = false, requestedAgent_ = false;
  agent_coordination::TransportPause agentPause_;
};

} }
