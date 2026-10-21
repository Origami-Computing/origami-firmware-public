#include "hal/hal_input.h"
#include "core/rotary_decoder.h"
#include <esp_heap_caps.h>
#include "hal/hal_encoder_trace.h"

#include "Pins.h"
#include "driver/gpio.h"
#include "hal/hal_io.h"
#include "core/menu_config.h"
#include "core/power_button.h"

namespace hal {
namespace input {

// ---------------------------------------------------------------------------
// Event ring buffer
// ---------------------------------------------------------------------------

static constexpr int kRingSize = 16;
static InputEvent s_ring[kRingSize];
static volatile uint8_t s_ringHead = 0;
static volatile uint8_t s_ringTail = 0;
static bool s_injectedC=false;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
static uint8_t s_testButtons = 0;
void setTestButtons(uint8_t mask) { s_testButtons = mask & 3u; }
#endif
static bool s_wakeSuppressed=true;
static core::power_button::Gesture s_powerGesture;

static bool takeRotaryScroll(core::rotary::Scroll& out);

static void enqueue(EventType type, int16_t delta = 0, uint8_t key = 0, uint32_t gesture=0, bool undo=false) {
    static uint32_t serial=0;
    const uint32_t id=type==EventType::Scroll?++serial:0;
    uint8_t next = (uint8_t)((s_ringHead + 1) % kRingSize);
    if (next == s_ringTail) {if(id)encoderTrace(TraceKind::QueueFull,id,delta);return;}
    if(id)encoderTrace(TraceKind::Queue,id,delta,gesture,undo);
    InputEvent e;
    e.type  = type;
    e.delta = delta;
    e.key   = key;
    e.traceId = id;e.rotaryGesture=gesture;e.rotaryUndo=undo;
    s_ring[s_ringHead] = e;
    s_ringHead = next;
}

bool poll(InputEvent& out) {
    if (s_ringTail == s_ringHead) return false;
    out = s_ring[s_ringTail];
    if(out.type==EventType::Scroll)encoderTrace(TraceKind::Poll,out.traceId,out.delta);
    s_ringTail = (uint8_t)((s_ringTail + 1) % kRingSize);
    return true;
}

void injectEvent(EventType type, int16_t delta, uint8_t key) {
    if(type==EventType::ButtonC_Press) { s_injectedC=true; return; }
    if(type==EventType::ButtonC_Release) { s_injectedC=false; return; }
    if(type==EventType::ButtonC_Tap) return; // Generated on release by the shared gesture.
    if (type == EventType::None) return;
    enqueue(type, delta, key);
}

// ---------------------------------------------------------------------------
// Button + chord state machine
// ---------------------------------------------------------------------------

static bool s_prevA = false;
static bool s_prevB = false;
static bool s_prevC = false;
static bool s_chordConsumed = false;
static uint32_t s_lastChordMs = 0;
// Per-button chord tracking: set when chord fires, cleared on release.
// Used to suppress Tap on the release that follows a chord press.
static bool s_aWasChord = false;
static bool s_bWasChord = false;

// Hold-fallback state
static uint32_t s_abHoldStartMs      = 0;
static uint32_t s_abLastBothDownMs   = 0;
static uint8_t  s_abBothConsec       = 0;

static bool s_curA = false;
static bool s_curB = false;
static bool s_curC = false;

bool buttonADown() { return s_curA; }
bool buttonBDown() { return s_curB; }
bool buttonCDown() { return s_curC; }

void suppressUntilButtonsReleased() {
    s_wakeSuppressed=true;
    s_powerGesture.suppress();
    encoderTrace(TraceKind::Clear,0,(s_ringHead+kRingSize-s_ringTail)%kRingSize);
    s_ringTail=s_ringHead;
}

static void fireChord(uint32_t now) {
    s_lastChordMs = now;
    s_chordConsumed = true;
    s_aWasChord     = true;
    s_bWasChord     = true;
    s_abHoldStartMs = 0;
    s_abBothConsec  = 0;
    enqueue(EventType::Chord_AB);
}

void service(uint32_t nowMs) {
    encoderTraceService();
    // --- Read physical buttons (MCP23017 via hal::io) ---
    bool buttonA = false, buttonB = false, buttonC = false;
    if (!hal::io::readButtonsSnapshot(&buttonA, &buttonB, &buttonC)) {
        s_powerGesture.suppress(); // Unknown input must not complete a power hold.
        // I2C read failed (bus contended or briefly wedged — see the 263 issue).
        // readButtonsSnapshot reports both buttons UP on failure; using that would
        // inject a phantom "released" mid-press, dropping a real press or firing a
        // spurious tap — i.e. buttons feel intermittently dead until I2C recovers.
        // Reuse the last known state so this cycle produces no false edges. Rotary
        // (GPIO, below) is independent of I2C and still services normally.
        buttonA = s_curA;
        buttonB = s_curB;
        buttonC = s_curC;
    }
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    // Exercise the real chord/release state machine, not just its output event.
    buttonA = buttonA || (s_testButtons & 1u);
    buttonB = buttonB || (s_testButtons & 2u);
#endif
    buttonC=buttonC || s_injectedC;
    if(s_wakeSuppressed) {
        s_prevA=s_curA=buttonA; s_prevB=s_curB=buttonB; s_prevC=s_curC=buttonC;
        s_powerGesture.suppress();
        const int discarded=takeRotaryDetents();
        if(discarded)encoderTrace(TraceKind::Suppressed,0,discarded);
        if(!buttonA && !buttonB && !buttonC) {
            s_wakeSuppressed=false;
            s_powerGesture.update(false,nowMs);
        }
        return;
    }
    s_curA = buttonA;
    s_curB = buttonB;
    s_curC = buttonC;

    // Reset chord latch when both buttons are up
    if (!buttonA && !buttonB) {
        s_chordConsumed = false;
    }

    // Edge detection
    const bool pressedA  = buttonA && !s_prevA;
    const bool pressedB  = buttonB && !s_prevB;
    const bool releasedA = !buttonA && s_prevA;
    const bool releasedB = !buttonB && s_prevB;
    const bool chordBoth = buttonA && buttonB;

    // --- Chord detection (edge-based) ---
    const bool chordEdgeAB    = (pressedA && buttonB) || (pressedB && buttonA);
    const bool chordBothEnter = chordBoth && !(s_prevA && s_prevB);
    const bool chordComboIntent = chordEdgeAB || chordBothEnter;

    // rev4: the TCA6424A has no interrupt-capture register, so there is no INTCAP
    // path — chord/edge detection is poll-only (see hal::io::takeChordAB).
    bool halChordFired = false;

    // HAL chord detector (poll-only, from hal::io)
    const bool halSawChord = hal::io::takeChordAB();
    if (halSawChord && !s_chordConsumed) {
        fireChord(nowMs);
        halChordFired = true;
    }

    // Local edge fallback
    if (!halChordFired && !s_chordConsumed && chordComboIntent &&
        (uint32_t)(nowMs - s_lastChordMs) > 8u) {
        fireChord(nowMs);
    }

    // Hold-timer fallback: if both buttons are held past MENU_CHORD_HOLD_MS
    // without either edge path firing, force the chord.
    if (s_chordConsumed) {
        s_abHoldStartMs    = 0;
        s_abLastBothDownMs = 0;
        s_abBothConsec     = 0;
    } else if (buttonA && buttonB) {
        if (s_abBothConsec < 255) s_abBothConsec++;
        s_abLastBothDownMs = nowMs;
        if (s_abBothConsec >= 2) {
            if (s_abHoldStartMs == 0) s_abHoldStartMs = nowMs;
            if ((uint32_t)(nowMs - s_abHoldStartMs) >= MENU_CHORD_HOLD_MS) {
                fireChord(nowMs);
            }
        }
    } else if (s_abHoldStartMs != 0) {
        if ((uint32_t)(nowMs - s_abLastBothDownMs) > MENU_CHORD_HOLD_GRACE_MS) {
            s_abHoldStartMs    = 0;
            s_abLastBothDownMs = 0;
            s_abBothConsec     = 0;
        }
    } else {
        s_abBothConsec = 0;
    }

    // --- Emit individual button events (suppressed during chord) ---
    const bool suppress = chordBoth || chordComboIntent || s_chordConsumed;
    if (!suppress) {
        if (pressedA)  enqueue(EventType::ButtonA_Press);
        if (pressedB)  enqueue(EventType::ButtonB_Press);
    }
    if (releasedA) {
        enqueue(EventType::ButtonA_Release);
        if (!s_aWasChord) enqueue(EventType::ButtonA_Tap);
        s_aWasChord = false;
    }
    if (releasedB) {
        enqueue(EventType::ButtonB_Release);
        if (!s_bWasChord) enqueue(EventType::ButtonB_Tap);
        s_bWasChord = false;
    }
    const auto powerAction=s_powerGesture.update(buttonC,nowMs);
    if(powerAction==core::power_button::Action::Sleep) enqueue(EventType::PowerSleep);
    if(powerAction==core::power_button::Action::Click) {
        enqueue(EventType::ButtonC_Press);
        enqueue(EventType::ButtonC_Release);
        enqueue(EventType::ButtonC_Tap);
    }

    s_prevA = buttonA;
    s_prevB = buttonB;
    s_prevC = buttonC;

    // --- Rotary encoder ---
    core::rotary::Scroll step;
    // Preserve early/rollback ordering instead of merging opposite events.
    for(unsigned i=0;i<core::rotary::Decoder::kQueueSize && takeRotaryScroll(step);++i)
        enqueue(EventType::Scroll,step.delta,0,step.gesture,step.undo);
}

// ---------------------------------------------------------------------------
// Rotary encoder (unchanged)
// ---------------------------------------------------------------------------

static bool s_rotaryReady = false;

// Four transitions per complete cycle, anchored to the measured resting state.
static core::rotary::Decoder s_rotary;
static portMUX_TYPE     s_rotaryMux         = portMUX_INITIALIZER_UNLOCKED;
static bool             s_rotaryIsrInstalled = false;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
static constexpr uint32_t kTraceCapacity=4096;
static core::rotary::TraceStaging<128,kTraceCapacity> s_trace;
// Retained, bounded, single capture. Never touched by ISR or with cache disabled.
static core::rotary::TraceRecord* s_traceArchive=nullptr;
static uint32_t s_traceArchived=0;
void encoderTraceService(){
  // Bound UI work even if the encoder keeps moving while we drain.
  for(unsigned i=0;i<128;++i){
    core::rotary::TraceRecord r;
    portENTER_CRITICAL(&s_rotaryMux);const bool have=s_trace.pop(r);portEXIT_CRITICAL(&s_rotaryMux);
    if(!have)break;
    s_traceArchive[s_traceArchived++]=r;
  }
}
static uint32_t s_traceBase=0,s_traceContext=0;
static inline __attribute__((always_inline)) void traceLocked(TraceKind kind,uint32_t event=0,int a=0,int b=0,int c=0,int d=0) {
  if(!s_trace.active)return;
  s_trace.append({micros(),event,(int32_t)(s_rotary.forward-s_rotary.reverse-s_traceBase),a,b,c,d,kind});
}
void encoderTrace(TraceKind kind,uint32_t event,int a,int b,int c,int d) {
  encoderTraceService();
  portENTER_CRITICAL(&s_rotaryMux);traceLocked(kind,event,a,b,c,d);portEXIT_CRITICAL(&s_rotaryMux);
}
void encoderTraceContext(uint32_t event){s_traceContext=event;}
void encoderTraceMenu(int before,int after,int total,int delta){
  encoderTrace(TraceKind::Menu,s_traceContext,before,after,total,delta);
}
void encoderTraceStart(){
  if(!s_traceArchive)s_traceArchive=static_cast<core::rotary::TraceRecord*>(heap_caps_malloc(kTraceCapacity*sizeof(core::rotary::TraceRecord),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(!s_traceArchive)return; // Preserve normal operation on allocation failure.
  s_traceArchived=0;
  portENTER_CRITICAL(&s_rotaryMux);
  s_traceBase=s_rotary.forward-s_rotary.reverse;s_trace.start();
  traceLocked(TraceKind::Start,0,s_rotary.state,-s_rotary.partial,-s_rotary.pending,s_rotary.resyncs);
  portEXIT_CRITICAL(&s_rotaryMux);
}
void encoderTraceStop(){portENTER_CRITICAL(&s_rotaryMux);s_trace.stop();portEXIT_CRITICAL(&s_rotaryMux);}
EncoderTracePage encoderTracePage(uint32_t offset){
  encoderTraceService();
  EncoderTracePage page{};
  portENTER_CRITICAL(&s_rotaryMux);
  page.generation=s_trace.generation;page.count=s_trace.count;page.active=s_trace.active;
  page.overflow=s_trace.overflow;page.overflowReason=s_trace.overflowReason;
  portEXIT_CRITICAL(&s_rotaryMux);
  page.capacity=kTraceCapacity;page.storageReady=s_traceArchive!=nullptr;page.offset=offset;
  // External-RAM reads happen only on the UI task, outside critical sections.
  if(!page.active && offset<s_traceArchived){page.size=s_traceArchived-offset;if(page.size>16)page.size=16;
    for(uint32_t i=0;i<page.size;++i)page.records[i]=s_traceArchive[offset+i];}
  return page;
}
#endif


#if ROTARY_A_PIN >= 0 && ROTARY_B_PIN >= 0
// --- ISR-driven quadrature decoder (rotary on ESP GPIOs) -------------------

static void IRAM_ATTR rotaryIsr() {
  // Read both pins inside the ISR to capture the *current* state, not what
  // was at the moment of the edge — this naturally debounces rapid bounce
  // because the lookup returns 0 for invalid transitions.
  uint8_t s    = (uint8_t)((digitalRead(ROTARY_A_PIN) ? 2 : 0) | (digitalRead(ROTARY_B_PIN) ? 1 : 0));
  portENTER_CRITICAL_ISR(&s_rotaryMux);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  const int before=s_rotary.state,partial=-s_rotary.partial,pending=s_rotary.pending;
#endif
  s_rotary.step(s);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if(before!=s)traceLocked(TraceKind::Edge,0,before*4+s,partial,-s_rotary.partial,pending-s_rotary.pending);
#endif
  portEXIT_CRITICAL_ISR(&s_rotaryMux);
}
#endif  // ROTARY_A_PIN >= 0 && ROTARY_B_PIN >= 0

// rev4 (corrected): rotary is on direct ESP GPIOs 43/44 with the ISR-driven
// decoder above — see rotaryIsr(). This poll fallback only exists so a build
// with ROTARY_A_PIN/B_PIN left unset (-1) still links; there is no expander
// (IO expander) rotary path anymore, so it's a no-op stub in that case.
#if !(ROTARY_A_PIN >= 0 && ROTARY_B_PIN >= 0)
static void rotaryPollStep() {
  // No rotary hardware wired on this board revision — nothing to decode.
}
#endif

// --- Public API ------------------------------------------------------------

void configureRotaryEncoderPins() {
#if ROTARY_A_PIN >= 0 && ROTARY_B_PIN >= 0
  pinMode(ROTARY_A_PIN, INPUT_PULLUP);
  pinMode(ROTARY_B_PIN, INPUT_PULLUP);
#endif
  s_rotaryReady = true;
}

void resetRotaryEncoderPins() {
#if ROTARY_A_PIN >= 0 && ROTARY_B_PIN >= 0
  // esp_camera / peripheral routing can leave pins in a non-GPIO mode until explicitly reset.
  if (s_rotaryIsrInstalled) {
    detachInterrupt(digitalPinToInterrupt(ROTARY_A_PIN));
    detachInterrupt(digitalPinToInterrupt(ROTARY_B_PIN));
    s_rotaryIsrInstalled = false;
  }
  gpio_reset_pin((gpio_num_t)ROTARY_A_PIN);
  gpio_reset_pin((gpio_num_t)ROTARY_B_PIN);
#endif
  s_rotaryReady = false;
  configureRotaryEncoderPins();
}

int readRotaryA() {
#if ROTARY_A_PIN >= 0
  configureRotaryEncoderPins();
  return digitalRead(ROTARY_A_PIN);
#else
  return HIGH;  // no rotary wired on this board revision
#endif
}

int readRotaryB() {
#if ROTARY_B_PIN >= 0
  configureRotaryEncoderPins();
  return digitalRead(ROTARY_B_PIN);
#else
  return HIGH;  // no rotary wired on this board revision
#endif
}

void beginRotaryInterrupts() {
  configureRotaryEncoderPins();
  resyncRotaryQuadrature();
#if ROTARY_A_PIN >= 0 && ROTARY_B_PIN >= 0
  // attachInterrupt with CHANGE replaces any previous handler on the same
  // pin, so it is safe to call this every time we resume from camera etc.
  attachInterrupt(digitalPinToInterrupt(ROTARY_A_PIN), rotaryIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ROTARY_B_PIN), rotaryIsr, CHANGE);
#endif
  // rev4: no ESP GPIO ISR — mark "installed" so takeRotaryDetents() runs the
  // software poll decoder instead of returning 0.
  s_rotaryIsrInstalled = true;
}

static bool takeRotaryScroll(core::rotary::Scroll& out){
  if(!s_rotaryIsrInstalled)return false;
  portENTER_CRITICAL(&s_rotaryMux);
  const bool have=s_rotary.takeScroll(out);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  static uint32_t reportedDrops=0;
  if(s_rotary.eventDrops!=reportedDrops){traceLocked(TraceKind::QueueFull,0,s_rotary.eventDrops-reportedDrops);reportedDrops=s_rotary.eventDrops;}
  if(have)traceLocked(TraceKind::Drain,0,out.delta,-s_rotary.partial,out.gesture,out.undo);
#endif
  portEXIT_CRITICAL(&s_rotaryMux);return have;
}

int takeRotaryDetents() {
  if (!s_rotaryIsrInstalled) return 0;
#if !(ROTARY_A_PIN >= 0 && ROTARY_B_PIN >= 0)
  // rev4: poll the expander once per service tick and decode in software.
  rotaryPollStep();
#endif
  int32_t v;
  portENTER_CRITICAL(&s_rotaryMux);
  v = s_rotary.take();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if(v)traceLocked(TraceKind::Drain,0,v,-s_rotary.partial);
#endif
  portEXIT_CRITICAL(&s_rotaryMux);
  // Normalize the board-level quadrature polarity before emitting Scroll
  // events. App/UI code treats positive delta as moving down/forward; the
  // rev4 direct-GPIO decoder reports the physical encoder in the opposite
  // sign; Decoder::take() normalizes it once for all consumers.
  return (int)v;
}

void resyncRotaryQuadrature() {
  uint8_t s = (uint8_t)((readRotaryA() ? 2 : 0) | (readRotaryB() ? 1 : 0));
  portENTER_CRITICAL(&s_rotaryMux);
  s_rotary.resync(s);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  traceLocked(TraceKind::Resync,0,s,s_rotary.resyncs);
#endif
  portEXIT_CRITICAL(&s_rotaryMux);
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
RotarySnapshot rotarySnapshot() {
  RotarySnapshot out{};
  portENTER_CRITICAL(&s_rotaryMux);
  out.forward=s_rotary.forward;out.reverse=s_rotary.reverse;
  out.invalid=s_rotary.invalid;out.resyncs=s_rotary.resyncs;
  out.scrollForward=s_rotary.scrollForward;out.scrollReverse=s_rotary.scrollReverse;
  out.state=s_rotary.state;out.partial=-s_rotary.partial;
  out.ready=s_rotaryIsrInstalled;out.anchored=s_rotary.anchored;
  portEXIT_CRITICAL(&s_rotaryMux);
  return out;
}
#endif

}  // namespace input
}  // namespace hal
