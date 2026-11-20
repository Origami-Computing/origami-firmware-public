# Application-only USB deployment and recovery

Build using the pinned target in `docs/building.md`. For an existing supported device, use this procedure after a successful build or with a verified known-good recovery application.

## Find the device and prepare

Enumerate serial devices with the project Python (`python -m serial.tools.list_ports -v`). Honor the user's specified serial port or device path and bind it to its USB serial number. If no matching device appears, ask the user to connect it with a data-capable USB cable and retry. If several candidates remain, ask which device to use. Rediscover its port by USB serial before writing; release competing serial monitors.

Supported hardware: rev-4 ESP32-S3 N16R8, 16 MB flash, 8 MB octal PSRAM. Supported layout: `n16r8-ota-v1`:

| Region | Offset | Size |
| --- | --- | --- |
| Partition table | `0x8000` | `0x1000` |
| NVS | `0x9000` | `0x5000` |
| OTA selection metadata | `0xe000` | `0x2000` |
| ota_0 | `0x10000` | `0x600000` |
| ota_1 | `0x610000` | `0x600000` |
| Optional shipping data | `0xc20000` | `0x3000` |

Read hardware/layout with `ota status` at 115200 baud when establishing device compatibility; avoid an extra preflash serial session when adequate identity-bound evidence is already available. Capture partition/OTA metadata with `read_flash 0x8000 0x8000 <metadata.bin>` inside the grouped bootloader session below. Serial opening/readout can reboot the device. Compare the partition table and OTA selection against a previously verified record for this USB identity. If changed or first seen, decode the table and OTA selection using the matching ESP-IDF format/tools in the installed framework, including selection-record CRC and state. Derive the selected slot; do not infer it from the serial port. Both standard slots have the same size, so image-size validation is a host check.

Load any saved verification record for the connected device's USB identity from the current project. Treat saved ports and slot selections as historical observations; verify them before writing.

Store recovery files in a private directory under the project, such as `.origami/private/<device-id>/`, using a filesystem-safe identifier. Resolve this location from the project record or create it when preparing the first deployment.

Record backup paths, SHA-256 hashes, device identity, verified layout, rollback firmware identity when known, and exact restore commands in the project record. Create restore instructions appropriate to the host OS. Before reuse, verify the backup and its applicability to the connected device. Keep recovery files and machine-specific records local/private and exclude them from version control and public skill packages.

## One approval, then install and verify

Prepare the successful build and identify the device first. Ask once, unless the user has already explicitly authorized this deployment:

> Flash this build to <port / USB serial>, verify it, reboot and test the app? This includes reading device metadata, creating/reusing a recovery backup, and restoring the previous application if this build fails to boot.

This approval covers the entire procedure below. Perform missing mechanical steps yourself; do not ask the user for a recovery package for a healthy supported device. Approval does not resolve contradictory hardware facts: report a specific mismatch if found.

Resolve esptool from the packages selected by the current project's PlatformIO configuration, verify its version, and inspect its reset controls and supported API before using them. Prepare the host script and its dependencies before accessing the device.

### Group operations to avoid repeated restarts

Prefer one transition into the bootloader and one final reset into the application. Keep the same serial connection and flashing helper alive across the operations. Do not run a chain of independent esptool commands with default reset behavior: each can reboot the device. The RAM helper is temporary flashing code, not an application firmware installation.

After approval, perform this sequence:

1. Confirm USB identity and the candidate image hash and size, then enter the bootloader once.
2. Read partition/OTA metadata and confirm the selected slot. Stop before writing on a hardware, layout, or identity mismatch.
3. Reuse the intended verified recovery backup, or read and verify a fresh backup as described below.
4. Write only the selected application slot and require successful digest verification.
5. Read and compare preserved metadata while still in the flashing helper, before the application can change it during normal startup.
6. Issue one final reset into the application. Keep the serial handle open, switch to 115200 baud, and capture startup and a permitted status response in a bounded monitoring session. Confirm the same USB identity; if USB re-enumeration requires reopening the port, configure reset lines before opening and record any unavoidable additional reset.

For esptool 4.9.0, the tested approach creates one `ESP32S3ROM` connection, calls `connect()` and `run_stub()` once, then passes that returned helper object to successive `esptool.main(..., esp=helper)` calls. Use `--after no_reset_stub` between operations. With that already-running helper object, pass `--no-stub` to prevent `main()` from attempting to upload the helper again; this does not replace the existing helper object with a ROM connection. Load the managed package's bundled dependencies as its launcher does. After verification, reset once through the helper and monitor its existing serial handle. This API pattern is version-specific: inspect the installed implementation rather than assuming other versions expose the same behavior. Do not edit package caches.

The operation arguments below belong inside that session; they are not separate default-reset command invocations. Substitute actual paths and the verified slot offset, and use the installed version's syntax:

```text
read_flash 0x8000 0x8000 <metadata-before.bin>
read_flash <slot-offset> 0x600000 <previous-app.bin>  # only when a fresh backup is needed
verify_flash <slot-offset> <previous-app.bin>       # verify a fresh backup before writing
write_flash <slot-offset> <firmware.bin>
verify_flash <slot-offset> <firmware.bin>
read_flash 0x8000 0x8000 <metadata-after.bin>
```

If a grouped session fails, identify the failed step and whether a write began before retrying. Do not repeat a flash merely to repair logging or serial-monitor problems. After an interrupted write, do not assume the application is bootable; use the verified recovery procedure if needed and report the outcome. If the host/driver cannot preserve the session, use the required reset behavior and explain the additional resets rather than skipping verification.

Reuse a verified backup when it represents the intended rollback version for this device and layout. If the current application differs and should be restored on failure, capture and verify a fresh backup. For a new backup, check its size, hash and flash digest, and save the restore command: the same `write_flash` and `verify_flash` commands with `<previous-app.bin>`. Preserve partition table, NVS, OTA metadata, shipping data and SD files. Write only the application; never erase-all, write it at zero, or invoke an unexamined generic upload command.

After the grouped write, digest verification, metadata comparison, and final reset, confirm boot and app launch through available repository serial/runtime interfaces. Do not reset again merely to start monitoring. If production firmware restricts remote app launch, stop retrying the restricted commands; reconnecting or rebooting does not grant service access. Verify boot through permitted diagnostics and ask the user to open the app and test its controls. Do not change the production access policy just to complete this check. Record app launch and physical behavior as unverified until observed or reported; distinguish user reports from tool observations. If the app fails to boot, restore the verified backup once and verify its boot; report the failure instead of repeatedly flashing. Do not claim hash verification alone proves successful boot or physical behavior.

Report the source location, device, flash/boot results, menu path, and any physical interaction the user still needs to test. Save artifact hash, USB identity, selected slot, recovery paths, reset/session behavior, and verification evidence in `.origami/project.json` for the next iteration, preserving unrelated fields. Create the record if absent; keep device identities and local paths in that private record rather than in the skill.
