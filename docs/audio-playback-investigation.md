# Recorded-audio playback investigation

Status: 48x checkpoint installed and boot-verified. The user reports a substantial
improvement over the initial firmware and requested this checkpoint, while asking
for further loudness and clarity work because speech can still sound muffled.
This is a verified improvement checkpoint, not final subjective acceptance.

## Baseline and observations

Started from public `origin/main` commit
`0972384d` on branch `audio-playback-fix` (13 November 2026).
The connected rev-4 N16R8 device reported custom v1.0.8, Public channel.
USB diagnostics reported media UI volume 100, effective speaker route, and
20.00% final PCM gain. Capture settings were 1.00x, HPF off, limiter off.
Thus a low saved volume setting does not explain this observation.

The user-designated conversational voice recording was downloaded with the
existing size/CRC-checked USB protocol. The original remains untouched on SD;
recordings, logs and device identifiers are private under `.origami/`.
It contains 399,360 mono PCM16 samples at 48 kHz (8.32 seconds).
Its whole-file RMS is -42.95 dBFS. The median 100 ms RMS is -46.02 dBFS;
the 90th percentile is -39.07 dBFS. The peak is -5.02 dBFS at 8.184 seconds.
The latter is a brief transient, not representative of sustained speech.
Its physical cause has not been established. Peak normalization alone could
add at most about 5 dB, far short of the available speech headroom.

Source inspection: `mic_tuning::Processor` converts left-aligned microphone
samples by shifting 16 bits and applies the configured capture controls.
`WavPlayer` previously applied only the route-aware media volume to recordings.
Music and recordings share the volume profile. Video's audio also uses
`WavPlayer`. Absence of loudness processing plus this low-amplitude source is
a supported explanation; microphone hardware sensitivity/calibration has not
yet been independently ruled out as a contributor to the capture level.

## Candidate

`RecordingPlaybackDsp` adds a maximum 48x (33.62 dB) preamp with a shared peak
envelope, instantaneous peak protection and 150 ms envelope decay. It retains
state across read buffers, resets on track start/restart, and adds no buffering,
file scan or audiovisual delay. The processor is enabled for mono files under
`/recordings/` and `/videos/`; generated PCM and music decoding are unchanged.

Processing happens **before** the existing media-volume multiplier. The speaker
ceiling remains 2,000 basis points (20%). Candidate PCM peaks are bounded to
27,852, so at maximum speaker volume the resulting sample magnitude is at most
5,570, below the existing full-scale-times-20% limit. Codec gain is unchanged.

Running the initial 24x candidate C++ processor on the private original yields
-16.04 dBFS RMS, an increase of 26.91 dB, with peaks bounded to 27,852.
This is digital evidence, not an acoustic loudness measurement or intelligibility
judgment. A separate processed copy is used for same-firmware speaker A/B tests.
The original is never overwritten. The processor also amplifies recorded noise;
it cannot recover speech masked by noise or repair already-clipped captures.

## Measurement protocol and exclusions

Use the EarPods inline microphone, approximately 10 cm from the device speaker,
with fixed placement and unchanged host input settings. Capture at 48 kHz PCM16.
Record approximately six seconds before playback and a post-playback interval.
Verify each selected screen, playback-start path and end/stop event in USB logs.
Compare the same source and existing music at unchanged device volume.

Measure pre/post noise separately, inspect short-window variability and transients,
and reject unstable/noise-dominated runs. Subtract background **power**, not dB,
only for stationary uncorrelated noise with a sufficiently clear playback signal.
Host noise suppression/AGC can invalidate that model; nearly gated-to-zero noise
has appeared in exploratory captures, so report that limitation rather than
claim calibrated SPL. Repeated runs and source-correlated activity are needed.
User listening confirmation is separate from numeric level acceptance.

After observing near-zero windows in DirectShow captures, an additional Windows
WASAPI capture helper requested per-stream RAW mode through
[`IAudioClient2::SetClientProperties`](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient2-setclientproperties)
and `AudioClientProperties.Options`. It changes no system-wide settings. The
10-second probe returned 479,520 frames at 48 kHz, zero silent-flagged frames,
and one startup discontinuity. Its one-second noise levels ranged approximately
-75 to -69 dBFS rather than the earlier gated near-zero floor. Final comparisons
use RAW for both speech and music; do not numerically mix capture modes.

Exploratory runs before the user confirmed microphone placement are excluded from
final acoustic acceptance. Runs without a verified playback start are invalid.
One fixed navigation sequence inadvertently entered the firmware updater and
started downloading. It was canceled; the device reported `download cancelled`
at 1,373,972 bytes. No successful update or activation was reported. Subsequent
navigation checks scene labels instead of assuming fixed menu indices.

## Acoustic observations

These are **raw EarPods capture levels**, not calibrated SPL or noise-corrected
speaker levels. Playback windows exclude navigation sounds and recording ends.
Original/processed voice uses seconds 7–12 of the host capture; music uses 7–15.

| Trial | Playback RMS dBFS | Above louder pre/post noise, dB |
| --- | ---: | ---: |
| Original, previous firmware | -80.61 | 4.49 |
| Processed copy, previous firmware, run 1 | -53.98 | 4.49 |
| Processed copy, previous firmware, run 2 | -54.01 | 11.82 |
| Music reference, previous firmware | -55.17 | 2.54 |
| Untouched original, installed candidate | -53.32 | 14.88 |

The two processed-copy playback windows agree within 0.04 dB. The installed
candidate's original-file playback is within 0.7 dB of those copies. The observed
original-to-processed difference (~26.6 dB) is consistent with the independently
computed digital gain (26.91 dB). The user reported the processed speech was
"clearly intelligible." These independent observations support the intervention.

However, background drift, transients and near-zero microphone windows cause
the strict linear-noise-subtraction checks to reject these captures. In particular,
the weak baseline cannot support a precise acoustic gain estimate. Do not report
the raw differences as an exact SPL improvement or a noise-free music match.
`tools/analyze_recording_playback.py` retains the rejection reasons and declines
to produce a corrected value. Repeated active levels and listening confirmation
are reported separately from noise-subtraction acceptance.

A fresh capture of a quiet laptop speech stimulus played at -67.57 dBFS at the
EarPods mic, 16.70 dB above measured background. The user said the laptop source
was faint but clear; this is not an equivalent conversational-level input.
The second local diagnostic laptop trial measured RMS 34 PCM counts, peak 414
(-37.97 dBFS), without capture over-range samples. It reinforces the limitation:
bounded amplification cannot make arbitrarily faint or noise-masked inputs equal
to mastered music. The final fresh trial uses the user's normal speaking voice.

The first fresh test used the normal recorder, which queued its saved clip for
the existing account-upload workflow. Automatic approval review blocked a second
such test. Subsequent trials use the inspected `mic record` diagnostic, which
saves locally without enqueueing uploads; separately named SDPUT copies likewise
do not enqueue uploads. Original recordings are preserved.

## Fresh conversational trial and iteration

The user then spoke a new sentence into the device using the local diagnostic
recorder. The CRC-verified file contains 722,432 mono PCM16 samples at 48 kHz
(15.051 seconds), peak 1,271 and no full-scale samples. Speech at seconds 8–13
measures -50.85 dBFS; recorded background at 2–6 measures -69.05 dBFS.
The playback copy is byte-identical and is never enqueued for upload.

With 24x maximum boost, fresh speech measured -59.81 dBFS after valid background
power subtraction in RAW mode. Background drift was 0.32 dB and playback exceeded
background by 17.54 dB. The matching music reference measured -54.99 dBFS, with
2.58 dB background drift and a 19.66 dB margin. Both passed the predeclared noise
checks. Thus this fresh conversational take remained about 4.82 dB below music.
A repeat speech window agreed within 0.01 dB, but its noisier pre-roll failed
the background checks and was excluded from corrected comparisons.

The user judged this fresh playback better but still needing more volume. This
is a failed first-iteration loudness acceptance, despite intelligibility of the
earlier clip. Increasing the bounded preamp to 48x predicts 5.68 dB more speech
level on the fresh source, closing the measured gap. The original clip gains
only about 2.7 dB because the unchanged peak envelope limits stronger material.
The peak and final speaker ceilings are unchanged. The revised safety and
WAV/AVI tests passed. The second iteration booted, its flash digest verified,
and metadata remained unchanged; the first iteration was separately backed up.

Two RAW playback windows of the same fresh take measured -53.97 and -54.06 dBFS,
about 5.7 dB above the first iteration, matching the prediction. Both had noisy
pre-rolls and failed strict noise-drift acceptance, so neither is reported as a
noise-corrected level. Each captured stream had only its initial discontinuity
(frame zero), with no subsequent discontinuities or silent-flagged frames.
The user requested saving this version as a checkpoint: much better than the
starting point, but still potentially louder and sharper, with some muffling.
Further clarity tuning must be compared against this checkpoint, not silently
folded into its evidence.

## Build, installation and regression evidence

- Initial production build passed. Image: 5,798,976 bytes, below 6,291,456-byte slot limit.
  SHA-256: `b69017332e6b27d844f2c243b3a17b0a62d75d8ea29c4021e9d25cfd285090a1`.
  Pinned esptool 4.9.0 image checksum and validation hash passed.
- Revised 48x production build passed: 5,798,960 bytes. SHA-256:
  `cc7895a594c4e87cbda861677da657d8aeea0538aec2e62b23daf06985c9cbe2`.
  Pinned esptool image checksum and validation hash passed again.
- After explicit installation approval, verified USB identity, 16 MB flash,
  8 MB PSRAM, partition-table checksum and OTA-selection CRC/state. Read and
  digest-verified a fresh 6 MiB backup of the selected application. Wrote only
  that application slot and verified its digest. Metadata before/after was
  identical. One final reset; setup and OTA status were observed on the same
  serial handle. Private recovery commands and hashes are in `.origami/`.
- Sanitizer-backed production DSP tests passed: gain, silence, full-scale bounds,
  20% final scaling/mute, stereo linking, transient recovery, reset and chunk
  invariance at 8/16/44.1/48 kHz.
- Production WAV/AVI playback tests passed, including enabled-processor parity,
  exact durations, bounded interleave scanning, reopen and malformed input.
- Architecture dependency check, seven architecture tests, and tracked-secret
  check passed. Four synthetic measurement tests verify stationary-noise power
  subtraction and rejection of drift, gating and noise-dominated playback.

Video audio shares the tested processor, but a fresh physical video capture and
listening test has not been performed. Microphone sensitivity/calibration remains
outside the demonstrated playback fix; this investigation does not claim every
possible low-input cause is ruled out.
