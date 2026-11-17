# Camera frame rates on COM55

Measured on November 16, 2026, on one Origami Rev 4 N16R8 with its installed SD card. The baseline was checkpoint `e6291810` with timing counters added. All COM55 benchmark runs used the same device, Sunny color, automatic exposure, 2048 x 1536 still capture, and 640 x 480 video with the microphone on. Each cycle included about 13 seconds of still preview and 14 seconds of recording. Lighting and SD write latency can change the results.

| Firmware trial | Still preview fps | Recording preview fps | New pictures/s in each saved AVI |
| --- | ---: | ---: | --- |
| Checkpoint | 3.34 | 4.87 | 23.31, 28.26 |
| Two still buffers and tile row copies | 3.84 | 3.77 | 3.58, 28.51 |
| Plus scaled video JPEG decoder | 3.82 | 7.10 | 25.76, 26.06 |
| Plus packed recording queue | 3.83 | 7.05 | 27.55, 26.96, 27.37, 27.46 |
| Plus internal still-decoder workspace (rejected) | 3.82 | 7.10 | 27.49, 27.30, 27.19, 21.16 |
| Packed queue + separate QVGA photo preview (installed) | 10.64 | 7.04 | 27.71, 27.49, 27.69, 27.54 |

## How to reproduce the measurements

1. Open Camera from the main menu. Select 480p and turn the microphone on. Keep the scene, lighting, card, and device position fixed between builds.
2. Collect the USB serial log. Allow 13 seconds in the photo viewfinder, hold B to start a video, record for 14 seconds, then tap B to stop. Repeat the cycle.
3. For each mode, sum the `frames` in the complete `[cine-rate]` windows and divide by the sum of their elapsed seconds. A counted image has finished decoding and been drawn to the viewfinder. Redrawing an overlay does not count. These counters measure application paint cadence. They do not instrument the display electrically. Mode changes and gaps over six seconds reset the window, so a pooled rate is a steady-state measurement.
4. For a microphone-on clip, subtract `repeats` from `frames` in `[cine-file]` and divide by `audio_samples / 48000`. The AVI timeline runs at 30 Hz and holds the preceding image when a capture misses a tick. Counting every timeline tick would overstate the captured motion.

The first trial stalled during one recording. Its worst preview window averaged 0.57 fps, and the file contained 3.58 new pictures/s. The scaled decoder cut recording-preview decode time from around 110 ms to around 54 ms. Packing variable-size JPEGs into the existing 768 KiB queue gave the SD writer room to absorb more burst frames with the same payload memory. Full-resolution still JPEGs and the recorded MJPEG and PCM formats are preserved. The 256 x 192 viewfinder uses a separate decode surface.

The packed-queue build improved the pooled still preview by **15%** and recording preview by **45%**. Saved video averaged **27.33 new pictures/s**, up **6%** from the pooled baseline of 25.79. The fastest baseline clip exceeded the fastest selected-build clip. With only two baseline runs and four final runs, the recording figures describe these clips and cannot establish a guaranteed gain. That build reached 22–32 queued frames and individual SD writes took up to 69–80 ms across the four clips. The subsequent internal-memory trial offered no additional still-preview gain and had a slower fourth saved clip.

## Public firmware comparison

COM46 reported official public v1.0.11 on the same N16R8 Rev 4 hardware. Its Camera opened a two-buffer, VGA (640 x 480) JPEG feed at sensor quality 6. Three 60-frame live-preview windows measured 10.51, 10.53 and 10.46 fps. The public timing counters reported zero repeated sensor timestamps, about 54.7 ms to decode and about 29.2 ms to present each frame, including an 18.1 ms panel transfer. COM46 was returned to Home after observation.

The original merged viewfinder decoded a 2048 x 1536 JPEG for each 256 x 192 preview. Even with eighth-scale output it still processed the compressed 3 MP source. The two devices used different timing instrumentation, cards and scenes. The public and merged results are a comparison of the observed paths, not a controlled comparison of two builds on the same hardware unit.

## Installed COM55 firmware

The photo viewfinder now asks the OV3660 for 320 x 240 JPEGs. Pressing B switches the sensor to 2048 x 1536 within the existing full-size DMA allocation, reapplies color and the selected shutter for the new sensor timing, waits for a fresh frame, verifies the coded JPEG dimensions, saves it, and restores the preview. Holding B still enters the configured VGA or HD video capture path. The lower-resolution source applies only to the live still preview. The viewfinder may render fine detail differently than the original downsampled 3 MP feed.

Over four 480p microphone-on cycles the installed build averaged **10.64 fps** in the photo viewfinder (3.19 times baseline), **7.04 fps** in the recording viewfinder (45% faster), and **27.61 distinct captures/s** in the saved files (7% faster by pooled average). The final photo-preview windows ranged from 10.49 to 10.70 fps, and recording-preview windows ranged from 6.80 to 7.32 fps. HD and slow manual shutter settings were outside the timing benchmark. A 100 ms exposure alone limits the sensor to at most ten new exposures per second.

In photo preview, capture plus decode took roughly 22–26 ms and drawing the image and controls took about 18–19 ms. A live display sample reported 41.46 ms for a full canvas flush, including 17.29 ms for the panel transfer. The flush rotates the canvas, synchronizes with the display and transfers it over SPI. On this build display presentation now costs more time than photo-preview decode. During recording, decoding the VGA preview still takes about 54 ms, and painting it takes about 26 ms. The camera and SD writer continue recording independently of the viewfinder.

The COM55 production app image SHA-256 is `a894c759a585e99bdd1e2a10938789b5c6093d8437308c8887eea1e8dbec12d9`. The app-only install verified the flash contents and preserved the partition, OTA and NVS metadata.

### Device checks

- From Camera, selected Gold, used the shutter wheel to reach 1/1001, and scrolled one more detent in the faster direction to reach Auto. Selected a manual 1/60 exposure, took a photo and held B to record. Both capture modes kept Gold. The still metadata reports 16,666 microseconds in manual mode, and the video sensor reported 16,668 microseconds on entry.
- Returned the shutter to Auto with the wheel and took a second photo. Its sidecar reports automatic exposure and a live 37,567-microsecond integration. Both gallery entries rendered. The manual and automatic JPEGs were saved separately.
- Recorded 480p with the microphone on and 720p with the microphone off, then restored 480p and microphone on. Opened both clips from Photos. The player reached 127/127 and 109/109 respectively with no decoder error. The player skips presentations to maintain real time. These counters are playback end positions.
- The standalone Audio Recorder produced a CRC-verified, 48 kHz mono WAV containing 151,040 samples. Both production and development PlatformIO builds passed. Host checks exercised dropped frames, queue wrap, exact PCM samples, JPEG ownership, shutter and ISP register changes, EXIF and the UI.

### Saved-frame counts behind the averages

| Run | AVI ticks | Repeated pictures | PCM samples at 48 kHz | New pictures/s |
| --- | ---: | ---: | ---: | ---: |
| baseline 1 | 436 | 98 | 696064 | 23.31 |
| baseline 2 | 437 | 26 | 698112 | 28.26 |
| qvga 1 | 436 | 34 | 696320 | 27.71 |
| qvga 2 | 433 | 37 | 691456 | 27.49 |
| qvga 3 | 438 | 34 | 700416 | 27.69 |
| qvga 4 | 431 | 36 | 688384 | 27.54 |
