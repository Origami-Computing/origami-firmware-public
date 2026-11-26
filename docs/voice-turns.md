# Voice turns: conversation view, A+B gesture and turn cues

Current contract for how talking to Kami looks, sounds and is controlled on the
`codex/voice-streaming-handoff` line of work. Design proposal and mockups:
"Kami Voice Turns" (private design artifact). Device validation is still pending;
nothing here claims on-device acceptance.

## Controls

| Where | Input | Result |
| --- | --- | --- |
| Home, reply, error | A+B tap (held under `kTapHoldMs`, 250 ms) | Agent mode in the same mic session (double buzz): the mic stays open and the server ends each turn |
| Home, reply, error | A+B held past `kTapHoldMs` | Push-to-talk; releasing **both** buttons sends |
| Agent mode | A tap or A+B | Leave agent mode (cancel cue once the mic has closed) |
| Agent mode | `kAgentSilenceMs` (60 s) without speech, replies or work | Leaves by itself so idle sleep can run |
| Thinking | A | Cancel (cancel cue) |
| Reply | Wheel | Scroll; scrolling to the end follows new text |
| Anywhere | C hold | Sleep (unchanged). A short C tap has no conversation meaning |

A+B down opens the microphone at once (12 ms haptic; no cue, I2S is
half-duplex) with the session's start frame held (`agent_transport::holdStart`),
so PCM buffers and no word is lost. Tap vs hold uses `hal::input::chordHeldMs()`:
the time from the first poll that saw A and B down to the last poll that still
did. UI-loop stalls only shorten it, so a quick tap never reads as a hold. A hold
releases the start as push-to-talk; a tap switches the same session to
continuous before releasing it. Logic: `flow::beginTurnGesture`,
`flow::serviceTurnGesture` (recording tick), `flow::takeTapAsAgentMode`
(recording release). Agent mode is the existing continuous transport mode.

## View

One painter, `ui::agent_conversation` (pure drawing), composed by
`render::paintConversation` from workflow state. The status bar is hidden on
Recording, Thinking and Reply through `presentationPolicy()`
(`StatusBar::Hidden`, `contentUsesStatusArea`); Home and errors keep it.

- Listening: large centred eyes; the eyes bob with the mic level; your words on
  one line (baseline y 212), centred while they fit in 232 px, otherwise
  right-anchored with a left-edge fade. The last word is grey until final.
- Thinking: your line dims; the eyes glance up and right, then look side to side
  after 3 s.
- One-line answers (24 px, or 20 px if needed) keep the large eyes and sit under
  them; in agent mode slightly higher so your live line still fits.
- Several messages: one per screen, "2 of 3" top-right; B next, A back. Agent
  mode shows the newest message. In agent mode the answer stays until the next
  turn starts; stray transcripts only change the bottom line.
- Two eye positions only: big and centred (the Home renderer itself:
  `drawConversationEyes`, `drawThinkingEyes`) and small at the top. Size changes
  play the Home renderer's cached travel frames; small eyes use its shared blink.
  Once a long reply raises the eyes they stay up until Home (`eyesUp`), so
  follow-ups and agent-mode turns don't bounce; short answers then show as reply
  text.
- Reply: the eyes ease to the small size (380 ms ease-out) and the reply gets five
  18 px lines (four in agent mode, with "Listening..." and the red mic marker
  below). The shared scrollbar geometry shows position.
- Documents keep their existing sheet; out of scope for this pass.

Repaints are full-panel into the framebuffer, at most every 33 ms, and skipped when
nothing visible changed. When only the listening eyes moved, only rows 0–170 are
marked dirty.

## Reply reveal and cues

The reveal chases received text (`kRevealCharMs`, never more than
`kRevealMaxLag` characters behind); the first chunk paints at once, so
first-paint latency is unchanged.

Cues are the phone prototype's product sounds (`sfx::product`, a port of
`ProductSound`; recipes are data, `render()` is pure and host-tested against a
reference transcription in `tools/test_product_sounds.py`). I2S is half-duplex,
so sounds play only while the mic is closed; `flow::cue` waits up to `kCueWaitMs`
for it to close and otherwise drops the cue.

| Moment | Cue |
| --- | --- |
| A+B down | 12 ms haptic (mic already open) |
| A+B tap | 30 ms haptic (mic stays open) |
| Push-to-talk send | Send |
| Reply revealing (push-to-talk, Voice off) | Typing, ≥ 56 ms apart |
| Push-to-talk reply finished | WaterDrop |
| Cancel / leave agent mode | Cancel |
| Agent mode: Kami takes the turn, or listens again | 12 ms haptic (`vibration::pulse`, Navigation owner) |

## Known limits

- Home eyes and conversation eyes use different renderers; entering a
  conversation is a cut, not a tween. Listening ↔ reply sizes do tween.
- Error notices keep their existing layout; only their retry hint changed to A+B.
- Cue levels (`kLoudness`) and the mic-level scale need tuning on the speaker.
