#if defined(ORIGAMI_DEV_SERIAL_INPUT) && ORIGAMI_DEV_SERIAL_INPUT && defined(MENU_APP)
#include "tooling/lua_runtime_probe.h"
#include "services/countdown.h"
#include "widgets/vm.h"
#include <Arduino.h>
#include <string.h>

namespace tooling {
void probeLuaRuntime(Print &output) {
    struct Case {
        const char *name;
        const char *source;
        bool mustFault;
    };
    static const Case cases[] = {
        {"snapshot",
         "function widget.tick() local s=kami.device.snapshot(); assert(type(s.app)=='string' and "
         "type(s.sfx_volume)=='number'); assert(type(s.timer.revision)=='string'); "
         "assert(kami.timer.get().state==s.timer.state); assert(#kami.capabilities.list()==6); "
         "assert(kami.capabilities.describe('timer.start').argument.minimum==5); "
         "assert(kami.capabilities.describe('navigation.home')==nil); assert(dofile==nil and "
         "loadfile==nil) end",
         false},
        {"loop", "function widget.tick() while true do end end", true},
        {"caught_budget",
         "function widget.tick() while true do pcall(function() while true do end end) end end",
         true},
        {"xpcall_budget",
         "function widget.tick() while true do xpcall(function() while true do end end,function() "
         "return '' end) end end",
         true},
        {"coroutine_budget",
         "function widget.tick() local c=coroutine.create(function() while true do end end); "
         "coroutine.resume(c) end",
         true},
        {"lookup_budget", "setmetatable(widget,{__index=function() while true do end end})", true}};
    unsigned passed = 0;
    for (const auto &test : cases) {
        widgets::LuaVm vm;
        bool loaded = vm.open(128u * 1024u) &&
                      vm.loadScript(test.source, strlen(test.source), "=runtime-probe");
        const uint32_t started = millis();
        bool returned = loaded && vm.callTick(0);
        bool correct = loaded && (test.mustFault ? (!returned && vm.faulted() &&
                                                    strstr(vm.error(), "instruction budget"))
                                                 : returned);
        const uint32_t elapsed = millis() - started;
        vm.close();
        output.printf("[lua-runtime] case=%s ok=%u elapsed_ms=%lu closed=%u\n", test.name, correct,
                      (unsigned long)elapsed, !vm.isOpen());
        if (correct)
            ++passed;
    }
    output.printf("[lua-runtime] passed=%u total=%u\n", passed,
                  (unsigned)(sizeof(cases) / sizeof(cases[0])));
}
void probeLuaTimer(Print &output) {
    using namespace services::countdown;
    const auto original = snapshot();
    // Only an idle timer can be borrowed. Keep every stored preference intact.
    if (original.state != TimerState::Idle || services::automation::wantsInputCapture()) {
        output.println("[lua-timer] skipped=timer_or_alert_active");
        return;
    }
    static const char source[] = R"(
      function widget.start()
        local timer,err=kami.timer.start({duration_seconds=30})
        assert(timer,err and err.code);assert(timer.state=='running')
      end
      function widget.input(event)
        local timer,err=kami.timer[event.type]()
        assert(timer,err and err.code)
      end
    )";
    widgets::LuaVm vm;
    bool started = false, paused = false, resumed = false, canceled = false;
    if (vm.open(128u * 1024u) && vm.loadScript(source, sizeof(source) - 1, "=timer-probe")) {
        started = vm.callStart() && snapshot().state == TimerState::Running;
        if (started)
            paused = vm.callInput("pause", 0) && snapshot().state == TimerState::Paused;
        if (paused)
            resumed = vm.callInput("resume", 0) && snapshot().state == TimerState::Running;
        if (resumed)
            canceled = vm.callInput("cancel", 0) && snapshot().state == TimerState::Idle;
    }
    vm.close();
    cancel();
    bool restored = save(original);
    output.printf("[lua-timer] start=%u pause=%u resume=%u cancel=%u restored=%u passed=%u\n",
                  started, paused, resumed, canceled, restored,
                  started && paused && resumed && canceled && restored);
}

} // namespace tooling
#endif
