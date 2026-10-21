#include "services/peripheral_reservation.h"
#include "services/background_network.h"
#include "services/agent_coordination.h"

namespace services { namespace background_network {

PeripheralReservation::Result PeripheralReservation::acquire(bool pauseAgentTransport,
                                                            uint32_t timeoutMs) {
  if (held_) return pauseAgentTransport == agentPause_.held() ? Result::Acquired : Result::PeripheralBusy;
  if (!suspendForPeripheralIo(timeoutMs)) return Result::PeripheralBusy;
  held_ = true;
  if (pauseAgentTransport && !agentPause_.acquire(timeoutMs)) {
    release();
    return Result::AgentBusy;
  }
  return Result::Acquired;
}

void PeripheralReservation::release() {
  if (!held_) return;
  held_ = false;
  resumeAfterPeripheralIo();
  agentPause_.release();
}

} }
