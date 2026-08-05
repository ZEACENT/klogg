# Klogg Task Backlog

| ID | Task | Priority | Status | Related |
|----|------|----------|--------|---------|
| TASK-001 | [Search generation ID refactoring](#task-001-search-generation-id-refactoring) | Low | Done | [PR #11](https://github.com/ZEACENT/klogg/pull/11) |
| TASK-002 | [Chart Panel — regex match → time series](#task-002-chart-panel) | Medium | Planned | [SPEC](SPEC_CHART_AND_FILTERS_PANEL.md) |
| TASK-003 | [Filters Panel — persistent dock with groups & pinning](#task-003-filters-panel) | Medium | Planned | [SPEC](SPEC_CHART_AND_FILTERS_PANEL.md) |
| TASK-004 | [macOS code signing and notarization hardening](#task-004-macos-code-signing-and-notarization-hardening) | Medium | Planned | CI Release |
| TASK-005 | [Continuous and Release tag Changes automation](#task-005-continuous-and-release-tag-changes-automation) | Medium | Planned | Releases |
| TASK-006 | [Vectorscan 5.4.12 upgrade and search-backend hardening](#task-006-vectorscan-5412-upgrade-and-search-backend-hardening) | High | Planned | [Vectorscan 5.4.12](https://github.com/VectorCamp/vectorscan/releases/tag/vectorscan%2F5.4.12) |

### TASK-001: Search generation ID refactoring

**Scenario:**
`CrawlerWidget::replaceCurrentSearch()` needs to discard stale `searchProgressed`
signals from an interrupted search before starting a new one. The current approach
temporarily disconnects and reconnects the
`LogFilteredData::searchProgressed` / `CrawlerWidget::updateFilteredView` slot.

**Problem:**
With `Qt::QueuedConnection`, `disconnect()` does not remove already-posted
`QMetaCallEvent`s from the receiver's event queue — they will still be delivered
after reconnect. Currently the window between disconnect and reconnect is very
small (a few synchronous calls in `replaceCurrentSearch()`), so the practical
impact is negligible. However, the pattern is fragile and could become a real bug
if the window widens in future refactors.

**Code context:**
- Signal: `LogFilteredData::searchProgressed(LinesCount, int, LineNumber)` — `src/logdata/include/logfiltereddata.h:149`
- Emit sites (6+): `src/logdata/src/logfiltereddata.cpp` (lines 164, 634, 646, 666), `src/logdata/src/logfiltereddataworker.cpp` (lines 319, 456, 485, 623, 708)
- Disconnect/reconnect: `src/ui/src/crawlerwidget.cpp` (lines 1830–1831, 1904–1905)
- Slot: `CrawlerWidget::updateFilteredView()` — `src/ui/src/crawlerwidget.cpp:630`

**Proposed fix:**
1. Add a monotonic `uint64_t` generation counter to `LogFilteredData`, incremented by `runSearch()` / `updateSearch()`
2. Extend `searchProgressed` signal to carry the generation ID
3. In `CrawlerWidget::updateFilteredView()`, ignore signals where `generation != activeSearchGeneration_`
4. Remove the disconnect/reconnect calls in `replaceCurrentSearch()`

**Trade-offs:**
Cross-cutting change touching signal signature, all emit sites, and all connected slots.
Should be done in a dedicated PR with thorough regression testing.

**Resolution:**
Implemented in branch `docs/backlog-generation-id`.  The wire type for the
generation argument is plain `quint64` rather than the
`LogFilteredDataWorker::OperationGeneration` typedef, because moc treats
typedefs of non-builtin types as unregistered metatypes and `QSignalSpy`
decodes the `QVariant` back to 0; the typedef alias is kept for
code-readability but does not appear in any `Q_SIGNAL` signature.  Two
new SCENARIOs in `tests/ui/logfiltereddata_test.cpp` cover generation
increment and signal payload.  Receiver-side staleness gate is factored
into `klogg::isStaleSearchGeneration` (`src/logdata/include/searchgeneration.h`)
with its own unit test.  Cache-hit path bumps the generation via
`LogFilteredDataWorker::bumpGeneration()` so prior-search progress
signals are correctly dropped.

### TASK-002: Chart Panel

**Scenario:**
Render a time-series chart of regex-matched events inside klogg so that
post-mortem analysis goes beyond "search & scroll" into "see when things
happen and how often".

**Spec:** [`SPEC_CHART_AND_FILTERS_PANEL.md`](SPEC_CHART_AND_FILTERS_PANEL.md)
covers goals / non-goals, UX, data model, three phased iterations
(Phase 1: filter-frequency on a line-number axis; Phase 2: capture-group
numeric aggregation; Phase 3: optional timestamp axis), files affected,
testing strategy, and effort estimate (~5 weeks for Phase 1 + 2).

**Recommended order:** ship after TASK-003.

### TASK-003: Filters Panel

**Scenario:**
Surface the existing Predefined Filters feature as a persistent
left-side dock with grouping and pinning, so users can toggle filter
sets in one click instead of opening a multi-step dialog.

**Spec:** [`SPEC_CHART_AND_FILTERS_PANEL.md`](SPEC_CHART_AND_FILTERS_PANEL.md)
covers goals / non-goals, UX (tree view + drag-drop + per-group pin),
data-model migration (legacy flat list → grouped collection), three
phased iterations, testing strategy, and effort estimate (~1.5 weeks
to feature-complete).

**Recommended order:** ship before TASK-002.

### TASK-004: macOS Code Signing And Notarization Hardening

**Scenario:**
Release macOS artifacts should be signed and notarized consistently so users can
open downloaded DMGs without Gatekeeper warnings or manual override steps.

**Problem:**
The packaging action contains signing and notarization hooks, but the backlog
needs a dedicated plan to verify identity setup, entitlement requirements,
secret handling, and artifact validation for both Intel and Apple Silicon
packages.

**Proposed plan:**
1. Document required Apple Developer certificates, installer identities, App
   Store Connect issuer/key material, and GitHub secret names.
2. Add CI preflight checks that fail early when signing secrets are missing on a
   release run.
3. Sign the `.app` bundle before DMG creation, sign the DMG, submit for
   notarization, staple the result, and verify with `codesign`, `spctl`, and
   `stapler`.
4. Keep unsigned local/PR builds possible while making release signing behavior
   explicit in logs and artifact metadata.

### TASK-005: Continuous And Release Tag Changes Automation

**Scenario:**
The `continuous` prerelease and versioned release tags should publish useful
Changes text so users can understand what changed without digging through CI
runs or commit history.

**Problem:**
Release assets are uploaded automatically, but the release notes need a clear
plan covering the moving `continuous` tag and immutable version tags.

**Proposed plan:**
1. Generate Continuous Changes from commits merged since the previous
   successful Continuous build, grouped by change type using
   `scripts/gen_changelog.py`.
2. Generate Release Changes from the previous version tag to the new version
   tag, including highlights, fixed issues, and artifact notes.
3. Update the release workflow to write the generated text into the GitHub
   Release body after assets are uploaded.
4. Add a dry-run mode that prints the planned release notes in CI logs for PR
   validation before changing any GitHub Release.

### TASK-006: Vectorscan 5.4.12 Upgrade And Search-Backend Hardening

**Scenario:**
Klogg uses Vectorscan as the high-throughput regular-expression backend for
line filtering, folder search, and highlighter candidate selection. The pinned
5.4.11 dependency predates the current stable 5.4.12 release and the search
stack needs stronger ISA, semantic, and measurement guarantees before further
performance work is safe.

**Evidence and scope:**
- Klogg pins Vectorscan in both the normal and prefetch CPM projects:
  `3rdparty/CMakeLists.txt` and `cmake/prefetch_cpm/CMakeLists.txt`.
- Vectorscan 5.4.12 fixes an AVX512VBMI Teddy page-end out-of-bounds read
  ([upstream #333](https://github.com/VectorCamp/vectorscan/issues/333)). The
  issue has no assigned CVE/GHSA; treat it as a stability and memory-safety
  maintenance update, not as a documented RCE.
- Klogg currently uses block-mode databases. The live-log transport is a byte
  stream, but `CaptureStore` rebuilds complete lines before filtering; adopting
  Vectorscan streaming mode is not a substitute for line assembly.
- Vectorscan is configured with UTF-8/UCP flags. Raw block scanning depends on
  valid UTF-8 and on scanned byte offsets remaining aligned with `endOfLines`.

**Target:**
Upgrade from Vectorscan 5.4.11 commit
`d29730e1cb9daaa66bda63426cdce83505d2c809` to the exact 5.4.12 tag commit
`b585ad466658624bb31fb1d194cdb168df34833c`. Do not pin a moving `master` or
`develop` revision.

**Phase P0 — Release-blocking correctness and portability:**
1. Define explicit generic, AVX2, and experimental AVX512/VBMI build profiles.
   Keep AVX512/VBMI disabled for normal artifacts until target-hardware tests
   demonstrate compatibility and benefit. Align artifact labels, linked ISA,
   runtime checks, and the documented minimum CPU.
2. Use target-architecture detection that covers both `arm64` and `aarch64`,
   and align the Vectorscan enable gate with the actual ISA required by the
   linked library.
3. Update both CPM pins, rebase and validate the MSVC warning and aligned-free
   patches, and run the upstream Vectorscan unit suite once in the upgrade PR.
   Verify recursive submodules, CPM prefetch, cache size, and offline builds.
4. Check every `hs_scan()` return value. A scan failure must not silently be
   reported as an empty match set; recover through a documented fallback or
   make the failure visible to tests and diagnostics.
5. Make raw block-scan input validity explicit. When a complete valid UTF-8
   byte-coordinate contract cannot be proven, use the decoded per-line path.
6. Add differential tests comparing Qt per-line, Vectorscan per-line, and
   Vectorscan block matched-line sets. Cover zero-width patterns, CRLF,
   unterminated lines, UTF-8/UCP, malformed input, case folding, and chunk
   boundaries. Any accepted cross-line block-scan difference must be a small,
   documented whitelist.

**Phase P1 — Measurement and regression visibility:**
1. Split benchmark timing into compile, single/block database creation, scratch
   allocation, matcher cloning, steady-state scan, and end-to-end search.
2. Correct the full, incremental, and live benchmark runner, aggregation keys,
   denominators, and emitted environment metadata (CPU model/features,
   compiler, Qt, Vectorscan commit, ISA flags, and exact/prefilter/fallback
   classification).
3. Add fixed-rate live-input benchmarks that report append-to-visible-match and
   model-update p50/p95/p99 latency, backlog age, event-loop stalls, and
   catch-up time.
4. Run a same-environment `KLOGG_USE_VECTORSCAN=ON/OFF` CI matrix and extend
   the encoding × ANSI × backend × search-path integration corpus.

**Phase P2 — Measurement-gated performance work:**
1. If compile latency is a measured hotspot, lazily create the block database
   and scratch only when a whole-buffer path needs them.
2. If repeated patterns are common and memory is bounded, add an observable,
   evictable compiled-expression cache that shares immutable databases but
   keeps scratch private to each matcher.
3. Profile block callback, line-offset mapping, and `seenLines` costs before
   reusing/epoching buffers or choosing block versus per-line scanning by
   workload characteristics.
4. Tune live batching, worker start frequency, merge thresholds, and matcher
   pool limits only against fixed-rate p95/p99 and backlog measurements.
5. Use folder-search benchmarks to distinguish I/O, enumeration, encoding,
   decoding, regex, and context-assembly bottlenecks before optimizing any one
   layer. Track prefilter candidate rate and Qt confirmation time separately.

**Phase P3 — Deliberate long-term experiments:**
1. Review post-5.4.12 upstream correctness fixes individually; backport only
   small, sourced fixes with dedicated regression tests until a new stable
   release is available.
2. Add deterministic property/fuzz testing with a fixed seed, failure
   minimization, and sanitizer coverage.
3. Evaluate `FAT_RUNTIME`, AVX512/VBMI, and ARM SVE/SVE2 only as separately
   labelled distribution experiments with target-hardware smoke tests.
4. Design `HS_MODE_STREAM` only for an explicit future product feature that
   supports continuous multi-line regex spans, stream ownership, reset/close
   semantics, and span-aware results.
5. Treat GPU regex as a separate, restricted offline batch-processing research
   path. It is not a direct Vectorscan backend upgrade and is not the default
   solution for interactive ADB/iOS live filtering.

**Acceptance criteria:**
- The 5.4.12 pin, patched builds, and upstream/Klogg test suites pass on the
  supported x64 and ARM build matrix.
- Generic and AVX2 artifacts run on their documented minimum hardware without
  an illegal instruction fault.
- Exact-mode matched-line sets agree with the Qt oracle outside documented
  block-scan exceptions.
- Benchmark artifacts make compile cost, steady-state throughput, and live
  end-to-end latency independently comparable before P2 optimizations land.
- Every `hs_scan()` return value is checked; a scan failure either triggers the
  documented fallback or is made visible to tests and diagnostics.
- Raw block scanning only runs on input validated against the UTF-8
  byte-coordinate contract; anything else takes the decoded per-line path.
- Differential tests cover the P0 corpus, and every accepted block-scan
  exception is documented.
- Both CPM pins, the patched builds, CPM prefetch, offline builds, and the
  normal-artifact ISA restrictions are verified in the upgrade PR.
