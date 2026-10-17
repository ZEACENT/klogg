# ADB Logcat Live Source Architecture

This document describes current Android live capture and its shared capture,
search, and save path. See the [documentation hub](README.md),
[technical overview](TECHNICAL_DOCUMENTATION.md), and
[dependency inventory](DEPENDENCIES.md) for related architecture and packaging.

## Sources and Transport

Fresh Android sessions use a managed ADB server and smart-socket transport,
not a per-tab `adb logcat` stdout process. The packaged, source-built ADB helper
supplies server infrastructure; `AdbSmartSocketTransport` supplies the stream.
`AdbInfrastructureManager`, `AdbServerSupervisor`, and `AdbDeviceTracker` own
infrastructure readiness and device discovery. This distinction matters:
launching a helper server is not the same as using QProcess as the log data path.

Fresh iOS sessions are macOS-only and use native libimobiledevice through
`IosNativeTransport` and the native adapters in `src/livecapture/`. They do not
require Python or a Python log-stream process. The shared source controller
retains the historical name `AdbLogcatSource` for both Android and iOS sources.

Old process-backed sessions can be restored as compatibility read-only tabs.
Their process transport classes are not the fresh-session product path, and
klogg does not expose a generic arbitrary-process capture feature. The Android
legacy *logcat format* retry is separate: it retries unsupported timestamp
modifiers using threadtime format, without switching to a legacy process backend.

## Capture and Presentation

1. A source adapter delivers bytes through the live transport contract.
2. `AdbLogcatSource` settles accepted delivery and passes UTF-8 data to
   `StreamingLogData::appendUtf8()`.
3. `CaptureStore::appendUtf8()` normalizes records, commits complete lines to
   segments, and retains a pending partial line until more bytes or end of input.
   `finishInput()` finalizes an unterminated last record.
4. `StreamingLogData` exposes the capture through `SearchableLogData`; the main
   view reads it directly and `LogFilteredData` supplies the filtered view.
   Search and presentation use the capture, not the optional output file.
5. Live append publication is coalesced on a fixed 33 ms window (about 30 FPS).
   Hidden/background/minimized views suspend expensive presentation, not
   ingestion; becoming presentable consumes accumulated changes.

The data store distinguishes accepted ingress, committed records, capture
persistence, and optional output writes. A failure after partial acceptance is
not permission to replay an entire byte batch. Likewise, an output-file error
does not mean the capture append was rolled back.

## Key Defaults

| Parameter | Default / behavior | Source |
|-----------|--------------------|--------|
| Segment target | 1 MiB | `CaptureStore::Limits::segmentTargetBytes` |
| Resident payload budget | 256 MiB | `CaptureStore::Limits::memoryBudgetBytes` |
| Ingress allowance | 16 MiB | `CaptureStore::Limits::ingressBudgetBytes` |
| Capture root | `QDir::tempPath()/klogg_live/{captureId}/` | `capturestore.cpp` |
| Segment name | `segment_000000.log` style | `capturestore.cpp` |
| Bound output flush | 1 MiB or 1,000 lines, plus a 1-second timer | `capturestore.h`, `streaminglogdata.cpp` |
| Rolling size / backups | Disabled by default (`0`) | `CaptureStore::Limits` |
| Retained line limit | Unlimited by default (`0`) | `CaptureStore::Limits::maxTotalLines` |

These are payload/retention controls, not a promise that total process RSS is
limited to 256 MiB. Indexes, caches, snapshots, allocator overhead, and pending
work have separate costs. The active segment and partial records also make a
segment target different from a strict maximum record size.

## Default Mode: Memory and Capture Files

CaptureStore keeps resident segment payload and spills data into its capture
directory. Segment metadata records byte offsets and line lengths, so readers
can address lines without loading the whole capture. Store synchronization,
shared resident data, and leases on spilled files protect reads and snapshots
against concurrent lifecycle changes.

Without a bound output file, the capture remains usable for viewing and search.
Capture persistence and cleanup are separate from output-file flushing;
`CaptureStore::flush()` flushes bound output, not capture segments. See
`persistPending()` and the secure capture-directory implementation for persistence
and cleanup details rather than assuming that each displayed line is durable.

## Save Live Log As: Snapshot, Tail, and Future Output

The UI uses `LiveLogExportService`, not a synchronous whole-capture write on the
UI thread. An export job pins a `CaptureStore::Snapshot`, writes it to staged
output on a worker, and drains a bounded tail of data accepted while the
snapshot is being written. Successful publication and owner-thread cutover
bind future output to the selected destination.

The intended contents are the **current capture snapshot, its in-flight tail,
and future output**. Save As does not concatenate an older rolling-file family.
A stopped source can still export its retained capture. Cancellation, tail
overflow, snapshot-read errors, publication failures, and cutover failures are
explicit job results; they must not be reported as a successful save.

Once bound, future records also go to persistent output. Flushing is batched
(1 MiB / 1,000 lines / 1 second), not performed for every line. These flushes
are not an fsync-style power-loss durability guarantee. Capture memory/spill
storage continues independently so the views do not need to reopen the saved
file for normal display.

### Rolling Output and Restore

Rolling output uses the configured maximum file size and backup count. The
optional retained-line window is a separate capture-store concern; saved-file
history and currently retained searchable history need not be identical.

Session persistence records stopped run intent. Restoring a tab loads its
capture and configuration without starting device capture automatically.
Restoring an output binding uses `OutputBindMode::Restore`: it resumes an
existing output rather than truncating it or exporting the snapshot again.
Compatibility process sessions remain read-only; a supported stopped session
requires an explicit user start/reconnect to resume capture.

## Lifecycle and Search Invariants

- Connection state, user run intent, and transport completion are distinct.
  A request to stop is not proof that all queued bytes have settled.
- Generation-tagged callbacks prevent a previous transport attempt from
  updating a newer session. Final input handling follows delivery settlement.
- Searches share the file/live `LogFilteredDataWorker` pipeline. Live targets
  coalesce rather than restarting a worker for every arriving batch.
- Search results publish at 33 ms cadence, independently of 100 ms
  progress/status publication; terminal results precede terminal status.
- There is no guaranteed one-event-loop or 16 ms end-to-end latency. Device,
  transport, ingestion, search, and presentation scheduling all contribute.

See [Incremental / Streaming Search Architecture](INCREMENTAL_SEARCH_ARCHITECTURE.md)
for dispatch, watermarks, and presentation rules.

## Implementation References

| File / area | Responsibility |
|-------------|----------------|
| `src/livecapture/src/adbinfrastructuremanager.cpp` | Managed ADB infrastructure |
| `src/livecapture/src/adbserversupervisor.cpp` | Helper/server lifecycle |
| `src/livecapture/src/adbdevicetracker.cpp` | Device tracking |
| `src/livecapture/src/adbsmartsocketclient.cpp` | ADB protocol client |
| `src/ui/src/adbsmartsockettransport.cpp` | Android stream transport |
| `src/ui/src/iosnativetransport.cpp`, `src/livecapture/src/iosnativeadapter.cpp` | Native iOS transport/adaptation |
| `src/ui/src/adblogcatsource.cpp` | Shared source delivery and lifecycle |
| `src/logdata/src/streaminglogdata.cpp` | Searchable live data, publication, output timer |
| `src/logdata/include/capturestore.h`, `src/logdata/src/capturestore.cpp` | Limits, records, snapshots, spill, output and rolling |
| `src/ui/src/livelogexportservice.cpp` | Asynchronous staged export and cutover |
| `src/ui/src/session.cpp` | Capture/session restore and compatibility gate |
| `packaging/adb/`, `packaging/ios-native/` | Source-built helper/library packaging |
