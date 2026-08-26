# Live Capture Benchmark Methodology

## Synthetic Matrix

The deterministic matrix runs 128 versioned framed records per trial through the actual benchmark-only transport compositions. The process arm uses `ProcessLiveSourceTransport`; the integrated arm uses `IosNativeTransport` with an injected source session. Both feed `LiveLogController` and then `StreamingLogData` with its owned `CaptureStore`. Two scenarios are measured: steady capture with one segment and deterministic reconnect with two segments. Each scenario uses balanced ABBA order: process, integrated, integrated, process. Every trial runs in a fresh process with one stream at a time.

## Metric Definitions

- **Process tree:** child processes started and maximum simultaneously live children, counted by the benchmark transport factory.
- **CPU:** process and reaped-child user plus system CPU from `getrusage`, reported in nanoseconds when supported.
- **RSS:** process peak resident set size from `getrusage`, normalized to bytes.
- **Context switches:** voluntary and involuntary process-tree deltas from `getrusage`.
- **Startup:** start to production transport `Connected`.
- **First byte latency:** `Connected` to the first accepted transport byte.
- **First commit latency:** `Connected` to the first framed record whose append increased the `StreamingLogData` line count.
- **Throughput:** committed payload bytes divided by first-byte to last-commit time.
- **Teardown:** last committed record to stopped state after staged fixtures and the `CaptureStore` root are verified empty.
- **Queue:** process-arm queue internals are unavailable because the production process transport does not expose them. Integrated queue values come from the injected native session and are explicitly marked synthetic.

## Correctness Gates

Every row requires contiguous sequence numbers, canonical CRC32, exact fixture identity, expected reconnect transitions, all 128 records committed, normal stop, and cleanup verification. A failed gate produces no successful row.

## Real-Device Safety Gates

Real-device comparison is optional and fail-closed. Enumeration is passive through the packaged native catalog. A run requires exactly one USB endpoint, an existing pair record, and an already available baseline. The endpoint identifier is pinned only in process-local temporary state and is never written to results. Trials must use balanced ABBA, one stream at a time, short bounds, no install, no launch, no pair or unpair, no clear, no reboot, and no settings changes. A failed gate emits `not_run` instead of weakening safety. The native ABI closure is validated before either warmup. Acceptance uses a newly isolated stack rebuilt from locked local archives and all four pinned patches. Native trials force the syslog-relay service to match the baseline `syslog live` source. Two warmups precede twelve measured rows in three balanced ABBA blocks.

## Cleanup and Privacy

Synthetic fixtures are temporary and deleted before each trial returns. The caller-owned `CaptureStore` root must be empty. Real-device data, if a comparison is permitted, may only be counted in memory with no raw log retention. Aggregate JSON contains counts, timings, resource metrics, correctness flags, availability states, and reason codes only.

## Real-Device Acceptance Execution

The acceptance run used two-second captures, ANSI disabled, the same volatile `CaptureStore` configuration, one stream at a time, two warmups, and twelve measured trials. Each trial ran in a fresh process, retained aggregates only, verified cleanup, and checked for leaked stream children before the next trial. Natural logs are not claimed to be event-equal, and no zero-drop claim is made because neither arm exposes a complete natural-log drop counter.

## Production-Default os_trace Acceptance

The pre-fix production-default warmup terminated with sanitized decode error 7 (`SpanOutOfBounds`). Protocol review found that the native callback had erased the relay record type: type 1 control plists could reach the packet decoder, while type 2 activity records and type 8 log-message records share the fixed 129-byte header but only type 8 defines the five trailing text spans. Applying those text lengths to an opaque type 2 activity body misroutes a valid alternate record shape into strict log-message validation. The callback boundary now classifies and ignores bounded binary/XML control plists, and the decoder preserves the 16 MiB limit, explicit little-endian fixed-header checks, timestamp and level validation, and UTF-8 sanitization; type 2 bodies remain opaque and are never retained, while type 8 text spans retain all strict bounds and terminator checks.

Sanitized diagnostics report only decode code, field, packet byte count, marker, packet type, declared header size, five declared lengths, declared span total, and available span bytes. They never include packet bytes, message text, process metadata, or a device identifier. Unit/property and native worker regressions pass.

Post-fix acceptance passively requires exactly one USB endpoint and an existing pair record through the verified isolated native stack; stale and network endpoints are rejected. The identifier exists only in a mode-600 file inside a mode-700 ephemeral directory and is deleted after the run. Execution uses the production-default `os_trace` service through `IosNativeTransport -> LiveLogController -> AdbLogcatSource -> StreamingLogData -> CaptureStore`, two warmups followed by exactly three measured two-second trials, one stream and one fresh process at a time, ANSI disabled, and `volatile-capture-v1`. Each row must have non-empty bytes and committed lines, `format_complete=1`, `error_count=0`, `ansi_escape_count=0`, no child process, no backpressure event, normal stop, and an empty deleted `CaptureStore` root. Any failure is terminal and recorded with its exact sanitized reason; successful rows retain aggregate metrics only. Natural event equality and zero-drop are not claimed because the native drop counter is not exposed.
