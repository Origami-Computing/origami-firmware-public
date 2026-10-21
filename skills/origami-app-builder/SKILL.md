---
name: origami-app-builder
description: Build and iterate on Origami device apps from natural-language ideas, using sandboxed Lua widgets first and native C++ firmware or narrow host bindings when the Lua API cannot support the requested behavior.
---

# Build something delightful on Origami

Turn the user's idea into a small, usable device app. Prefer a working Lua widget that they can copy to their SD card, try, and refine. Keep the user's idea central: choose sensible visual and interaction defaults, build the first useful version, and ask only for missing information that changes what can be built.

This skill belongs to the Origami firmware repository. Locate the checkout containing `platformio.ini`, `src/widgets/`, and `include/app.h`. Paths in the references are relative to that root. Inspect the checked-out implementation when APIs differ from these notes; do not invent device capabilities.

## Pick the smallest implementation that works

| Need | Approach |
| --- | --- |
| Small games, counters, interactive screens, clocks, data views | Lua widget |
| Background countdown using the existing device timer | Lua plus `kami.timer`, respecting the shared timer |
| Weather or other external data | Lua rendering with an externally prepared `data.json`; live fetching needs an existing supported host integration or native work |
| Missing device operation with an otherwise simple UI | Consider one bounded C++ binding backed by an existing service; keep the UI in Lua |
| Camera/audio pipelines, hardware ownership, persistent storage, substantial computation or unsupported input | Native C++ app/services |

Complexity alone is not a reason to switch languages. Explain the concrete missing API or resource constraint before choosing C++. A Lua widget cannot arbitrarily access the internet, SD files, GPIO, microphone or camera. Do not present the optional hosted Kami sync path as a public service everyone can use.

## Lua workflow

Read [the Lua contract](references/lua.md). Start from [the counter starter](assets/counter/main.lua), adapting it into the requested app rather than leaving a generic demo behind.

1. Choose a short lowercase slug and an interaction that works with rotary scroll and select. Leave back/escape to the host.
2. Produce `widgets/<slug>/main.lua` and, only if needed, `data.json` containing a JSON object. Keep generated output outside firmware source unless the user asks to contribute an example.
3. Keep state in locals, update it in lifecycle/input callbacks, and build a complete screen in `widget.draw(now)`. Use `kami.W` and `kami.H`, legible contrast and a concise control hint. Use stable node IDs and bounded collections.
4. Handle absent data, unsynced time, empty lists and service failures gracefully. Keep side effects out of drawing. Do not silently commandeer an existing device timer.
5. Check Lua syntax and exercise input/state behavior with available tooling. A desktop mock verifies logic only; it does not prove device rendering, resource budgets or hardware behavior. Inspect the final script against actual bindings and limits.
6. Deliver the app files, controls, SD destination, what was tested and one or two useful next refinements. Clearly distinguish authored, host-tested and device-tested results.

No firmware rebuild is needed for an SD widget. Explain how to copy the folder to the SD card and open **Lua Widget**. Before overwriting an existing widget, preserve its files. A flashing request is separate from creating an app; identify the device and preserve its state before any authorized install/recovery work. Do not open serial casually: it can reset the board.

## When Lua is not enough

Read [the native development guide](references/cpp.md). Prefer extending a narrow capability backed by a service over giving Lua unrestricted handles. For a native app, use the repository's App/Screen lifecycle and navigation patterns, and compile the actual target. Keep app behavior, reusable UI and service/hardware work in their respective layers.

Do not change firmware deployment, OTA signing, repository visibility or release publication as a side effect of building an app. Use the user's existing authorization for requested operations; do not add unrelated approval steps.

## Example requests this skill should handle

- “Make a cozy breathing animation I can speed up with the wheel.” → Lua, bounded animation state, clear start/pause control.
- “Turn this JSON into a tiny transit board.” → Lua with validated optional data and a useful empty state.
- “Make a focus timer that still rings after I leave.” → Inspect and use the shared timer service; explain existing timer interactions.
- “Record audio and show its waveform.” → Inspect native audio ownership and service APIs; use C++ where those capabilities are missing from Lua.
