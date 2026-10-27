# Device analytics v1

## Contract

`POST /api/device/events` uses the existing per-device bearer middleware. Identity
comes from authentication, never the JSON body. The v1 body contains `v`, `boot`
(128-bit boot nonce as 32 lowercase hex characters), `seq` (uint32), `uptime_ms`
(monotonic), `ts` (epoch seconds, or zero before sync), `app`, `event`, and optional
`duration_ms`. Unknown payload fields are discarded. No transcript, photo, audio,
file path, URL, or arbitrary properties are accepted.

The endpoint also accepts `{batch, events}`: a 16-character hexadecimal request
nonce and one to four validated v1 events. All rows are inserted in one database
statement. The receipt is `{ok:true,batch,count}` only after commit. Legacy
single-event requests remain supported. Firmware bounds the complete request to
800 bytes, groups usage events across interleaved logs in its current snapshot,
and requires an exact bounded receipt before acknowledging them. An intervening
log failure retains the unacknowledged FIFO suffix; already accepted usage events
in that suffix safely replay with their original identities.

The device stores the serialized payload in its existing telemetry FIFO/journal.
A retry reuses its identity. The database primary key is
`(device_id, boot_id, sequence)`; conflict-ignore inserts preserve the first
record and original receipt time. HTTP success follows the database write. The
firmware retains usage events on every non-2xx or mismatched receipt, including 404 during a
backend rollout. A broken schema or invalid record can block that FIFO; current
health bypasses it through the existing latest-value mailbox.

This provides deduplicated ingestion, not a guarantee that every physical action
is recorded. The 512-item queue is bounded; lock contention, full queues, missing
SD, short writes, card failure and power loss can lose observations. Heartbeats
include pending depth, durable drop count and journal failure count, all since
boot. Journal failures are not equivalent to lost events: RAM may still deliver
an event. Legacy log/crash records do not gain retry deduplication.

## SD write policy

Each durable event is still appended and flushed before enqueue returns; this
change does not add a deferred-durability window. ACKs that remove only volatile
records do not rewrite the journal. Usage-only ACKs checkpoint after 32 retired
records or when the queue empties. Normally up to 31 acknowledged usage records can replay
following a reset (a failed checkpoint can retain more within the bounded journal); database identity deduplication makes this safe. Legacy log or
crash ACKs still checkpoint immediately because those records lack deduplication.
The disk record count is bounded by the same replay capacity: compaction failure
at that bound rejects a new event with a drop/failure counter instead of losing
the unacknowledged tail on restart. A torn/malformed journal object is skipped
with failure/drop counters; a failed append is repaired from the RAM snapshot
before another valid record can be appended to its torn tail.

SD append latency remains on the calling task. Logical byte/write reductions are
not a measurement of physical flash wear or proof of no UI impact. No lossless
claim is made for a failed card, overflow or a power cut during an append.

## Events and meanings

- `app.enter`: one activation; `app.resume`: uncovered/refreshed view.
- `app.cover`, `app.exit`, `app.sleep`: completed foreground interval where known.
  Screen changes do not create launches. Covered/background time and sleep are
  excluded. Foreground residency is not equivalent to user attention. Abrupt
  resets and currently open intervals are not estimated.
- `chat.capture`, `chat.submit`, `chat.ready`, `chat.display`, `chat.cancel`,
  `chat.error`: guarded milestones from the existing agent latency owner.
  Ready/display durations are device monotonic submission-to-milestone values.
- `camera.saved`, `camera.failed`: normal VGA/high-resolution photo save outcome.
  Diagnostic/tutorial capture paths do not all have identical event coverage.
- `video.saved`, `video.failed`: reserved for the separate video-recording branch;
  staging does not yet emit video finalization events.
- `mp3.start/stop/pause/resume`, `audio.start/stop/pause/resume`: playback owner
  transitions, not acoustic evidence. Stop reflects owner cleanup, not exact EOF.
- `audio.capture/finalized/failed`: WAV capture/finalization. Finalized is not a
  promise the app kept/uploaded the file; discard workflows may finalize first.

## Metrics and UI

`dashboard_device_usage(days,device)` aggregates durable rows inside a bounded
1–90 day server receipt window. It returns device/event/boot counts, per-app
launches and foreground duration/interval mean/median/p95, action counts, daily
UTC receipts, unsynced/delayed-or-skewed clocks and interior sequence gaps.
No REST row-limit sampling is used for these aggregates. Queries time out rather
than silently reporting a partial aggregate. Sequence gaps are not a loss rate;
window boundaries, missing tail events and pending uploads prevent that claim.
Backend environments stay separate to avoid summing distinct-device counts.

The existing fleet and device pages embed the new activity console. Missing
migration, missing firmware, failed reads and absent events remain explicit.
Current health exposes transport reliability counters. Device display latency
remains separately labeled and is still based on legacy best-effort log history.

## Backend response timing

The new migration replaces both existing reply timing RPCs with:
backend execution creation → first nonempty, visible, persisted reply belonging
to that execution and user. Empty placeholders, internal notes, deleted records,
and replies predating the execution are excluded. Queue/setup time after
execution creation is included. This is not model-only inference latency, and
reply persistence may follow a provider send. Device upload/render clocks are
not used. Missing replies are excluded from percentiles and remain unmeasured.
The legacy cap of the newest 1,000 eligible executions is disclosed.

Responses include `definition=backend_execution_to_reply_v1`. Dashboard and Pi
monitor consumers reject older definitions rather than silently mislabeling
legacy incoming-message timing. Inspector shows the same backend boundary.

## Deployment order

1. Review/apply `20261026010000_device_usage_events.sql` and
   `20261026011000_backend_reply_timing.sql` to the intended staging database.
2. Deploy the backend event endpoint and heartbeat fields, then the dashboard/Pi
   monitor changes. Verify authenticated insert/replay and read aggregates.
3. Flash the test device preserving NVS/SD, exercise apps and offline/reconnect,
   then compare retained IDs against database rows and aggregates.
4. Only after review and explicit approval, promote production database/backend,
   dashboard and firmware in the same order. Do not deploy new backend heartbeat
   writes before its migration. Old firmware continues to use existing routes.

Rollback: revert application changes, keep additive tables/columns and stored
evidence; restoring the prior timing RPC requires its previous migration body.
Do not drop collected events as a rollback shortcut.

## Local verification

Server: device-events and device-usage-pipeline Vitest suites, production build,
and test typecheck. Pipeline fixtures come from the actual firmware encoder via
`tools/test_usage_events.py --wire-output ...`; HTTP ingestion is exercised with
a local Postgres-compatible PGlite adapter, not a deployed Supabase instance.
Dashboard: new migration/RPC tests plus existing timing/proxy tests, typecheck,
production build, desktop/mobile browser checks with synthetic fixture data.
Firmware: build `esp32-s3-menu-n16r8`, architecture dependency guard, telemetry
queue/drain/journal/encoder tests, navigation, agent-latency, microphone and
playback control tests. Production FAT/SD power-cut testing remains required.
