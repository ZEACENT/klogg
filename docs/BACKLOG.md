# Klogg Task Backlog

| ID | Task | Priority | Status | Related |
|----|------|----------|--------|---------|
| TASK-001 | [Search generation ID refactoring](#task-001-search-generation-id-refactoring) | Low | Done | [PR #11](https://github.com/ZEACENT/klogg/pull/11) |
| TASK-002 | [Chart Panel — regex match → time series](#task-002-chart-panel) | Medium | Planned | [SPEC](SPEC_CHART_AND_FILTERS_PANEL.md) |
| TASK-003 | [Filters Panel — persistent dock with groups & pinning](#task-003-filters-panel) | Medium | Planned | [SPEC](SPEC_CHART_AND_FILTERS_PANEL.md) |

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
