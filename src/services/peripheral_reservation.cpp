#include "services/peripheral_reservation.h"
#include "services/background_network.h"
#include "services/agent_coordination.h"

namespace services { namespace background_network {

PeripheralReservation::Result PeripheralReservation::acquire(bool pauseAgentTransport,
                                                            uint32_t timeoutMs) {
  if (held_) return pauseAgentTransport == agentPause_.held() ? Result::Acquired : Result::PeripheralBusy;
  if (requested_) return Result::PeripheralBusy;
  if (!suspendForPeripheralIo(timeoutMs)) return Result::PeripheralBusy;
  held_ = true;
  if (pauseAgentTransport && !agentPause_.acquire(timeoutMs)) {
    release();
    return Result::AgentBusy;
  }
  return Result::Acquired;
}

PeripheralReservation::Result PeripheralReservation::pollAcquire(bool pauseAgentTransport) {
  if (held_) return pauseAgentTransport == agentPause_.held() ? Result::Acquired : Result::PeripheralBusy;
  if (requested_ && requestedAgent_!=pauseAgentTransport) return Result::PeripheralBusy;
  if (!requested_) {
    requestPeripheralIo();requested_=true;requestedAgent_=pauseAgentTransport;
  }
  if (!peripheralIoReady()) return Result::Pending;
  if (pauseAgentTransport && !agentPause_.pollAcquire()) return Result::Pending;
  held_=true;
  return Result::Acquired;
}

void PeripheralReservation::release() {
  if (!held_ && !requested_) return;
  held_ = requested_ = requestedAgent_ = false;
  resumeAfterPeripheralIo();
  agentPause_.release();
}

} }
