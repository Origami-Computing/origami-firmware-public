void onTick(AgentState& s, uint32_t nowMs) {
    if (flow::isSendDone()) {
        flow::finalizeSend();
    }

    render::serviceTypewriter(s, nowMs);

    if ((nowMs - s.lastAnim) >= cfg::agent::kIdleReplyPeriodMs) {
        s.animFrame++;
        s.lastAnim = nowMs;
        if (nowMs >= s.miniBlinkMs) {
            if (s.miniEyesOpen) {
                s.miniEyesOpen = false;
                s.miniBlinkMs  = nowMs + cfg::agent::kMiniBlinkClosedMs;
            } else {
                s.miniEyesOpen = true;
                scheduleNextMiniBlink(s, nowMs);
            }
            render::paintMiniEyes(s);
        }
    }
}
