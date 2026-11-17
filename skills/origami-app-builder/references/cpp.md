# Native C++ app development

Read `docs/architecture.md`, `docs/building.md`, `CONTRIBUTING.md`, `include/app.h` and `include/scene/screen.h` in the target checkout. Inspect the nearest existing app before choosing an implementation; `src/apps/timer_app.cpp` demonstrates a service-backed, multi-screen app. It is a reference, not a template to copy wholesale.

## Native implementation path

1. Add the public descriptor declaration under `include/apps/<slug>_app.h` and implementation under `src/apps/<slug>_app.cpp`, with a distinctive namespace and stable app ID.
2. Define the required `scene::Screen` callbacks and complete screen catalog. Use an appropriate `SCENE_APP_CATALOG_*` macro from `include/app.h`; read the current signature rather than guessing positional fields.
3. Register a menu entry in `src/apps/menu_app.cpp` using its existing entry structure and a real existing icon or a properly added asset. Verify the app is reachable, not merely compiled.
4. Keep state/model work separate from rendering. Use `ui::scene` and existing UI helpers where suitable, the input abstraction for controls, and `NavHandler`/screen transitions for navigation. Preserve back/home behavior and system chrome.
5. Put asynchronous network, storage, audio and camera work in services. Reuse existing resource/session owners. Do not block UI callbacks, directly take over shared hardware, or assume a worker stopped because cancellation was requested.

## Lifecycle that matters

App activation is not screen entry. `onEnter` initializes an activation; `onResume` recovers when uncovered. `onCover` handles view-scoped release. Screen switching must not recreate app-owned work unnecessarily. Screen exit runs before app exit on final pop. Cancel work and wait for acknowledgment before freeing buffers or returning shared peripherals. The app's `onPreTick` is bounded UI-task work, not a background thread, and must not navigate.

Respect dependency direction: apps coordinate workflows; scene/UI render; services own operations; core owns reusable models/policy; HAL owns hardware. Core must not import services. Core/services/SFX must not import app/navigation/presentation interfaces, including transitively. Do not patch installed PlatformIO packages to make a build pass.

## Independently installed native apps

The firmware-integrated path above produces a custom OS image. An App Store package is a separate delivery path: inspect `include/services/native_apps.h`, `include/config/native_apps.h`, `src/services/native_apps.cpp` and the available packaging tools before promising an installable app. The current loader requires a publisher signature, matching hashes and ABI, and an ELF payload within its size limit. Do not remove authentication checks or assume a public publisher service is available.

## Verification and delivery

From the repository root:

```sh
python3 tools/check_architecture_dependencies.py
python3 tools/test_architecture_dependencies.py
python3 tools/check_tracked_secrets.py
pio run -e esp32-s3-menu-n16r8-production
```

Use the pinned toolchain in `docs/building.md`. Run focused behavioral checks appropriate to changed logic. Report the compiled environment, actual results and hardware checks still outstanding. Build success does not prove installation, rendering, audio, camera or recovery. For an authorized device install, follow `docs/installation.md`, identify the board and preserve state; never use erase-all as a routine development step.

Deliver the app source, navigation entry, behavior/controls and build instructions. Firmware images are custom builds, not signed official OTA updates. Release creation and publication follow the repository's separate review workflow.
