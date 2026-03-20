# Incremental / Streaming Search Architecture

## Overview

This document describes the unified search architecture that klogg uses for
both static files and live streams (e.g. ADB logcat).  The same pipeline
handles the initial full search triggered by the user and every incremental
update that follows when new data arrives -- whether that data comes from a
growing file on disk or from a live `CaptureStore` stream.

The design achieves three goals simultaneously:

1. **Non-blocking UI** -- the search runs on a dedicated `std::thread`; the
   main thread never waits for it.
2. **Eventual consistency** -- every line that has been committed to the data
   source will eventually be searched, even under continuous high-throughput
   streaming.
3. **Correctness** -- results are never lost and line ordering is preserved.

---

## Architecture Diagram

```text
  Data sources
  ============
  File on disk            CaptureStore (live stream)
       |                         |
       v                         v
  +-----------------------------------------+
  |          SearchableLogData               |  (abstract interface)
  |  getNbLine(), getLinesRaw(), getFileSize()|
  +-----------------------------------------+
       |                         |
       |   fileChanged /         |
       |   loadingFinished       |
       v                         v
  +------------------------------------------+
  |           CrawlerWidget                   |
  |  loadingFinishedHandler():                |
  |    if truncated -> replaceCurrentSearch()  |
  |    else         -> updateSearch()          |
  +------------------------------------------+
                     |
         runSearch() | updateSearch()
                     v
  +------------------------------------------+
  |          LogFilteredData                  |
  |  nbLinesProcessed_  (watermark)          |
  |  matching_lines_    (Roaring64Map)       |
  |  currentRegExp_                          |
  +------------------------------------------+
         |                        |
   search()               updateSearch()
         v                        v
  +------------------------------------------+
  |       LogFilteredDataWorker               |
  |  operationsMutex_                        |
  |  interruptRequested_  (AtomicFlag)       |
  |  operationGeneration_ (atomic<uint64>)   |
  |  searchData_          (SearchData)       |
  |  opThread_            (std::thread)      |
  +------------------------------------------+
              |
              v
  +------------------------------------------+
  |         SearchOperation                   |
  |  FullSearchOperation::run()              |
  |    -> searchData_.clear()                |
  |    -> doSearch(0)                        |
  |  UpdateSearchOperation::run()            |
  |    -> doSearch(lastProcessedLine)        |
  +------------------------------------------+
              |
              v
  +---------------------------------------------------+
  |  doSearch()  --  single-thread or TBB pipeline     |
  |                                                    |
  |  [single-thread path]                              |
  |    loop: getLinesRaw() -> filterLines() -> addAll() |
  |                                                    |
  |  [TBB flow-graph path]                             |
  |    limiter -> buffer -> N x matcherNode -> buffer  |
  |                              |                     |
  |                              v                     |
  |                       matchProcessor -> addAll()   |
  +---------------------------------------------------+
              |
              v
  +------------------------------------------+
  |     PatternMatcher  (per thread)          |
  |  HsSingleMatcher | HsMultiMatcher        |
  |  | HsPrefilterMatcher                    |
  |  | DefaultRegularExpressionMatcher        |
  +------------------------------------------+
```text

---

## Unified Search Model

The same two operation classes serve every scenario:

| Scenario                | Operation           | What happens                                                      |
|-------------------------|---------------------|-------------------------------------------------------------------|
| New pattern entered     | `FullSearchOperation`  | `searchData_.clear()`, scan from line 0                          |
| File appended           | `UpdateSearchOperation`| Resume from `nbLinesProcessed_` watermark                        |
| Live stream data        | `UpdateSearchOperation`| Identical to file append -- CaptureStore is a SearchableLogData  |
| File truncated          | `FullSearchOperation`  | `clearSearch(dropCache=true)`, then full `replaceCurrentSearch()` |

`CrawlerWidget::loadingFinishedHandler()` decides which path to take:

- `searchState_.isFileTruncated()` --> `replaceCurrentSearch()` (FullSearch)
- otherwise --> `logFilteredData_->updateSearch(startLine, endLine)`

`LogFilteredData::updateSearch()` passes `LineNumber(nbLinesProcessed_.get())`
as the resume position to the worker.

---

## Watermark Mechanism

The watermark is `nbLinesProcessed_` stored in two places:

1. **`SearchData::nbLinesProcessed_`** -- the worker-side watermark, updated
   atomically inside `addAll()` after each chunk is processed.  It records the
   highest line number that has been fully scanned.

2. **`LogFilteredData::nbLinesProcessed_`** -- the UI-side copy, updated in
   `handleSearchProgressed()` when partial results are consumed.

Key properties:

- **Line number, not file offset.**  The value is a `LinesCount` representing
  how many lines from the source have been searched.
- **Monotonically increasing.**  `addAll()` uses `qMax()`:
  `nbLinesProcessed_ = qMax(nbLinesProcessed_, processedLines)`.
- **Append-only assumption.**  UpdateSearch assumes new data only appears after
  the current end of the file.  If this assumption is violated (truncation),
  the UI layer detects it via `MonitoredFileStatus::Truncated` and triggers a
  FullSearch instead.

When `UpdateSearchOperation::run()` starts, it computes:

```text
initialLine = max(searchData.getLastProcessedLine(), initialPosition_)
if initialLine >= 1:
    initialLine--                     // re-check last line (may not have been LF-terminated)
    searchData.deleteMatch(initialLine)  // avoid double-counting
doSearch(searchData, initialLine)
```text

The one-line backup handles the edge case where the previously-last line was
incomplete (no trailing newline) and has since had more bytes appended to it.

---

## Backpressure Handling

When the data source produces lines faster than the search worker can consume
them, the system uses an **interrupt-and-resume** pattern rather than
buffering.

### Timeline

```text
  Time ---->

  Data source:  [lines 0..999]  [lines 1000..1999]  [lines 2000..2999]
                     |                  |                   |
  loadingFinished    |                  |                   |
                     v                  v                   v
  Search worker: |--UpdateSearch----|  |--UpdateSearch--| |--UpdateSearch--|
                  watermark=0->1000    watermark->2000    watermark->3000

  If search is still running when new data arrives:

  Data source:       [0..999]        [1000..1999]
                        |                 |
                        v                 v
  Search worker: |---UpdateSearch---X    |--UpdateSearch--|
                  watermark=0->700       watermark=700->2000
                  (interrupted at 700)   (resumes from 700)
```text

### The Interrupt Mechanism

`LogFilteredDataWorker::updateSearch()` begins by calling
`interruptRequested_.set()` *before* acquiring `operationsMutex_`.  The
currently running `doSearch()` loop checks `interruptRequested_` at every chunk
boundary and exits early.  The mutex is then released, the old thread is joined,
and a new `UpdateSearchOperation` is launched from the watermark.

### Guarantees

- **Ordering** -- Lines are scanned in monotonically increasing order.  The
  Roaring64Map result set preserves insertion order implicitly (it is sorted
  by line number).
- **Eventual consistency** -- As long as data eventually stops arriving, the
  search will run to completion and cover every line.
- **Non-blocking UI** -- The main thread never calls a blocking function on
  the worker.  `interruptRequested_` is an atomic flag; setting it is
  wait-free.
- **Correctness** -- Partial results accumulated before an interruption are
  preserved in `SearchData`.  The next UpdateSearch resumes from the
  watermark, so no lines are skipped and no lines are double-counted (the
  one-line backup + `deleteMatch` handles the boundary).

---

## Search During Data Arrival

Two scenarios arise depending on which operation is in flight when new data
arrives.

### Scenario 1: UpdateSearch Running When New Data Arrives

This is the common case during live streaming.

1. `loadingFinishedHandler()` calls `logFilteredData_->updateSearch()`.
2. `LogFilteredDataWorker::updateSearch()` sets `interruptRequested_`.
3. The running `doSearch()` exits at the next chunk boundary.
4. The old thread is joined; `interruptRequested_` is cleared.
5. A new `UpdateSearchOperation` starts from
   `max(searchData.getLastProcessedLine(), position)`.
6. All matches found before the interrupt are already in `searchData_` and
   `matching_lines_`; nothing is lost.

### Scenario 2: FullSearch Running When New Data Arrives

This can happen if the user changes the search pattern while data is still
being indexed.

1. `FullSearchOperation::run()` called `searchData_.clear()` at the start,
   then began scanning from line 0.
2. While scanning, new data arrives and `loadingFinishedHandler()` fires.
3. `updateSearch()` sets `interruptRequested_`.
4. The FullSearch exits early.  At this point `searchData_` contains partial
   results for lines 0 through `watermark`.
5. The new `UpdateSearchOperation` resumes from the watermark.
6. Results are correct because FullSearch already cleared `searchData_` and
   accumulated all matches for lines below the watermark.  UpdateSearch
   continues from there and adds the rest.

---

## Thread Safety

### Lock Hierarchy

Locks must always be acquired in the order shown below (outermost first).
Acquiring them in a different order risks deadlock.

```text
  operationsMutex_          (LogFilteredDataWorker -- Mutex)
      |
      v
  CaptureStore::mutex_      (std::recursive_mutex -- protects segments_)
      |
      v
  SearchData::dataMutex_    (SharedMutex -- protects matches_, nbLinesProcessed_)
      |
      v
  searchProgressMutex_      (LogFilteredData -- Mutex -- protects searchProgress_ tuple)
```text

### operationGeneration_ Atomic Counter

Each call to `search()` or `updateSearch()` increments
`operationGeneration_`.  The generation is captured by the task lambda and
passed through signal marshalling.  When a queued signal arrives on the
owner thread, the handler compares the captured generation against the
current value:

```cpp
if (generation != operationGeneration_.load()) return;  // stale, discard
```text

This filters out progress/finished signals from a search that has already been
superseded, avoiding races where a delayed signal from search N arrives after
search N+1 has started.

### CaptureStore Snapshot-Based Reading

`CaptureStore` protects its `segments_` vector with a `std::recursive_mutex`.
Readers call `buildRawLines()` or `lineAt()`, which lock the mutex, locate the
relevant segment(s), and read data.  If a segment's `memoryData` is a
`std::shared_ptr<QByteArray>`, the shared_ptr keeps the underlying buffer alive
even if the writer thread rotates or spills the segment concurrently --
preventing use-after-free.

### TBB Parallel Matchers

When `matchingThreadsCount > 1`, the TBB flow-graph path is used.  Each
`RegexMatcherNode` holds its own `PatternMatcher` instance (via the
`matcherData` vector, indexed by node).  These matchers have **no shared
mutable state** -- the Vectorscan `hs_scratch_t` is cloned per matcher, and
the `HsMatcherContext` is local.  The only shared write target is `SearchData`,
which is protected by `dataMutex_` and only accessed by the single serial
`matchProcessor` node.

---

## Vectorscan Block Scan

### Per-Line Approach (Current Implementation)

The current search path operates per-line.  `doSearch()` reads a chunk of raw
lines via `getLinesRaw()`, then calls `filterLines()` which iterates line by
line:

```cpp
for (auto offset = 0u; offset < lines.size(); ++offset) {
    const auto& line = lines[offset];
    if (matcher.hasMatch(line)) {
        results.matchingLines.add(lineNumber.get());
    }
}
```text

Each `hasMatch()` call invokes `hs_scan()` on the individual line buffer.

### Database Compilation

`HsRegularExpression` compiles two database variants using `hs_compile_multi`:

| Variant          | Flags                                        | Purpose                        |
|------------------|----------------------------------------------|--------------------------------|
| Primary          | `HS_FLAG_UTF8 \| HS_FLAG_UCP \| HS_FLAG_SINGLEMATCH` | Exact matching, terminates on first match |
| Prefilter        | Same + `HS_FLAG_PREFILTER`                   | Fallback when primary compilation fails (unsupported syntax) |

The primary database is tried first.  If `hs_compile_multi` fails (e.g. the
pattern uses unsupported features), the prefilter database is compiled instead.
Prefilter matches are then verified with `QRegularExpression` to eliminate
false positives.

### Callbacks and Match Dispatch

Two callbacks are used:

- **`matchSingleCallback`** -- sets `context->matchingPatterns[0] = true` and
  returns 1 (halts scanning).  Used by `HsSingleMatcher` for single-pattern
  searches.
- **`matchMultiCallback`** -- sets `context->matchingPatterns[id] = true` and
  returns 0 (continues scanning all patterns).  Used by `HsMultiMatcher` for
  multi-pattern / boolean searches.

### Boolean Combination Handling

When the user enters a boolean expression (e.g. `"error" AND "timeout"`):

1. `parseBooleanExpressions()` extracts quoted sub-patterns and replaces them
   with unique IDs (`p_0`, `p_1`, ...) in the expression string.
2. All sub-patterns are compiled into a single `HsMultiMatcher` database.
3. Per line, `hs_scan()` produces a pattern bitmap (`MatchedPatterns` --
   a `std::string` where each byte is 0 or 1).
4. `BooleanExpressionEvaluator::evaluate()` evaluates the boolean expression
   against the bitmap.

### QRegularExpression Fallback

If the CPU lacks SSE2/SSSE3, or if Vectorscan is not compiled in
(`!KLOGG_HAS_VECTORSCAN`), all matching falls back to
`DefaultRegularExpressionMatcher`, which wraps `QRegularExpression`.  The
matcher interface (`MatcherVariant`) is a `std::variant`, so the switch is
resolved at construction time with zero per-line dispatch overhead.

---

## CaptureStore Segment-Level Parallelism

`CaptureStore` organizes data into fixed-size segments
(`segmentTargetBytes`, default 1 MiB).  Each segment tracks its own line
offsets and byte boundaries.  This structure enables future segment-level
parallel search:

- Segments that have already been spilled to disk (or are in memory) can be
  scanned independently by separate TBB tasks.
- The `cumulativeEndLine` field on each segment maps global line numbers to
  segment-local offsets, allowing results to be expressed in global line
  numbers and merged into the shared `SearchData`.
- The `shared_ptr<QByteArray> memoryData` ensures that a segment's buffer
  remains valid for the duration of any reader, even if the writer thread
  rotates or spills the segment concurrently.

Currently, the TBB parallelism in `doSearch()` operates at the **chunk** level
(chunks of `searchReadBufferSizeLines` lines from `getLinesRaw`), not at the
segment level.  Segment-level parallelism would be an optimization for
CaptureStore-backed sources specifically.

---

## Key Files Reference

| File | Role |
|------|------|
| `src/logdata/include/searchablelogdata.h` | Abstract interface for all searchable data sources (`getLinesRaw`, `getNbLine`) |
| `src/logdata/include/logfiltereddata.h` | Owns search results, watermark (`nbLinesProcessed_`), marks, and visibility |
| `src/logdata/src/logfiltereddata.cpp` | Orchestrates search lifecycle: `runSearch`, `updateSearch`, `handleSearchProgressed` |
| `src/logdata/include/logfiltereddataworker.h` | Defines `SearchData`, `SearchOperation`, `FullSearchOperation`, `UpdateSearchOperation`, `LogFilteredDataWorker` |
| `src/logdata/src/logfiltereddataworker.cpp` | Implements the search loop (`doSearch`), TBB flow graph, interrupt/resume, generation filtering |
| `src/logdata/include/capturestore.h` | Segment-based append-only store for live streams |
| `src/logdata/src/capturestore.cpp` | Segment rotation, memory budget enforcement, spill-to-disk |
| `src/ui/include/crawlerwidget.h` | UI controller: connects file-change signals to search operations |
| `src/ui/src/crawlerwidget.cpp` | `loadingFinishedHandler`, `fileChangedHandler`: decides FullSearch vs UpdateSearch |
| `src/regex/include/regularexpression.h` | `RegularExpression`, `PatternMatcher`: compile-time dispatch for match strategy |
| `src/regex/src/regularexpression.cpp` | Boolean expression parsing, matcher construction, inverse match |
| `src/regex/include/hsregularexpression.h` | Vectorscan database compilation, `HsSingleMatcher`, `HsMultiMatcher`, `HsPrefilterMatcher` |
| `src/regex/src/hsregularexpression.cpp` | `hs_scan` callbacks, scratch cloning, prefilter fallback |
| `src/utils/include/atomicflag.h` | `AtomicFlag`: lock-free interrupt signalling between UI and worker threads |
