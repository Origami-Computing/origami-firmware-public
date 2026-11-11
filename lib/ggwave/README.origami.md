# GGWave vendoring

Upstream: https://github.com/ggerganov/ggwave
Revision: `060aec73dd7123ccac200442f75bdc7369795ffe` (0.4.3).
MIT license retained in LICENSE. Sources include the upstream FFT and Reed-Solomon codec.
The laptop sender vendor/ggwave.js comes from the same revision and embeds WASM.

Local patch: on ESP32 allocate the codec arena explicitly in PSRAM and reject a
failed allocation before initializing arena pointers. RX waveform capacity uses
the enabled protocols' maximum receive duration (the same bound used by marker
detection), instead of reserving 2048 frames regardless of enabled protocols.
The minimum-bytes helper starts at the maximum, rather than 1, so disabling
one-byte protocols actually reduces that bound. With no protocols it returns 1.
Together these changes keep the audible-only arena below 3 MiB without reducing
the payload limit; default all-protocol configurations retain their minimum of 1.
