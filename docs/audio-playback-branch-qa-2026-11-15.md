# Audio playback branch QA, November 15, 2026

Tested public v1.0.9 merged into the audio playback branch on the attached rev-4 N16R8. The 48 rows below correspond in order to the November 13 public shipping checklist. Results describe this candidate.

## Motion failure and repair

Before reboot the attached unit returned zero valid samples in 20 attempts, rejected raw streaming as unavailable and failed interrupt routing. Rebooting the previous build recovered the sensor. Its identity register returned 0x6A at address 0x6B, and 35 stationary samples averaged 983 mg. This establishes a real outage and recovery by reboot. The initiating electrical or software event was not captured.

A failed startup probe left the driver absent. Motion FIFO and calibration callers checked readiness before acquiring a gyro lease, so neither game could reinitialize the driver after the sensor returned. The driver could also publish its pointer before setup completed. The repair retries initialization at lease acquisition, publishes only after driver and accelerometer startup succeed, and loads saved calibration independently of boot-probe success. The diagnostic now acquires and releases the gyro it tests.

A regression injects probe loss and driver startup failure, restores the sensor without rebooting, and exercises game entry, ownership exclusion and release. The native simulator also exercised the production Tennis swing and Bowling throw detectors with controlled motion. On the installed image, three-axis raw samples, 20/20 diagnostic samples and the IMU interrupt assertion and clear passed. Tennis calibrated, Bowling opened with imu=1, and Tennis calibrated again. A live Tennis interval reported 103 gyro samples/sec. Camera, microphone and speaker use were each followed by another passing IMU read. A physical human swing and throw are separate acceptance checks.

## Original checklist

| # | Test | Result | Candidate evidence |
|---|---|---|---|
| 1 | Fetch latest public origin and merge without losing local shipping changes | Pass | Merged public v1.0.9 at d7d03f0a into audio-playback-fix at 85bed97b. The 48x checkpoint remains in its history. |
| 2 | Required architecture and tracked-secret checks | Pass | Architecture guard and Windows tracked-file secret scan. |
| 3 | Broad available host regression suite; investigate every failure | Pass | 180 public test scripts plus two guards. See host evidence below. |
| 4 | Production build, image inspection, size and SHA-256 | Pass | 5,796,592 bytes. Production image SHA-256 aa2c586667c612cee85ddffdf1b7b1f57b05bbabedeb7d03bb864bca9803b7fc. |
| 5 | Identify attached hardware, running build and partition layout | Pass | USB identity and selected partition checked against the attached rev-4 ESP32-S3. |
| 6 | Back up selected application and preserve NVS/OTA metadata | Pass | Read back and verified the full 6 MiB selected slot. Private recovery instructions saved. |
| 7 | Flash selected slot, verify digest and unchanged metadata | Pass | Wrote ota_0 only, verified its digest and compared the metadata byte for byte before reboot. |
| 8 | Confirm boot, SD mount, health and stable idle | Pass | Setup completed in 14,162 ms, SD mounted and repeated heartbeats arrived. |
| 9 | Join supplied Wi-Fi; confirm address and connection | Pass | Device joined the saved network and obtained a LAN address. |
| 10 | Delete currently connected saved network; confirm no crash and offline transition | Pass | Deleted the active network through Settings, reached zero saved networks and confirmed offline BLE startup. |
| 11 | Re-add deleted network and reconnect | Pass | Restored the saved network over USB. Account and connection checks passed. |
| 12 | Delete unconnected entry; retain/recover valid connection | Pass | Created and deleted an isolated non-active entry with the real network connected, then verified one network remained. |
| 13 | Empty network list, re-entry, selection bounds and persistence | Pass | Hardware empty-list and subsequent re-entry verified. Host tests cover selection bounds and persistence. |
| 14 | QR provisioning cancellation and camera/radio cleanup | Host + device | QR lifecycle and cancellation covered by host tests. Device App Downloader scanner opened, resolved a test code and returned resources. A physical account QR scan was outside this run. |
| 15 | Sound provisioning input and lifecycle regressions | Host pass | Sound onboarding and radio/input lifecycle tests. |
| 16 | Portal deletion uses the same credential owner safely | N/A | Production disables the captive portal. |
| 17 | Public App Store six-digit code resolution and native package download | Pass | Device resolved public code 100012 and installed Pong via the normal scanner confirmation. |
| 18 | Package signature/hash rejection and download cancellation/retry | Host pass | Package validation, interrupted publication, scanner cancellation and transport retry. |
| 19 | Installed app launch, exit, re-entry and resource cleanup | Pass | Pong launched twice with the current-app API confirming each launch after exiting and reopening the menu. |
| 20 | Offline/transport failure recovery and TLS cleanup | Host + device | TLS, ownership and transport failure tests passed. Wi-Fi deletion/reconnect and actual upload retries exercised on device. |
| 21 | Navigation, Back, cover/resume and screen lifetime | Host + device | Navigation, cover/resume and lifetime tests passed. Real device navigated Settings, Games and the App Downloader. |
| 22 | Onboarding controls, tutorial progress and exit | Host pass | 1,000 onboarding input/state sequences and 20 complete rendered sequences passed. The linked DUT retained its existing onboarding state. |
| 23 | Settings lifecycle, backend selection and persistence | Host pass | Settings lifecycle, backend selection and persistence. |
| 24 | Brightness, sound volume and haptics settings | Partial | Device brightness 5/50/100 and restoration passed. Haptics driver ran. Human confirmation of vibration is outstanding. |
| 25 | Physical display, buttons and rotary controls | Manual open | Display telemetry and injected buttons/rotation work. Physical button edges, rotary detents and visual display inspection require an operator. |
| 26 | Clock sync, retained time and alarm settings | Host + device | Clock sync/retention and alarm settings covered by host tests. Device scheduled and fired the test alarm. |
| 27 | Alarm activation, dismissal and navigation handoff | Pass | Observed alarm ringing, dismissed it with the A input and verified ringing cleared. |
| 28 | Camera initialization, preview, capture and cancellation | Host + device | Device OV3660 returned a JPEG. Host camera startup and cancellation passed. Fresh user camera capture and normal preview-to-save path await permission. |
| 29 | Photo storage, gallery, deletion and queue synchronization | Partial | Synthetic JPEG upload succeeded. Host coverage includes gallery/deletion and queue coordination. Fresh real photo through gallery and account delivery remain open. |
| 30 | Video recording, teardown and container checks | Partial | Firmware-format AVI generation, independent decode and teardown passed on host. Fresh camera-to-SD capture on this build remains open. |
| 31 | Video playback, seeking, timing and audio handoff | Pass | Device played a 214-frame clip. Paused twice at frame 39, resumed and finished frame 214 with error=0. Host timing/wrap/audio tests passed. |
| 32 | Recorder capture, saved recording and upload enqueue | Partial | Microphone sampled on-device. Host WAV tests and synthetic upload passed. Normal record/pause/resume/save with fresh audio remains open. |
| 33 | Music/MP3 metadata, playback admission, stop and cleanup | Host + device | Device MP3 decoder played 10,632 ms successfully. Host tests cover metadata, admission and release. |
| 34 | Audio listening quality and microphone capture on hardware | Partial | Microphone captured 96,000 samples with range −398 to +211 and zero read errors. Device speaker tone completed. New listening comparison and physical output confirmation remain open. |
| 35 | SD readers/writers, partial failure and mutation coordination | Pass | Hardware SD scratch write/read/delete and controlled media reads/writes passed. Host partial I/O and mutation tests passed. |
| 36 | Upload retries, receipts, manifests and clock admission | Host + device | Worker retry, receipt, manifest, clock and scheduling tests passed. See live synthetic uploads and retained historical item below. |
| 37 | Photo/video/recording upload success with configured account | Device fixtures | Synthetic still and WAV each returned HTTP 200. Video evidence is recorded below. Real capture and account-side visual confirmation remain open. |
| 38 | Agent entry, speech, cancellation and resource return | Host pass | Agent lifecycle, ownership and cancellation tests passed. A live spoken question and answer requires an operator. |
| 39 | BLE initialization and file-transfer lifecycle | Partial | BLE started on device when the final Wi-Fi credential was deleted. Host lifecycle and transfer callback tests passed. Independent over-air peer transfer remains open. |
| 40 | Network/background worker cancellation and memory ownership | Host + device | Worker coordination and cancellation tests passed. Device recovered networking across app and upload transitions. |
| 41 | Sleep timers, network shutdown and wake boundaries | Host pass | Sleep timers, radio shutdown and wake boundaries passed in host suites. Hardware wake results below. |
| 42 | Battery display and USB power behavior | Partial | Fuel gauge and charger inputs respond. Observed 4,199 mV and −5 mA while the gauge reported 0% SOC. Accuracy of percentage, temperature and battery endurance are unverified. |
| 43 | Physical sleep/wake and alarm wake | Device + manual | Hardware wake results below. Physical button or motion wake on battery requires an operator. |
| 44 | OTA Public access, version selection and confirmation policy | Pass | OTA reports Public, production-only and confirmation required. |
| 45 | OTA download resume, validation and boot-health regressions | Host + device | OTA resume and validation tests passed. Device application digest and boot health verified. No remote release activated. |
| 46 | Manufacturing/factory handoff and Public shipping evidence | Host pass | Manufacturing and handoff regressions. Destructive factory cleanup is outside this linked device run. |
| 47 | Shipping cleanup keeps telemetry disabled until physical unlock | Host pass | Factory/shipping handoff protection regressions passed. |
| 48 | Final firmware health, source status and verification gaps recorded | See disposition | Final branch and device health, known historical state and outstanding physical checks are recorded below. |

## Added sensor acceptance checklist

| Input | Evidence from this build | Remaining physical check |
|---|---|---|
| Accelerometer, gyro and FIFO | Real three-axis samples and repeatable calibration on the bench. Simulator recognized a Tennis swing and Bowling throw. | Rotate each axis, then register one intentional swing and throw on the device. |
| IMU data-ready interrupt, tap, step and motion wake | Data-ready asserted and cleared after the read. Previous routing restored. Boot reported pedometer enabled. | Confirm a real tap, step-count increase after walking, and motion wake in its supported power mode. |
| Microphone and camera | 96,000 microphone samples with zero errors. OV3660 delivered a JPEG on-device. | Listen to newly recorded speech and inspect a newly saved image from this candidate. |
| Fuel gauge, temperature, USB and charger inputs | BQ27220 present and initialized, 4,199 mV, −5 mA, 35.2 °C, PG=1, CHG=1. Gauge SOC was 0% while the UI showed 100%. | Verify unplug/replug transitions and battery-only behavior. SOC accuracy remains unresolved. |
| SD card and headphone jack detect | SD mounted, passed temporary file I/O and returned data from existing media. | Observe removal/reinsertion of the card and jack and verify remount and audio reroute. |
| Buttons, rotary, display and haptics | Input expander responded, software-driven navigation worked and haptics command ran. | Check A/B/C, both rotary directions, visible display output and felt vibration. |

## Reproducible evidence and open checks

Firmware commit fab2d196. All 182 host checks passed after the environment-specific reruns. The native-game suite used a recovered local test harness with the public production game controllers and simulated peripherals. The harness skips sending stop to a Lua process that has never started. All eight public games ran twice with release checks, and all six native-game tests passed. The public camera startup fixture now supplies a structurally valid JPEG.

The synthetic JPEG and WAV each returned HTTP 200. A generic 160×120 FFmpeg AVI triggered HTTP 422 and bounded retry. It was removed through the guarded deletion path. A 640×480 AVI created with the firmware's production MJPEG/PCM writer then returned HTTP 200 with a verified video receipt. The three successful fixtures used generated content.

The retained personal recording's historical queue item still reports “File missing.” On this candidate a separate SD read opened that exact path, reported 850,988 bytes and returned payload data. The queue ended at one historical item, zero pending, worker idle. The event that produced the original open failure is unknown. The historical item was left intact.

Accessory shutdown quiesced networking, powered down peripherals and armed the five-second timer (return code 0). The DUT then disappeared from Windows USB and did not answer at its last known LAN address. A timer-triggered boot was not observed. Scheduled alarm deep wake, post-wake health and over-air BLE transfer remain open pending physical recovery. The earlier on-device alarm firing/dismissal result is independent of deep sleep.

Private bench artifacts in this worktree are under .origami/private/qa-motion/. The host-final.json manifest maps all 182 checks to their final logs. The motion-flash-result.json file records the verified installation. The cold-check.log, candidate-functional.raw and extra-device.raw files contain real IMU, audio, camera and video observations. Network and store logs cover the Settings and Pong flows. The synthetic-media.raw, synthetic-video.raw and power-device.raw files contain transfer and shutdown evidence. Raw files stay local because some include device, network and media information.
