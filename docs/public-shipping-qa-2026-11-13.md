# Public firmware acceptance — 2026-11-13

Branch: `fix/public-shipping-acceptance`. Latest `origin/main` fetched and merged locally. Subsequently published at user request through PR #27.

A checked item means the stated verification was completed, not that every possible interaction is proven. Host and device evidence are reported separately. Credentials and raw device logs remain in ignored `.origami/private/`.

## Integration and installation
- [x] Fetch latest public origin and merge without losing local shipping changes — clean merge.
- [x] Required architecture and tracked-secret checks. — PASS: architecture guard and Windows tracked-secret scan.
- [x] Broad available host regression suite; investigate every failure. — 176/178 checks pass. Native simulator unavailable; onboarding state checks pass but renderer unavailable. See exact results below.
- [x] Production build, image inspection, size and SHA-256. — PASS: production build; final candidate 5,796,848 bytes, below 6 MiB; SHA-256 `f92546bb31be46d7d78dec31cc5f4c7099ef57f164ea1ce3aa05c86d993a90a1`.
- [x] Identify attached hardware, running build and partition layout. — PASS: ESP32-S3 rev 0.2, embedded 8 MB PSRAM, 16 MB flash; first boot reports rev4/n16r8-ota-v1/v1.0.8.
- [x] Back up selected application and preserve NVS/OTA metadata. — Full selected-slot baseline backup, verified against flash; local recovery instructions saved.
- [x] Flash selected slot, verify digest and unchanged metadata. — ota_0 application-only update, digest matched, metadata byte-identical, one final reset and healthy boot.
- [x] Confirm boot, SD mount, health and stable idle. — PASS on merged baseline: serial version/status, mounted SD, repeated heartbeat; user confirmed onboarding screen.

## Network and App Store
- [x] Join supplied Wi-Fi; confirm address and connection. — PASS: connected and assigned LAN address; credentials omitted.
- [x] Delete currently connected saved network; confirm no crash and offline transition. — Reproduced under live telemetry, fixed network-owner quiescence, then verified actual Settings deletion with linked account: count 0, offline, BLE, responsive diagnostics.
- [x] Re-add deleted network and reconnect. — Plain-text USB provisioning uses production owner; connection restored and account retained.
- [x] Delete unconnected entry; retain/recover valid connection. — DEVICE: created an isolated test SSID, reconnected to the valid network, selected/deleted only the unconnected entry; valid connection and account retained.
- [x] Empty network list, re-entry, selection bounds and persistence. — DEVICE: empty list after four active-network deletions, re-entry and reconnect; final application update retains Wi-Fi/account. HOST: credential bounds and persistence failures.
- [x] QR provisioning cancellation and camera/radio cleanup. — HOST: QR admission/lifecycle/preview cleanup pass. DEVICE: user account QR setup and App Downloader camera-to-network handoff pass; cancellation is host-covered.
- [x] Sound provisioning input and lifecycle regressions. — HOST PASS: Wi-Fi onboarding input and GGWave lifecycle suites.
- [x] Portal deletion uses the same credential owner safely. — NOT APPLICABLE: captive portal is disabled by production policy. Legacy nonproduction path is outside this candidate.
- [x] Public App Store six-digit code resolution and native package download. — Actual App Downloader scanner/code/confirmation flow installed Pong; managed SD catalog confirms package.
- [x] Package signature/hash rejection and download cancellation/retry. — HOST: package hash/schema/incompatibility, interrupted publication, scanner cancellation and transport retry pass; no live corrupt-package install attempted.
- [x] Installed app launch, exit, re-entry and resource cleanup. — DEVICE: public Pong opened/exited twice, confirmed by current-app API; account emotion-browser rendered and navigated. The account desk-clock source issue is separately excluded at user direction.
- [x] Offline/transport failure recovery and TLS cleanup. — HOST: TLS cleanup/deadlines/backpressure pass. DEVICE: repeated offline/reconnect, camera/recorder handoffs and upload retry after an invalid receipt.

## UI, settings and onboarding
- [x] Navigation, Back, cover/resume and screen lifetime. — HOST PASS: navigation/presentation; device menu/Settings navigation observed. Broader physical controls remain separate.
- [x] Onboarding controls, tutorial progress and exit. — HOST: tutorial boot/progress/lifecycle and 1,000 onboarding state iterations pass. DEVICE: user saw onboarding and completed QR account setup. Full tutorial visual rendering remains a coverage gap because public renderer tools are absent.
- [x] Settings lifecycle, backend selection and persistence. — HOST PASS: settings lifecycle, backend contract and server modes.
- [x] Brightness, sound volume and haptics settings. — DEVICE: brightness 5/50/100 and restoration to saved setting pass; SFX volume change/restoration pass; user confirmed haptic output. HOST: brightness/vibration persistence contracts.
- [x] Physical display, buttons and rotary controls. — User confirmed normal display and reported the physical checks worked. Serial navigation also passed.
- [x] Clock sync, retained time and alarm settings. — HOST PASS: clock retention/sync and alarm suites. DEVICE: synchronized wall clock observed.
- [x] Alarm activation, dismissal and navigation handoff. — DEVICE: scheduled test alarm rings, PCM audio starts, A dismisses and input capture clears. HOST: alarm/recipe navigation and lifecycle.

## Media and shared resources
- [x] Camera initialization, preview, capture and cancellation. — DEVICE: OV3660 preview, VGA JPEG saved with fresh-frame evidence, exit restored network.
- [x] Photo storage, gallery, deletion and queue synchronization. — DEVICE: fresh JPEG saved, gallery decoded it, upload receipt and user-confirmed account delivery. HOST: coordinated deletion and queue mutation; personal images not deleted.
- [x] Video recording, teardown and container checks. — DEVICE: silent AVI, 214 frames, zero dropped frames, SD file and validated upload receipt. HOST: RIFF/index/recording failures and teardown.
- [x] Video playback, seeking, timing and audio handoff. — DEVICE: found/fixed stale-timestamp wrap; final build renders intermediate frames, pauses/resumes and reaches frame 214. HOST: clock wrap, late-frame skipping and audio timing. UI rotary changes clips; it does not seek within a clip.
- [x] Recorder capture, saved recording and upload enqueue. — DEVICE: record/pause/resume/save; WAV finalized at 819,244 bytes, queued and HTTP 200 uploaded. Audible quality remains separate.
- [x] Music/MP3 metadata, playback admission, stop and cleanup. — DEVICE: CRC-uploaded synthetic 44.1 kHz mono MP3 decoded 5,067 ms to EOF, stopped task/I2S/file cleanly, then the test file was deleted. HOST: metadata/admission/power tests.
- [x] Audio listening quality and microphone capture on hardware. — User confirmed alarm/speaker/haptic checks worked. Microphone probe captured 96,000 nonzero samples without read errors; waveform/subjective speech quality is not a calibrated measurement.
- [x] SD readers/writers, partial failure and mutation coordination. — DEVICE: SD diagnostic writes/reads/deletes temporary fixture; photo/WAV/AVI files saved and read. HOST: streamed I/O, cancellation and coordinated mutation failures.
- [x] Upload retries, receipts, manifests and clock admission. — HOST: all related regression checks pass. DEVICE: rejected missing image receipt then succeeded on retry; video interruption resumed and verified receipt; WAV HTTP 200.
- [x] Photo/video/recording upload success with configured account. — DEVICE: photo and WAV HTTP 200; silent and microphone-video receipts verified; all queued stress-test media drained. User confirmed photo arrival.
- [x] Agent entry, speech, cancellation and resource return. — DEVICE: authenticated WebSocket ready, microphone capture active, cancellation releases microphone and returns home. HOST: voice/delivery/lifecycle suites. Full spoken question/reply remains a manual coverage gap.
- [x] BLE initialization and file-transfer lifecycle. — HOST PASS: BLE lifecycle/notification/file callbacks. DEVICE: BLE starts after last network deletion; over-air transfer not yet tested.
- [x] Network/background worker cancellation and memory ownership. — HOST: lifecycle, 100,000 concurrent worker cycles and 8,000 coordination cycles; DEVICE: camera/recorder radio handoff and connected-network deletion.

## Power, OTA and shipping
- [x] Sleep timers, network shutdown and wake boundaries. — HOST: timers, network boundaries and power shutdown suites pass. Live sleep/wake is tracked below.
- [x] Battery display and USB power behavior. — DEVICE: fuel-gauge telemetry valid, approximately 4.19 V, raw SOC 86%; host-profile SOC differs by configuration. USB disconnects during sleep. This checks interfaces, not charge calibration or endurance.
- [x] Physical sleep/wake and alarm wake. — User confirmed physical wake and responsiveness after timed forced-battery sleep. DEVICE: accessory shutdown/timer wake and scheduled deep wake both report deep=1/timer=1, retain account, restore Wi-Fi. Forced light sleep while attached to USB left diagnostics silent until cable reconnection; normal USB sleep avoids CPU light sleep. This bench-test limitation is retained as a caveat.
- [x] OTA Public access, version selection and confirmation policy. — HOST PASS: Public access/default/version/manual-confirmation suites. DEVICE: Public, production-only, confirmation required.
- [x] OTA download resume, validation and boot-health regressions. — HOST: download policy/probe/resume and boot-health suites pass. DEVICE: final application digest/boot verified. No remote OTA activation replacing this candidate.
- [x] Manufacturing/factory handoff and Public shipping evidence. — HOST PASS: manufacturing, factory test and handoff suites. Destructive shipping cleanup not run on account-linked device.
- [x] Shipping cleanup keeps telemetry disabled until physical unlock. — HOST PASS: shipping and device handoff suites retain branch cleanup protection.
- [x] Final firmware health, source status and verification gaps recorded. — DEVICE: Wi-Fi connected, account retained, SD mounted/upload-ready, upload queue empty, Public OTA confirmation retained, timer/deep wake confirmed. Repeated healthy heartbeats; no crash signature in final capture. Source fixes submitted to public main through PR #27. Coverage limits below.

## Findings and evidence
1. The attached board was erased (confirmed by user and all-FF factory-region read). Installed the verified four-component factory recipe before application QA; no erase-all.
2. Reproduced the reported connected-network deletion crash under live account/telemetry load after an initial passing attempt. Backtrace: net_poll health upload -> TLS recv -> pbuf_free -> esp_wifi_internal_free_rx_buffer after Wi-Fi deinitialization. Fixed by acknowledged network quiescence held through manager completion. Host regression and post-fix device flow pass; three additional delete/reconnect cycles pass (four post-fix cycles total), with account retained.
3. Found and fixed USB plain-text Wi-Fi provisioning: it bypassed the production provisioning owner and could report success while Wi-Fi initialization failed after last-network deletion. Both text and JSON now use the deferred owner; credential buffers are cleared. Focused host regression and device reconnect after deleting the last network pass.
4. A public native package resolved and installed by code (11,640 bytes). Subsequent raw USB downloads demonstrated admission delays; actual App Downloader scan/code/confirm/install flow passed and Pong appears in the managed catalog.
5. Corrected public host harness paths, Windows CRLF fixture hashing, missing fixture declarations/includes and obsolete asynchronous widget fixtures. Two simulator/render tests require components excluded from the public repository; these remain explicitly unavailable.

6. Found live silent-video playback showing only first/final frames. Input dispatch can set the playback start after the UI timestamp was sampled; unsigned subtraction then wraps and requests the final frame. The synchronous renderer now samples a fresh clock, matching the asynchronous path. A new regression fails before the fix and passes afterward. Device verification: frame 62 at 1.97 s, pause held frame 82, final frame 214 with 86 decoded frames (bounded timing-based frame skipping remains intentional).
7. Video upload left only 92 bytes of stack headroom. Increased UploadQ from 10 to 12 KiB within its already reserved 12 KiB shared stack, so no additional static/internal heap reservation. Rebuilt/reflashed; live video upload passed with 2,192 bytes free (later audio uploads also exceeded 2 KiB).
8. An account-synced Lua widget raised `attempt to call a nil value (field 'millis')`. The error view contained it and Back recovered. Read back the account package and confirmed three unsupported `kami.sys.millis()` calls. A corrected local Lua source is prepared without modifying the account original; tested with the vendored Lua runtime. User explicitly deferred downloaded-app implementation fixes; the account original remains unchanged. Logical app A is the encoder click; its displayed A/B labels are misleading. Firmware containment/Back recovery passed; the original app is not marked passed.

## Physical observations
- User confirmed that pressing/releasing the power/encoder button wakes the sleeping device normally.
- User confirmed normal screen appearance and successful photo arrival in the linked account.
- Account was linked by the user through Wi-Fi QR setup.
- User replied that the alarm/speaker, vibration and physical-control checks worked. These are user observations; serial-injected inputs and PCM writes alone do not prove them.


## Additional camera findings
- Under speaker/camera/microphone churn, the SDK camera buffer contained two JPEGs: 17,048 bytes, first complete envelope ending at byte 7,833. A previous session rejected all 204 submitted frames. The HAL now exposes the first complete structural JPEG, retaining its DMA timestamp. Strict file/writer validation still rejects joined/trailing payloads. New host tests cover joined, truncated, malformed, borrowed and non-JPEG frames. DEVICE: normalized an actual 6,613-byte DMA buffer to its 2,514-byte first JPEG; three subsequent recordings saved with zero invalid/dropped frames.
- Leaving Camera while recording saved a valid 81-frame clip but left upload count zero. Stop and navigation teardown now share finalized-video enqueue, guarded by the existing pending flag. Tests cover success/failure and repeated teardown with exactly one enqueue. DEVICE: navigation exit saved a 114-frame microphone video and queued it; sound playback reached the end with 3,770 ms audio, then all uploads drained.

## Publication validation
GitHub CI caught a stale shutter fixture after the final camera helper change: the extracted teardown referenced `camQueueSavedVideo` without a test stub. Updated the fixture to assert the enqueue handoff and reran shutter and real teardown regression tests successfully. Firmware source and flashed image are unchanged by this fixture correction.

## Coverage limits
- Full spoken Agent question/reply and over-air BLE transfer were not exercised.
- Battery endurance, charge calibration and subjective microphone speech quality were not measured.
- Native simulator and full onboarding visual renderer are absent from the public repository.
- Remote OTA activation and destructive factory reset were host-covered; neither was performed on the account-linked candidate.
- Downloaded-app implementation defects are deferred at user direction; the desk-clock correction remains local and uninstalled.
- Forced CPU light sleep over USB required cable reconnection for diagnostics, although the user confirmed the screen woke and remained responsive.

## Exact host-check inventory
Generated from recorded process exit codes. Passing host checks exercise their stated source/fakes and do not replace physical tests. Evidence remains local in the ignored QA directory.

| Check | Result | Evidence |
| --- | --- | --- |
| `check_architecture_dependencies.py` | PASS | `final-check_architecture_dependencies.py.log` |
| `check_tracked_secrets.py` | PASS | `final-check_tracked_secrets.py.log` |
| `test_agent_audio_detach.py` | PASS | `results.json` |
| `test_agent_backend_parser.py` | PASS | `results.json` |
| `test_agent_completion_policy.py` | PASS | `results.json` |
| `test_agent_coordination.py` | PASS | `mediafix-test_agent_coordination.py.log` |
| `test_agent_delivery.py` | PASS | `results.json` |
| `test_agent_error_view.py` | PASS | `results.json` |
| `test_agent_exit_lifecycle.py` | PASS | `results.json` |
| `test_agent_heartbeat.py` | PASS | `final-results.json` |
| `test_agent_home_lifecycle.py` | PASS | `results.json` |
| `test_agent_home_repaint.py` | PASS | `results.json` |
| `test_agent_inbox_delivery.py` | PASS | `results.json` |
| `test_agent_latency.py` | PASS | `results.json` |
| `test_agent_mic_retry.py` | PASS | `results.json` |
| `test_agent_motion_clock.py` | PASS | `results.json` |
| `test_agent_presentation.py` | PASS | `results.json` |
| `test_agent_preview_chrome.py` | PASS | `results.json` |
| `test_agent_reconfigure.py` | PASS | `results.json` |
| `test_agent_recording_error.py` | PASS | `results.json` |
| `test_agent_reply_mailbox.py` | PASS | `results.json` |
| `test_agent_ring_status.py` | PASS | `results.json` |
| `test_agent_slab.py` | PASS | `results.json` |
| `test_agent_thinking_policy.py` | PASS | `results.json` |
| `test_agent_tls_deadline.py` | PASS | `results.json` |
| `test_agent_tls_io.py` | PASS | `results.json` |
| `test_agent_tls_timeout.py` | PASS | `results.json` |
| `test_agent_transport_backpressure.py` | PASS | `final-results.json` |
| `test_agent_transport_prepare.py` | PASS | `results.json` |
| `test_agent_voice.py` | PASS | `results.json` |
| `test_alarm_catalog.py` | PASS | `final-results.json` |
| `test_alarm_editor_layout.py` | PASS | `results.json` |
| `test_alarm_service.py` | PASS | `results.json` |
| `test_alarm_view_lifecycle.py` | PASS | `results.json` |
| `test_app_store_download.py` | PASS | `results.json` |
| `test_app_store_lifecycle.py` | PASS | `results.json` |
| `test_architecture_dependencies.py` | PASS | `results.json` |
| `test_automation_scheduler.py` | PASS | `results.json` |
| `test_avi_audio_playback.py` | PASS | `final-results.json` |
| `test_background_network.py` | PASS | `mediafix-test_background_network.py.log` |
| `test_background_restart.py` | PASS | `results.json` |
| `test_background_scheduling.py` | PASS | `results.json` |
| `test_ble_file_transfer.py` | PASS | `final-results.json` |
| `test_ble_initialization.py` | PASS | `results.json` |
| `test_ble_notification.py` | PASS | `final-results.json` |
| `test_board_time_retention.py` | PASS | `results.json` |
| `test_bounded_http.py` | PASS | `results.json` |
| `test_camera_dma_compat.py` | PASS | `results.json` |
| `test_camera_event_diagnostics.py` | PASS | `camerafix-test_camera_event_diagnostics.py.log` |
| `test_camera_frame_envelope.py` | PASS | `camerafix-test_camera_frame_envelope.py.log` |
| `test_camera_preview.py` | PASS | `results.json` |
| `test_camera_preview_decode.py` | PASS | `final-results.json` |
| `test_camera_radio_suspend.py` | PASS | `results.json` |
| `test_camera_session.py` | PASS | `results.json` |
| `test_camera_settings.py` | PASS | `results.json` |
| `test_camera_shutter.py` | PASS | `results.json` |
| `test_camera_snapshot.py` | PASS | `camerafix-test_camera_snapshot.py.log` |
| `test_camera_startup.py` | PASS | `results.json` |
| `test_camera_still.py` | PASS | `results.json` |
| `test_camera_trial.py` | PASS | `results.json` |
| `test_camera_video_resolution.py` | PASS | `results.json` |
| `test_camera_video_start.py` | PASS | `camerafix-test_camera_video_start.py.log` |
| `test_camera_video_teardown.py` | PASS | `camerafix-test_camera_video_teardown.py.log` |
| `test_camera_view_lifecycle.py` | PASS | `results.json` |
| `test_clock_read.py` | PASS | `results.json` |
| `test_clock_sync_start.py` | PASS | `results.json` |
| `test_custom_apps.py` | PASS | `final-results.json` |
| `test_custom_apps_settings.py` | PASS | `results.json` |
| `test_device_agent_text_admission.py` | PASS | `final-results.json` |
| `test_device_fs_json_write.py` | PASS | `results.json` |
| `test_device_fs_mutation.py` | PASS | `results.json` |
| `test_device_fs_upload.py` | PASS | `results.json` |
| `test_device_handoff.py` | PASS | `results.json` |
| `test_factory_test.py` | PASS | `results.json` |
| `test_firmware_update_session.py` | PASS | `results.json` |
| `test_ggwave_lifecycle.py` | PASS | `results.json` |
| `test_jpeg_envelope.py` | PASS | `camerafix-test_jpeg_envelope.py.log` |
| `test_lua_security.py` | PASS | `results.json` |
| `test_manufacturing.py` | PASS | `results.json` |
| `test_media_delete.py` | PASS | `results.json` |
| `test_media_gallery.py` | PASS | `results.json` |
| `test_mic_tuning_snapshot.py` | PASS | `results.json` |
| `test_microphone_capture.py` | PASS | `results.json` |
| `test_mp3_metadata.py` | PASS | `results.json` |
| `test_music_power.py` | PASS | `results.json` |
| `test_native_app_lifecycle.py` | PASS | `results.json` |
| `test_native_simulator.py` | UNAVAILABLE (excluded simulator) | `rerun-results.json` |
| `test_navigation_presentation.py` | PASS | `results.json` |
| `test_network_stack.py` | PASS | `mediafix-test_network_stack.py.log` |
| `test_network_worker_lifecycle.py` | PASS | `mediafix-test_network_worker_lifecycle.py.log` |
| `test_onboarding_controls.py` | UNAVAILABLE (renderer portion) | `rerun-results.json` |
| `test_onboarding_demo_speech.py` | PASS | `results.json` |
| `test_onboarding_release_config.py` | PASS | `results.json` |
| `test_ota_boot_health.py` | PASS | `rerun-results.json` |
| `test_ota_download_policy.py` | PASS | `results.json` |
| `test_ota_download_probe.py` | PASS | `results.json` |
| `test_ota_download_resume.py` | PASS | `results.json` |
| `test_ota_manual_confirmation.py` | PASS | `results.json` |
| `test_ota_public_access.py` | PASS | `results.json` |
| `test_ota_public_default.py` | PASS | `results.json` |
| `test_ota_versions.py` | PASS | `results.json` |
| `test_peripheral_reservation.py` | PASS | `results.json` |
| `test_photo_library.py` | PASS | `results.json` |
| `test_photo_storage.py` | PASS | `results.json` |
| `test_photo_upload_timestamp.py` | PASS | `results.json` |
| `test_photos_sync_policy.py` | PASS | `results.json` |
| `test_photos_video_upload.py` | PASS | `results.json` |
| `test_playback_start_admission.py` | PASS | `results.json` |
| `test_playback_status.py` | PASS | `results.json` |
| `test_pomodoro_settings.py` | PASS | `results.json` |
| `test_power_shutdown_boundary.py` | PASS | `results.json` |
| `test_preview_decoder.py` | PASS | `results.json` |
| `test_preview_decoder_pixels.py` | PASS | `final-results.json` |
| `test_public_distribution.py` | PASS | `results.json` |
| `test_qr_pomodoro_lifecycle.py` | PASS | `results.json` |
| `test_qr_preview_cleanup.py` | PASS | `final-results.json` |
| `test_qr_radio_admission.py` | PASS | `results.json` |
| `test_recipe_alerts.py` | PASS | `final-results.json` |
| `test_registration_priority.py` | PASS | `results.json` |
| `test_screen_brightness.py` | PASS | `results.json` |
| `test_sd_download.py` | PASS | `results.json` |
| `test_sd_stream_reader.py` | PASS | `results.json` |
| `test_sd_stream_writer.py` | PASS | `results.json` |
| `test_sd_upload_mutation.py` | PASS | `results.json` |
| `test_sd_write_compat.py` | PASS | `rerun-results.json` |
| `test_secret_transport_notification.py` | PASS | `results.json` |
| `test_security_boundaries.py` | PASS | `rerun-results.json` |
| `test_settings_backend_contract.py` | PASS | `results.json` |
| `test_settings_lifecycle.py` | PASS | `results.json` |
| `test_settings_server_modes.py` | PASS | `results.json` |
| `test_shipping.py` | PASS | `results.json` |
| `test_shipping_power.py` | PASS | `results.json` |
| `test_sleep_network.py` | PASS | `rerun-results.json` |
| `test_sleep_network_boundary.py` | PASS | `final-results.json` |
| `test_sleep_timer.py` | PASS | `results.json` |
| `test_telemetry_drain.py` | PASS | `results.json` |
| `test_telemetry_journal.py` | PASS | `final-results.json` |
| `test_telemetry_queue.py` | PASS | `final-results.json` |
| `test_tls_anchor_policy.py` | PASS | `results.json` |
| `test_tls_finished.py` | PASS | `rerun-results.json` |
| `test_tls_socket_cleanup.py` | PASS | `results.json` |
| `test_tutorial_boot_exit.py` | PASS | `results.json` |
| `test_tutorial_lifecycle.py` | PASS | `results.json` |
| `test_tutorial_progress.py` | PASS | `results.json` |
| `test_typewriter.py` | PASS | `rerun-results.json` |
| `test_upload_backend.py` | PASS | `final-results.json` |
| `test_upload_clock_admission.py` | PASS | `results.json` |
| `test_upload_counts.py` | PASS | `results.json` |
| `test_upload_file_mutation.py` | PASS | `results.json` |
| `test_upload_fixture.py` | PASS | `results.json` |
| `test_upload_fixture_serial.py` | PASS | `results.json` |
| `test_upload_manifest.py` | PASS | `final-results.json` |
| `test_upload_receipt.py` | PASS | `final-results.json` |
| `test_upload_retry.py` | PASS | `results.json` |
| `test_upload_suspension.py` | PASS | `results.json` |
| `test_usage_events.py` | PASS | `results.json` |
| `test_usage_receipt.py` | PASS | `results.json` |
| `test_vibration_contract.py` | PASS | `results.json` |
| `test_video_container.py` | PASS | `results.json` |
| `test_video_device_status.py` | PASS | `results.json` |
| `test_video_enqueue.py` | PASS | `camerafix-test_video_enqueue.py.log` |
| `test_video_frame_audit.py` | PASS | `results.json` |
| `test_video_library.py` | PASS | `results.json` |
| `test_video_playback_async.py` | PASS | `camerafix-test_video_playback_async.py.log` |
| `test_video_playback_timing.py` | PASS | `camerafix-test_video_playback_timing.py.log` |
| `test_video_recording.py` | PASS | `camerafix-test_video_recording.py.log` |
| `test_video_upload.py` | PASS | `camerafix-test_video_upload.py.log` |
| `test_video_upload_protocol.py` | PASS | `final-results.json` |
| `test_video_upload_queue.py` | PASS | `results.json` |
| `test_video_upload_state.py` | PASS | `final-results.json` |
| `test_video_upload_transfer.py` | PASS | `final-results.json` |
| `test_widget_launch.py` | PASS | `final-results.json` |
| `test_widget_lifecycle.py` | PASS | `final-results.json` |
| `test_widget_poll_coordination.py` | PASS | `final-results.json` |
| `test_wifi_onboarding_input.py` | PASS | `results.json` |
| `test_wifi_provision.py` | PASS | `mediafix-test_wifi_provision.py.log` |
| `test_wifi_qr_session.py` | PASS | `results.json` |
| `test_wpa3_sae_cleanup.py` | PASS | `final-results.json` |
