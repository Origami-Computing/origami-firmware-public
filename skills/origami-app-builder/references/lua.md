# Lua widget contract

Verified against this repository's `src/widgets/app.cpp`, `vm.cpp`, `api_*.cpp` and `include/ui/scene.h`. These are the authority for version-specific behavior.

## Files, lifecycle and input

On the SD card:

```text
/widgets/my-app/main.lua
/widgets/my-app/data.json   # optional JSON object, loaded before the script
```

Use a slug of at most 31 ASCII characters. The picker has room for 12 SD widgets. Scripts are limited to 32 KiB and data files to 8 KiB; stay comfortably below the limits. Use `kami.data` with defaults; there is no general Lua file API and in-memory changes do not persist across closing/reopening.

The host creates `widget = {}`. Define callbacks with dot syntax:

```lua
function widget.start() end
function widget.tick(now) end
function widget.input(event) end
function widget.draw(now) end
function widget.stop() end
```

All are optional. `now` is device uptime milliseconds, not wall-clock time. Lua integers are configured as 32-bit; avoid assuming desktop 64-bit arithmetic. For elapsed milliseconds across wrap, use a short-interval modulo calculation, e.g. `(now - previous) & 0xffffffff`, and clamp excessive elapsed time after interruptions.

The current runner forwards `event.type == "scroll"` with signed `event.delta`, and `event.type == "A"` for select. Do not invent touch, keyboard or other button callbacks. LB returns to the picker; LB+RB is host-reserved navigation. Scripts do not override those escapes. Widgets execute while active; a local countdown is not a background alarm.

## Rendering

`kami.W` and `kami.H` describe the content surface during draw. The current host uses width 280 and height 240 minus its status strip. Draw in content coordinates without adding a status-bar offset. Render the background every frame. Keep fewer than 48 scene nodes, with stable unique IDs. Drawing functions emit scene nodes only during `widget.draw`.

```lua
local bg = kami.draw.rgb(12, 14, 20)
kami.draw.rect("bg", 0, 0, kami.W, kami.H, bg)
kami.draw.label("title", 0, 12, kami.W, 28, "Hello", 0xffff, bg, 2, 1)
```

Core signatures (optional arguments in brackets):

- `kami.draw.rgb(r, g, b)` → RGB565 integer; keep channels within 0–255.
- `rect(id, x, y, w, h, color [, radius])`
- `label(id, x, y, w, h, text [, fg, bg, size, align, font])`; align 0 = top-left, 1 = top-center, 2 = middle-center; font must be 1, 2 or 4.
- `progress(id, x, y, w, h, value, maximum [, track, fill])`; use nonnegative bounded integers and a positive maximum.
- `circle(id, cx, cy, radius, stroke [, strokeWidth, fill, filled])`
- `line(id, x0, y0, x1, y1, color)`

All primitives above live under `kami.draw`. For bitmap/mask art, inspect `api_draw.cpp` for the exact packed byte layout and dimensions. `bitmap` takes a packed string, not an image filename or URL. Decode masks outside draw, reuse immutable buffers, and avoid one scene node per pixel.

## Device capabilities

- `kami.log(text)` / `print(...)`: diagnostic logging; never log credentials.
- `kami.sys.time()`: `{synced=false}` until time and timezone are ready; otherwise includes `hhmm`, `hour`, `min`, `sec`, `day`, `month`, `year`, `wday`, `epoch`.
- `kami.sound.play(name)`: names `chime`, `alarm`, `alert`, `success`, `error`, `tick`, `open`. True means recognized, not guaranteed audible: the audio bus may be busy. Trigger once on transitions, not every frame.
- `kami.device.snapshot()`: current `app`, `sfx_volume`, and `timer` fields. This does not expose arbitrary device sensors.
- `kami.capabilities.list()` / `describe(name)`: inspect supported control operations and argument constraints. This catalog is not a list of every drawing/system namespace.
- `kami.timer.get()`, `start({duration_seconds=...})`, `pause()`, `resume()`, `cancel()`: operate on the same timer used by the native app. Read current state first and start only on an intentional user action. Query `describe("timer.start")` for constraints rather than assuming an unlimited duration.

Timer calls return a timer table or `nil, error`. Timer fields are `state`, `duration_seconds`, `remaining_seconds`, `revision`. Revision is a decimal **string**. Mutations can accept `expected_revision`; preserve the string when coordinating with another view. An error can carry `may_have_applied=true`; re-read state before retrying. Do not retry side effects from draw or automatically cancel the user's timer in `widget.stop()`.

```lua
local current, err = kami.timer.get()
if current and current.state == "idle" then
  local started, failure = kami.timer.start({
    duration_seconds = 300, expected_revision = current.revision
  })
  -- Show failure.code when started is nil; re-read if it may have applied.
end
```

The snippet illustrates a transition, not code to run every tick. Additional domain-specific bindings such as transit must be inspected before use; they do not imply general HTTP access.

## Execution limits

The actual runner uses a 128 KiB Lua heap, 150,000 VM instructions per protected call, and callback deadlines (normally 100 ms; some initialization paths allow 250 ms). These are failure ceilings, not performance targets. Keep callbacks short and distribute work over ticks. Scene overflow faults the draw even if caught. Catching errors or using coroutines does not reset the budget.

The sandbox exposes base, table, string, math, utf8 and coroutine libraries. It omits `io`, `os`, `package` and `debug`; `dofile` and `loadfile` are removed. `load` accepts text only. Deliver self-contained source, not bytecode or a `require`-based module tree.

For desktop checks, use this vendored Lua version/configuration where feasible. Test scroll in both directions, repeated select, absent/invalid data, long labels and empty collections. Mocked APIs do not establish real device resource usage, button mapping or rendering fidelity.
