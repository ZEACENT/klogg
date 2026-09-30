# Incremental / Streaming Search Architecture

## Overview

Static files and live captures share `SearchableLogData`, `LogFilteredData`,
and `LogFilteredDataWorker`. The same pipeline handles an initial full search
and incremental updates as a file grows or a live capture receives data.
`StreamingLogData` implements the searchable interface over `CaptureStore`;
CaptureStore itself is storage, not a `SearchableLogData` implementation.

See the [documentation hub](README.md),
[technical overview](TECHNICAL_DOCUMENTATION.md), and
[live-source architecture](ADB_LOGCAT_ARCHITECTURE.md) for related components.

The important contracts are:

1. **Asynchronous operation dispatch.** Normal search/update requests do not
   join the running search on the UI thread. This is not a blanket claim that
   every search-related call is wait-free: request preparation and explicit
   stop/wait during teardown have different responsibilities.
2. **Catch-up.** A live update tracks an advancing target and resumes from
   processed data. Once input stabilizes, a non-cancelled search can finish
   the requested range; sustained input above processing capacity can lag.
3. **Result identity.** Logical search generations reject superseded results;
   operation IDs distinguish dispatches within a generation.

## Runtime Responsibilities

- `LogData` indexes files; `StreamingLogData` exposes retained live records.
- `CrawlerWidget` routes file/load changes, truncation, and live auto-refresh
  requests. Presentable live tabs coalesce auto-refresh requests on a 250 ms
  window; this is separate from the presentation cadence below.
- `LogFilteredData` owns the current pattern, result bitmaps, UI-side watermark,
  marks, and independent result/status publication timers.
- `LogFilteredDataWorker` accepts requests into a mutex/condition-variable
  dispatch queue. Its dispatch thread joins the previous operation and starts
  the next operation thread, rather than doing that join in `updateSearch()`.
- `FullSearchOperation` clears previous search data; `UpdateSearchOperation`
  resumes existing data. `doSearch()` chooses a single-thread path or a TBB
  chunk pipeline with per-matcher state and serial result combination.

## Unified Search Model

| Scenario | Operation | Behavior |
|----------|-----------|----------|
| New pattern / criteria | `FullSearchOperation` | Clear previous results and scan the requested range |
| File appended | `UpdateSearchOperation` | Resume from the processed watermark |
| Live data appended | `UpdateSearchOperation` | Coalesce/extend the live target and catch up |
| File truncated | `FullSearchOperation` | Drop invalid cached results and replace the current search |

`CrawlerWidget::loadingFinishedHandler()` routes truncation through
`replaceCurrentSearch()`; an append can extend the existing search through
`LogFilteredData::updateSearch()`. The latter supplies its processed-line
watermark to the worker. Full searches may have a nonzero requested start;
"full" does not necessarily mean line zero of the whole source.

## Watermark Mechanism

The watermark is `nbLinesProcessed_` in two places:

1. **`SearchData::nbLinesProcessed_`** is worker-side state protected by its
   data mutex. `addAll()` updates it as processed chunks are combined.
2. **`LogFilteredData::nbLinesProcessed_`** is the owner-thread copy, updated
   when partial search results are consumed.

It is a `LinesCount`, not a byte offset. Within an append-only search,
`addAll()` retains the maximum processed value with `qMax()`. Replacing a
search resets the state; truncation is not treated as ordinary append.

`UpdateSearchOperation::run()` begins with this boundary adjustment:

```text
initialLine = max(searchData.getLastProcessedLine(), initialPosition_)
if initialLine >= 1:
    initialLine--                       // last line may have grown
    searchData.deleteMatch(initialLine) // remove its old match
doSearch(searchData, initialLine)
```

The one-line backup handles a previously unterminated last line whose bytes
have changed. It also prevents the old match for that boundary line from
being counted twice. Match sets are keyed/sorted by source line number;
parallel matcher completion order is not display order.

## Dispatch, Coalescing, and Backpressure

### File Updates

A non-live `updateSearch()` sets the interrupt flag so a current operation
can stop at a chunk boundary, then queues an update. The pending request slot
selects the latest work. The dispatch thread performs the old operation's join
and launches the replacement from the watermark. The caller does not perform
that join or acquire `operationsMutex_` for the duration of the search.

### Live Updates

A live update first stores the newest `liveTargetEndLine_`. If a live update
is already running, it records the coalesced request and returns: ordinary
new data does **not** interrupt and restart the operation for every batch.
The running operation can extend its range as it catches up.

Small pending ranges can be deferred for a bounded coalescing interval;
larger ranges dispatch directly. Deferred requests merge the target end and
resume position. `finishLiveUpdateAndRestartIfNeeded()` handles data arriving
around completion so a newer target is not stranded. Compiled expressions
and pooled matchers are reused across incremental work.

A new full search advances the logical generation and cancels pending or
coalesced live work for the old criteria. Explicit cancellation and teardown
remain interrupt-and-wait operations; they should not be confused with normal
live append scheduling.

### Search During Data Arrival

- **Update in progress:** another live request extends its target. For a file
  source, an update can interrupt and queue a replacement from the watermark.
- **Full search in progress:** live update work is dispatched without the
  ordinary live-data request interrupting the full search. The dispatcher
  serializes the update after the previous operation. File updates can use
  the interrupt/resume path. Changing the pattern is a new full search, not
  just an append update.

## Result Publication and 30 FPS Presentation

Authoritative data and expensive visual publication have different schedules:

- Live append notification and filtered-result publication use a
  fixed-first-deadline **33 ms window (about 30 FPS)**. Repeated arrivals do not
  keep postponing the same deadline.
- Search progress/status has an independent **100 ms** window.
- Terminal completion publishes the final results first, then terminal
  status immediately, with no delayed timer residue.
- Hidden, background-tab, and minimized views suspend expensive presentation,
  while ingestion and model updates continue. Activation consumes accumulated
  dirtiness in one latest-state catch-up. A visible but unfocused window is
  still presentable.

These are coalescing policies, not hard real-time latency guarantees. Performance
work must preserve the active refresh cadence rather than trading away 30 FPS.

## Thread Safety and Lifetime

### Synchronization Responsibilities

`requestMutex_` and `requestCv_` coordinate dispatch; `operationsMutex_`
serializes operation execution; `opThreadMutex_` protects operation-thread
ownership. `SearchData` protects its matches and watermark separately.
CaptureStore protects segment metadata and reads with its recursive mutex.
Queued owner-thread delivery and result publication are additional boundaries,
not a single nested lock chain spanning all these components.

An owner of both Qt-child views and their source data must stop and wait for
view searches before destroying the source data. Asynchronous normal dispatch
does not remove this teardown requirement.

### Logical Generations and Operation IDs

A new full search or explicit `bumpGeneration()` advances
`operationGeneration_`; incremental `updateSearch()` retains it. Advancing the
generation for every append would incorrectly reject valid completion signals
from the same logical search. `operationId_` separately identifies individual
requests; dispatch rejects work superseded by a newer request.

Owner-thread signal marshalling rejects stale work, and view consumers also
check source identity and logical generation. In particular, already queued
Qt signals are not cancelled merely by disconnecting a signal connection.
See the completed generation-ID work in [BACKLOG.md](BACKLOG.md#task-001-search-generation-id-refactoring).

### CaptureStore Reads and Snapshots

CaptureStore uses segment metadata to locate requested lines. Shared resident
payload and spilled-file leases preserve the lifetime of data needed by reads
or snapshots while segments rotate or spill. Export snapshots have their own
fixed record sequence and cursor; they are not mutable live search results.

### TBB Parallel Matchers

For sufficiently large ranges with parallel matching enabled, a TBB flow graph
prefetches chunks, distributes matching across nodes, and combines results in
a serial processor. Each matcher owns its mutable matching context, including
Vectorscan scratch. Small ranges, especially incremental live work, use a
pooled single-thread path to avoid graph/setup overhead. A configured thread
count greater than one therefore does not imply every update uses the graph.

## Vectorscan Block Scan and Per-Line Fallback

### Eligible Chunk Scan

`filterLines()` can use `PatternMatcher::scanBuffer()` over a whole raw chunk,
sharing the block-scan primitive with folder search. Match byte offsets map
back to line indices through the chunk's end-of-line offsets.

Eligibility requires the default-on `perf.useBlockScan` setting, a matcher
that supports buffer scanning, UTF-8-compatible raw bytes, plain ANSI mode,
and no prefilter transformation. Boolean/inverse patterns, unsupported matcher
capabilities, re-encoded data, and transformed chunks retain the per-line path.
The block scan is an optimization; it does not change the line-based result
contract.

### Per-Line Matching

The fallback builds UTF-8 line views, calls `matcher.hasMatch(line)` for each
line, and adds matching source line numbers to the result bitmap. For a
Vectorscan matcher, that invokes `hs_scan()` on an individual line buffer.

### Database Compilation

For per-line Vectorscan matching, `HsRegularExpression` first tries an exact
database. When supported by the pattern, a prefilter database can fall back to
candidate matching followed by `QRegularExpression` verification.

| Variant | Flags | Purpose |
|---------|-------|---------|
| Primary | `HS_FLAG_UTF8 \| HS_FLAG_UCP \| HS_FLAG_SINGLEMATCH` | Exact per-line matching |
| Prefilter | Same + `HS_FLAG_PREFILTER` | Candidates verified by Qt regex |

This table describes the per-line variants, not the separate buffer-scan
compilation contract.

### Callbacks and Boolean Combination

- `matchSingleCallback` marks a single pattern and stops that scan.
- `matchMultiCallback` marks a pattern ID and continues, allowing boolean
  expressions to evaluate all relevant operands for the line.

`parseBooleanExpressions()` extracts quoted operands (with C-style escaping)
and substitutes identifiers such as `p_0`. A multi-pattern matcher produces
the per-line pattern bitmap, and `BooleanExpressionEvaluator` evaluates the
expression against it. Operand producers and this parser must preserve the
same escaping contract.

When Vectorscan is disabled or unavailable for the selected platform/CPU,
`DefaultRegularExpressionMatcher` supplies the Qt fallback. `MatcherVariant`
encapsulates the selected strategy; callers use the common matcher interface.

## CaptureStore Segments Versus Search Chunks

CaptureStore's default segment target is 1 MiB, with line offsets and
`cumulativeEndLine` metadata mapping global lines into each segment. This is a
storage boundary, not the parallel search unit. `doSearch()` works on chunks
of `searchReadBufferSizeLines` obtained through `getLinesRaw()`. Independent
segment-level search remains a possible optimization, not a shipped separate
live-search engine.

## Key Files Reference

| File | Role |
|------|------|
| `src/logdata/include/searchablelogdata.h` | Searchable source interface |
| `src/logdata/include/logfiltereddata.h` | Results, owner-side watermark, marks and visibility |
| `src/logdata/src/logfiltereddata.cpp` | Search lifecycle and result/status publication |
| `src/logdata/include/logfiltereddataworker.h` | SearchData, operations, dispatch and coalescing state |
| `src/logdata/src/logfiltereddataworker.cpp` | Dispatch loop, watermarks, chunk search, matcher reuse |
| `src/logdata/include/capturestore.h`, `src/logdata/src/capturestore.cpp` | Live segments, snapshot and storage lifecycle |
| `src/logdata/src/streaminglogdata.cpp` | Searchable live source and append publication |
| `src/ui/src/crawlerwidget.cpp` | Search request scheduling and presentation activity |
| `src/regex/include/regularexpression.h`, `src/regex/src/regularexpression.cpp` | Matcher abstraction and boolean parsing |
| `src/regex/include/hsregularexpression.h`, `src/regex/src/hsregularexpression.cpp` | Vectorscan compilation, block scan, callbacks, scratch |
| `src/utils/include/atomicflag.h` | Interrupt signalling |
