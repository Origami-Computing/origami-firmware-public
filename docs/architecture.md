# Architecture

The firmware runs on ESP32-S3 with Arduino and FreeRTOS. `src/main_menu.cpp` composes boot services, device bring-up, credential storage, telemetry, navigation, and the main UI loop. Public interfaces generally live under `include/`, with implementations under `src/`.

| Area | Responsibility |
| --- | --- |
| `apps` and `nav_handler` | User workflows, mounted app stack, input routing, activation, cover/resume, final exit |
| `scene` and `ui` | Screen callbacks, reusable geometry, drawing, framebuffer/direct-panel presentation |
| `services` | Camera/audio sessions, network work, OTA, persistence operations, cancellation and resource coordination |
| `core` | Models, parsers, DSP, storage helpers, policy and credential snapshots |
| `hal` | Bus ownership, devices, task registry and hardware policy |
| `control`, `tooling`, `widgets` | Shared device operations, serial adapters and Lua integration |
| `sfx` | Sound definitions and playback composition |

App activation differs from screen entry. Covering a view does not terminate its workflow; final pop performs screen exit before app exit. Services pair resource acquisition with cleanup. Cancellation acknowledgment must precede freeing buffers or returning shared peripherals.

The dependency guard prevents core, services and sound code from importing app/navigation/presentation interfaces, including through intermediate project headers. Core must not import services. This checks include direction, not runtime race freedom.

OTA has two distinct owners: `firmware_update::Session` reserves background and Agent resources, while `ota_client` owns retrieval, inactive-slot writes, validation and activation. Download authorization, firmware verification, and publishing identity remain separate. The public draft does not deploy or alter cloud infrastructure.

The N16R8 partition layout preserves NVS at `0x9000`, application slots at `0x10000` and `0x610000`, plus coredump and shipping storage. Only the matching partition layout is supported. Firmware embeds a public trust bundle and public verification material, not access keys.

This is an ownership-level inspection, not a comprehensive runtime/security audit. Embedded artwork, fonts, cloud endpoints and backend protocol implementation remain subject to publication approval.
