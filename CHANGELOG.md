# kamiOS release log

Each entry describes changes since the previous published release, reconstructed
from the public tagged source and merged changes. These notes do not claim new
hardware testing. [Release downloads](https://github.com/Origami-Computing/origami-firmware-public/releases).

The published history starts at v1.0.1. Versions v1.0.6 and v1.0.7 were qualification
builds, not published updates; their changes are included under v1.0.8.

## v1.0.14

Unreleased.

Changes since v1.0.12.

- Start automatic sleep’s inactivity interval when external power is removed, so time spent charging cannot cause immediate sleep after unplugging.
- Fix automatic Camera photo uploads so saved photos are queued for delivery without a manual upload action.
- Let the onboarding wheel lesson complete by turning C in either direction, with matching control illustrations and feedback.
- Add a hold-C sleep/wake lesson that saves its checkpoint before sleep and resumes after the normal wake greeting.
- Keep existing tutorial replay exit confirmation while adding the new control and sleep lessons.
- Show remaining transfers in a cloud badge, with directional dots that finish on a full upload or download arrow.
- Keep a red count on a closed cloud when Wi-Fi disconnects or transfers fail; clear the indicator when the queue is empty.
- Show waiting, sending and failed counts together in Upload Queue, with selected-file retry actions and clearer account errors.

## v1.0.13

Unreleased.

Changes since v1.0.12.

- Fix automatic Camera photo uploads so saved photos are queued for delivery without a manual upload action.
- Let the onboarding wheel lesson complete by turning C in either direction, with matching control illustrations and feedback.
- Add a hold-C sleep/wake lesson that saves its checkpoint before sleep and resumes after the normal wake greeting.
- Keep existing tutorial replay exit confirmation while adding the new control and sleep lessons.

## v1.0.12

Published: 2026-11-16 at 06:59 PM PST.

Changes since v1.0.11.

- Keep public firmware on the Public update channel, including devices with a saved private or staging preference.
- Combine still photos and video recording in the Camera viewfinder: tap B for a photo or hold B to start a clip.
- Add camera film looks and manual image controls, with settings restored after recording a clip.
- Improve full-resolution photo previews in Photos and wrap gallery navigation at the first and last item.
- Adjust display refresh timing during video playback to give full-frame transfers time to complete.

## v1.0.11

Published: 2026-11-16 at 03:09 PM PST.

Changes since v1.0.10.

- Add an A+B exit confirmation when replaying the tutorial, allowing users to leave the replay without completing every lesson.
- Offer Resume or Exit and pause tutorial timing while the exit confirmation is open.

## v1.0.10

Published: 2026-11-16 at 12:18 PM PST.

Changes since v1.0.9.

- Raise quiet microphone-recording playback with bounded gain while retaining the speaker volume limit.
- Retry motion-sensor initialization when entering a motion game if the sensor was missed during startup.

## v1.0.9

Published: 2026-11-13 at 05:07 PM PST.

Changes since v1.0.8.

- Show photo and video upload progress using transferred bytes, with sizes displayed in megabytes.
- Hide the gallery Upload action only when a saved receipt matches the current file and account, so changed or unconfirmed files can be uploaded again.
- Remove manual and automatic bulk app sync from Custom Apps; individual App Store installation remains available.

## v1.0.8

Published: 2026-11-13 at 03:03 PM PST.

Changes since v1.0.5.

- Rework onboarding and tutorial progress, add Wi-Fi setup by sound, and simplify setup instructions and practice screens.
- Add signed native App Store installs by QR or code, production stock apps and local multiplayer support; fix duplicate installs, cached app lists, installer timing and loading screens.
- Add motion-sensor calibration and a Haptics toggle; preserve time through deep sleep, support alarm wake and speed up return to Home.
- Automatically queue saved videos, including clips finalized when leaving Camera; retain failed uploads for retry and report queue/save failures instead of silently dropping work.
- Improve Camera, Bluetooth and network memory handling; bound TLS handshakes and drain active network sessions before forgetting a saved Wi-Fi network.
- Fix video start/resume skipping to the final frame and bound camera JPEG frames correctly when microphone and SD activity overlap.
- Keep cleaned shipping devices free of new telemetry until unlock and correct manufacturing acceptance of signed Public firmware.
- Remove unused demos, store-game sources, the brightness diagnostic and private station tooling from the public distribution; correct legacy Staging recovery manifest compatibility.

## v1.0.5

Published: 2026-10-31 at 08:56 PM PDT.

Changes since v1.0.4.

- Allow Public firmware updates without an OTA enrollment key, while keeping Private and Staging access restricted.
- Retain HTTPS, signed manifests, firmware hashes, hardware/layout checks and confirmation before installation.

## v1.0.4

Published: 2026-10-30 at 05:53 PM PDT.

Changes since v1.0.3.

- Add on-device video recording, including microphone audio, and local video playback.
- Include saved videos alongside still photos in Photos so clips can be browsed and opened from the gallery.

## v1.0.3

Published: 2026-10-30 at 05:16 PM PDT.

Changes since v1.0.2.

- Fix MP3 startup when a file contains large ID3 metadata, including embedded artwork.
- Calculate duration and seek positions from audio data rather than including the metadata block; handle seeks in very short tracks safely.
- Reject invalid or unreadable metadata cleanly instead of starting playback from an incorrect position.

## v1.0.2

Published: 2026-10-30 at 04:54 PM PDT.

Changes since v1.0.1.

- Add structured device-usage reporting with buffered, batched delivery and retry-safe persistence.
- Default upgraded devices to the Public update channel while retaining explicit developer channel selection.
- Publish the security reporting policy and vulnerability-reporting contact.

## v1.0.1

Published: 2026-10-27 at 09:18 PM PDT.

Initial published release.

- Establish the kamiOS 1.0 application release for rev-4 ESP32-S3 N16R8 devices, with corresponding source and dependency notices.
- Resume interrupted signed OTA downloads from the unwritten portion, validating the complete image before activation.
- Defer acceptance of newly installed firmware until the runtime boot-health check, instead of accepting it during Arduino startup.
- Harden TLS transcript-failure handling and retain signed update, hardware and partition-layout verification.
