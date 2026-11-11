bool ensureStarted() {
    if (s_sendHandle) return true;
    s_sendHandle = hal::tasks::spawn(hal::tasks::Id::KamiSend, kamiSendTask);
    if (s_sendHandle) services::agent_coordination::workerStarted();
    return s_sendHandle != nullptr;
}

void bindVoiceCallbacks(const VoiceCallbacks& callbacks) { s_callbacks.store(&callbacks); }
