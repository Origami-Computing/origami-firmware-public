void onTick(AgentState& s, uint32_t nowMs) {
    const uint8_t phase = (uint8_t)((nowMs / cfg::agent::kDotStepMs) % 3u);
    const char* status = flow::isCommitSent() ? "Thinking" : "Connecting agent";
    if (strncmp(s.thinkStatus, status, sizeof(s.thinkStatus)) != 0) {
        snprintf(s.thinkStatus, sizeof(s.thinkStatus), "%s", status);
        s.thinkDotPhase = 255;
    }
    if (phase != s.thinkDotPhase) {
        render::paintThinkingShell(s, phase);
    }
    if (flow::isReplyReady()) {
        flow::showReplyIfReady();
        return;
    }
    if (flow::isSendDone()) {
        flow::finalizeSend();
        return;
    }

    const uint32_t timeoutNowMs = millis();
    const uint32_t elapsedMs = timeoutNowMs >= sThinkingEnteredMs
        ? (timeoutNowMs - sThinkingEnteredMs)
        : 0u;
    const uint32_t timeoutMs = flow::isCommitSent()
        ? (cfg::agent::kResponseTimeoutMs + 2000u)
        : (cfg::agent::kConnectGraceMs + 2000u);
    if (elapsedMs > timeoutMs) {
        Serial.printf("[agent] thinking watchdog: commitSent=%d elapsed=%lu -> eyes\n",
                      flow::isCommitSent() ? 1 : 0,
                      (unsigned long)elapsedMs);
        flow::returnToHome();
    }
}
