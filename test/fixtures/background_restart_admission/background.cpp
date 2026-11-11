std::atomic<uint32_t> s_restartNetPoll{0};
void setPollerRestart(bool requested) {
  uint32_t current = s_restartNetPoll.load();
  uint32_t next;
  do {
    next = ((current + 2u) & ~1u) | uint32_t(requested);
  } while (!s_restartNetPoll.compare_exchange_weak(current, next));
}
void tick() {
  auto* queue = uploads::readyQueue();
  uint32_t request = s_restartNetPoll.load();
  if ((request & 1u) && (!queue || !queue->workerRunning()) && services::net_poll::start()) {
    s_restartNetPoll.compare_exchange_strong(request, request & ~1u);
    Serial.println("[queue] net_poll restarted after upload worker exit");
  }
}
void resumeAfterMusicAudio() {
  setPollerRestart(true);
  auto* queue = uploads::readyQueue();
  if (!queue) return;
  if (!queue->resumeWorker()) {
    Serial.println("[queue] Failed to resume upload worker after Music");
  }
}
AudioSuspension suspendForMusicAudio(uint32_t timeoutMs) {
  if (hal::audio::retirementPending()) return {false, AudioSuspension::Poller::NotRequested};
  auto* queue = uploads::readyQueue();
  if (!queue) return {true, AudioSuspension::Poller::NotRequested};
  if (!queue->stopWorkerForVoice(timeoutMs)) return {false, AudioSuspension::Poller::NotRequested};
  setPollerRestart(false);
  const bool pollerStopped = services::net_poll::stop(timeoutMs);
  Serial.println("[queue] Worker stopped for Music audio");
  return {true, pollerStopped ? AudioSuspension::Poller::Stopped : AudioSuspension::Poller::StopPending};
}
