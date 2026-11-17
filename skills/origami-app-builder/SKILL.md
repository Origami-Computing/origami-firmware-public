---
name: origami-app-builder
description: Build and iterate on native C++ apps for Origami devices, using the existing firmware app, screen, UI and service contracts. Use for app ideas, controls and device integrations; deployment is a separate operation.
---

# Build native C++ apps for Origami

Turn the user's idea into a usable C++ device app. The current public app-building path is C++ only. Choose controls and layout that suit the display, rotary input and buttons; ask only for information that changes the implementation.

Locate the firmware checkout containing `platformio.ini`, `include/app.h` and `src/apps/`. Read [the C++ development guide](references/cpp.md), then inspect the closest existing app and the actual APIs before editing. Repository paths in that guide are relative to the checkout root.

## Choose the delivery path

| User needs | Approach |
| --- | --- |
| A new app developed in this repository | Implement a C++ App/Screen, register navigation, and build custom firmware. |
| An independently installed App Store app | Inspect the native ABI and supported packaging/signing tools first. A compiled C++ file is not an installable signed package. Do not invent publisher access or bypass verification. |
| Camera, audio, networking or persistent data | Use existing services; keep workflow decisions in the app and hardware ownership below it. |

## Workflow

1. Define the smallest useful behavior, controls and failure/empty states.
2. Implement against existing App/Screen lifecycle, UI primitives and input contracts. Keep drawing bounded and free of hardware/network operations.
3. Reuse service sessions for operations, with explicit cancellation and cleanup on cover, exit and failure where required by the owner.
4. Verify navigation, input and state changes with focused checks. Build the actual firmware target when changing firmware source; distinguish host checks from physical-device results.
5. Deliver source files, how to reach the app, controls, build instructions and actual verification results. State packaging or hardware steps still outstanding.

Do not publish releases, change signing/deployment configuration or flash a device merely because an app was created. For an authorized installation, identify the board and preserve settings, partition layout and SD files using the installation guide.

## Examples

- “Make a breathing animation I can speed up with the wheel.” → C++ screen with bounded animation state and start/pause controls.
- “Make a focus timer that still rings after I leave.” → C++ app using the existing timer owner and its lifecycle contract.
- “Record audio and show its waveform.” → C++ workflow using the capture session and existing rendering primitives.
