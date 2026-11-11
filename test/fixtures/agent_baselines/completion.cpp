void finalizeSend() {
    services::agent_voice::poll();
    AgentState& s = s_state;
    const auto r = transport::result();
    transport::clearDone();

    switch (r.outcome) {
        case SendOutcome::Cancelled:
            restoreAudio();
            if (r.cancelTag == static_cast<uint8_t>(CancelTarget::Menu)) {
                goTo(Stage::Eyes);
                requestExitToMenu();
            } else {
                goTo(Stage::Eyes);
            }
            return;

        case SendOutcome::Ok:
            if (s.stage == Stage::Reply) {
                transport::clearDone();
                return;
            }
            if (!s.conv.ensure()) {
                snprintf(s.errBuf, sizeof(s.errBuf), "%s", cfg::agent::kErrReplyUi);
                restoreAudio();
                goTo(Stage::Error);
                return;
            }
            restoreAudio();
            goTo(Stage::Reply);
            return;

        default:
            // Network / WebSocket / memory trouble during a send is transient
            // infrastructure, not a user-facing error. Never dead-end on the
            // error screen (or sit on "Sending audio" forever): fall back to the
            // eyes home, where the bottom "Connecting…" hint shows the live link
            // state and the user can simply try again once it's connected.
            Serial.printf("[agent] finalizeSend failed -> eyes: %s\n", s.errBuf);
            restoreAudio();
            goTo(Stage::Eyes);
            return;
    }
}
