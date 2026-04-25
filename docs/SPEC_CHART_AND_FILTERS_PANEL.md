# Specification: Chart Panel + Filters Panel

This document specifies two new dock-widget features for klogg, drafted as the
next iteration after the TASK-001 generation-ID refactor.  The features are
inspired by features observed in `64x-lunicorn/LogSquirl` (see the comparison
notes in the project's commit history) but their implementation, scope, and
naming are klogg-specific.

The two features share UX patterns (dock widget, persistent state across
sessions, per-tab vs. global activation) and so are specified together; they
are nevertheless **independent deliverables** and can ship in either order.

- **Filters Panel** (TASK-003) — recommended first.  Smaller scope, exposes a
  feature most klogg users already have but cannot easily discover.
- **Chart Panel** (TASK-002) — recommended second.  Larger scope; opens a new
  capability dimension (post-mortem visualisation) but introduces a new
  long-term design surface (timestamp parsing).

Each section below is structured the same way:

1. Goal / non-goals
2. UX
3. Data model
4. Phased iteration plan
5. Files affected
6. Testing
7. Open questions
8. Effort estimate

A final section gives the cross-feature recommendations (CMake / Qt module
implications, settings schema migration, Q5 vs Q6 compatibility).

---

## TASK-002: Chart Panel

### 1. Goal / non-goals

**Goal.** Render a time-series chart of regex-matched events inside klogg, so
that post-mortem analysis goes beyond "search & scroll" into "see when things
happen and how often."

**Non-goals (Phase 1).**

- Timestamp parsing from log lines.  Phase 1 uses the line number as the
  horizontal axis ("density per N lines"); real wall-clock time becomes a
  Phase-3 optional extension and requires a separate timestamp-profile design.
- Statistical analysis beyond count / mean / min / max / p99 in a bucket.  No
  derivative, integral, autocorrelation, or anomaly detection.
- Cross-tab aggregation.  One chart per tab, exactly mirrors that tab's
  `LogFilteredData`.
- Plotting raw line content.  Only `LogFilteredData` matches and (Phase 2)
  numeric capture-group values are charted.

### 2. UX

A new `QDockWidget` named `Chart Panel` lives on the right side of
`MainWindow`, floatable and dockable like the existing search and overview
widgets.  The panel is **per-tab**: switching tabs swaps in that tab's chart
configuration.

**Two modes**, switchable from a small selector in the panel toolbar:

- **Filter Frequency** (Phase 1): one line per active highlighter / predefined
  filter, plotted as match count per bucket.
- **Capture-group values** (Phase 2): when the search regex contains a numeric
  capture group such as `latency=(\d+)ms`, an additional chart aggregates the
  captured values themselves (mean / p99 per bucket, configurable).

**Bucket size** is one of `auto`, 100 ms, 1 s, 10 s, 1 min, 5 min.  In Phase 1
the unit is "lines" not "seconds", so the choices are `auto`, 100, 1k, 10k,
100k lines.  `auto` picks the bucket so the chart shows ~100 buckets across the
visible range.

**Click-to-jump.**  Clicking a chart point scrolls the main view to the first
matching line in that bucket.  Hover shows a tooltip with bucket index, count,
and (in capture-group mode) the aggregate value.

**Preset.**  A Save Preset / Load Preset toolbar action serializes the
selection of highlighters, mode, and bucket size to JSON, stored in
`Configuration` under `chart.presets`.  Presets are listed in a combobox.

**Empty state.** If no highlighters are active and no search has been run, the
panel shows a help label with a one-line example: *"Enable a highlighter or
run a search to populate the chart."*

### 3. Data model

A new `ChartPanelModel` (in `src/ui/include/chartpanelmodel.h`) owns:

- A reference to the tab's `LogFilteredData`.
- The current mode (`Frequency` / `CaptureGroup`).
- The bucket size in line-count units (Phase 1).
- Per-active-filter:
  - filter ID (`HighlighterId` or `PredefinedFilterId`)
  - colour (mirrors the highlighter's colour)
  - bucket counts (`std::vector<quint64>`, indexed by bucket number)
  - in capture-group mode: bucket aggregate (`klogg::vector<double>` of mean
    or p99 per bucket)

**Data flow.**  The model subscribes to the existing
`LogFilteredData::searchProgressed` signal (after TASK-001's generation gate,
so it benefits from the same staleness protection).  On each progress event
it pulls newly-discovered matches from `LogFilteredData::getMatchingLineNumber`
and accumulates them into bucket counts.  No re-scanning of the file; we
piggyback on the existing search results.

For capture-group mode, we additionally pull `LogFilteredData::getLineString`
for each new match, run the configured capture regex on it, and accumulate
the numeric value into the bucket's aggregator (an online mean/percentile
estimator -- the existing `klogg/utils` directory is the natural home).

### 4. Phased iteration plan

**Phase 1 (M, ~2-3 weeks).**

- Add the `ChartPanelModel` and `ChartPanelWidget` (Qt5/Qt6 compatible
  rendering using `QtCharts` if available, fallback to a hand-rolled
  `QPainter`-based renderer if `QtCharts` is unavailable in the target build).
- Filter Frequency mode only.
- Line-number x-axis only.
- Click-to-jump on bucket centres.
- Preset save/load.
- All settings persisted under `Configuration::chartPanel`.

**Phase 2 (M, ~2 weeks).**

- Capture-group numeric aggregation (mean / p99).
- Per-filter on/off toggle inside the chart toolbar.
- Hover tooltip with rich content.

**Phase 3 (L, ~3-5 weeks, optional).**

- Timestamp x-axis.  Requires a separate "Log Profile" / "Timestamp Format"
  design surface where users either:
  - Pick a preset profile (ISO-8601, RFC-3339, syslog, Android logcat
    `MM-DD HH:mm:ss.SSS`, Java `yyyy-MM-dd HH:mm:ss.SSS`), OR
  - Provide a regex with a single named capture group `?<ts>` plus a
    strptime-style format string.
- Profile is per-tab and persisted in the session.

### 5. Files affected (Phase 1 estimate)

```
NEW   src/ui/include/chartpanelmodel.h
NEW   src/ui/include/chartpanelwidget.h
NEW   src/ui/src/chartpanelmodel.cpp
NEW   src/ui/src/chartpanelwidget.cpp
NEW   tests/ui/chartpanel_test.cpp
EDIT  src/ui/src/crawlerwidget.cpp        (subscribe / wire dock)
EDIT  src/ui/src/mainwindow.cpp           (View menu entry)
EDIT  src/settings/include/configuration.h (chart preset storage)
EDIT  src/ui/CMakeLists.txt                (compile new files; QtCharts dep)
EDIT  CMakeLists.txt                       (optional: KLOGG_USE_QTCHARTS)
```

Roughly 1.5 - 2k LOC + tests.

### 6. Testing

**Unit tests.**

- `ChartPanelModel` bucket accumulation given synthetic match streams (Phase
  1).
- Capture-group aggregator: mean / p99 correctness against reference
  implementations on small fixed inputs (Phase 2).
- Preset JSON round-trip.

**UI tests.**

- Chart panel renders without crash on empty / sparse / dense data.
- Clicking a bucket centres the main view on the right line range.
- Switching tabs swaps in the right configuration.

**Performance.** Bucket accumulation must add no measurable cost on top of
search; assert in a benchmark (`benchmarks/chartpanel_overhead_benchmark.cpp`)
that for 1M-match workloads the model update completes in < 50ms.

### 7. Open questions

- **Q1.** Does klogg's existing build matrix have `QtCharts` for both Qt 5.9
  and Qt 6.5+?  If not, the `QPainter` fallback path becomes mandatory rather
  than optional.
- **Q2.** Should the panel reflect the search results of the **active
  filtered tab**, or include marks?  Default to "matches only", configurable.
- **Q3.** Live sources (ADB logcat, future tail-mode): the model should keep
  appending to the rightmost bucket until the bucket boundary is crossed.
  Confirm with at least one ADB logcat reproduction that the chart updates
  in real time without main-thread stalls.
- **Q4.** Theming: charts must respect klogg's light/dark theme.  Phase 1
  uses the highlighter colours directly, Phase 2 may need a chart-only colour
  override.

### 8. Effort estimate

- Phase 1: **M** (2 - 3 weeks of focused work)
- Phase 2: **M** (2 weeks)
- Phase 3: **L** (3 - 5 weeks; timestamp profile design + UX tax)

Total to feature-complete (Phase 1 + 2): **~5 weeks**.

---

## TASK-003: Filters Panel

### 1. Goal / non-goals

**Goal.** Make the existing Predefined Filters feature discoverable and
fast-to-toggle by surfacing it as a persistent left-side dock with grouping
and pinning.

**Non-goals.**

- Adding new filter capabilities.  The data path is unchanged; this is a
  presentation refactor.
- Replacing `HighlightersDialog`.  The dialog stays for create / edit
  workflows; the dock is for activation / toggle.
- Server-synced filter library / community sharing.  Out of scope.

### 2. UX

A new `QDockWidget` named `Filters` lives on the left side of `MainWindow`,
collapsible to a sliver.  Contents are a `QTreeView` with filter groups as top
nodes and individual filters as leaves.

```
[ + ]  [ - ]  [ ↑ ]  [ ↓ ]                  toolbar (add/remove/move)

▼ Common (pinned)            ☑   ☐ pin   ⚙
   ☑ ERROR
   ☑ WARN
▼ Android (3)
   ☐ logcat-tag-pattern
   ☐ stacktrace-frame
   ☐ ANR
▶ Customer-Acme-incident-2026-04 (5)
▶ Backend-tracing (8)
```

- Each leaf has a checkbox: ON / OFF toggles whether the filter is applied to
  the **current tab**.
- Each leaf can be drag-dropped between groups; drop on the group header
  re-parents.
- Each group has its own pin toggle; pinned groups (and their checked filters)
  auto-apply to every newly opened tab.
- Right-click on a filter -> context menu with `Edit...` (opens
  `HighlightersDialog` scoped to that filter), `Delete`, `Move to group...`.
- Right-click on a group -> `Rename`, `Delete (filters move to Default)`,
  `Add filter...`.
- Empty state: a default `Default` group exists if no others are configured.

### 3. Data model

`PredefinedFiltersCollection` (already in
`src/ui/src/predefinedfilters.cpp`) gets an additional field per filter:

```cpp
struct PredefinedFilter {
    QString name;
    QString pattern;
    bool isRegex;
    QString group;   // NEW: empty string == "Default"
    bool pinned;     // NEW: per-group; stored on representative filter,
                     // computed at load time
};
```

**Activation state** (which filters are checked for the current tab) is
session-scoped state on `CrawlerWidget`, not part of the persisted filter
collection.  Pinned filters are auto-checked on tab creation; everything else
defaults off.

### 4. Phased iteration plan

**Phase 1 (S, ~3-5 days).**

- Schema migration: read legacy flat list → write back with `group=""`.
- `Filters` dock widget with `QTreeView`, group expand / collapse, leaf
  checkboxes, edit-via-dialog right-click action.
- No drag-drop, no pin yet.  Filters are activated on the current tab only;
  no auto-apply.

**Phase 2 (S, ~2-3 days).**

- Pin toggle on groups; pinned filters auto-apply on new-tab creation.
- Drag-drop reorder within and between groups; persist new ordering.
- Group rename / delete UX.

**Phase 3 (S, ~1-2 days).**

- Quick filter search box in the panel toolbar (case-insensitive substring
  match against filter name + pattern).
- Per-group "select / deselect all" checkbox.

### 5. Files affected (Phase 1 estimate)

```
NEW   src/ui/include/filterspanelwidget.h
NEW   src/ui/include/filterspanelmodel.h
NEW   src/ui/src/filterspanelwidget.cpp
NEW   src/ui/src/filterspanelmodel.cpp
NEW   tests/ui/filterspanel_test.cpp
EDIT  src/ui/include/predefinedfilters.h     (group, pinned fields)
EDIT  src/ui/src/predefinedfilters.cpp        (serialization + migration)
EDIT  src/ui/src/mainwindow.cpp               (View menu entry, instantiate)
EDIT  src/ui/src/crawlerwidget.cpp             (apply pinned-filter set on open)
EDIT  src/ui/CMakeLists.txt
```

Roughly 600 - 900 LOC + tests.

### 6. Testing

**Unit tests.**

- `PredefinedFiltersCollection` migration: legacy QSettings flat list reads
  back as `group=""` with `pinned=false`.
- Round-trip of grouped collection.

**UI tests.**

- Construct dock + collection with a couple groups, assert tree structure.
- Toggle a leaf -> confirm `CrawlerWidget` receives the activation
  notification.
- Pin a group -> open new tab -> confirm pinned filters auto-applied.
- Drag-drop one filter to another group -> persisted state reflects the move
  after teardown / reload.

### 7. Open questions

- **Q1.** Should "pin" be per-group or per-filter?  Per-group is simpler; per-
  filter is more flexible.  Default to per-group; revisit only if user
  feedback complains.
- **Q2.** Should activation state survive session restore?  Default yes: the
  filters checked when the session was saved should be re-checked on load.
- **Q3.** Performance: `LogFilteredData` recomputes when filters change.  Is
  the existing change-detection in `CrawlerWidget` sufficient, or do we need
  to debounce rapid toggles (a user clicking through 10 checkboxes in a
  second)?  Probably yes; reuse the search-update throttle from
  `crawlerwidget.cpp:88` (`kSearchThrottleActiveMs`).
- **Q4.** Should the dock float by default?  No; left-docked, collapsed-to-
  sliver by default for new users (one-time first-run setting).

### 8. Effort estimate

- Phase 1: **S** (3 - 5 days)
- Phase 2: **S** (2 - 3 days)
- Phase 3: **S** (1 - 2 days)

Total to feature-complete: **~1.5 weeks**.

---

## Cross-feature notes

### Settings schema

Both features add new sections to `Configuration`:

- `chart.presets` (TASK-002): list of `{name, mode, bucketSize, filterIds}`
- `chart.lastUsedPreset` (TASK-002): string name, restored on tab open
- `filtersPanel.expandedGroups` (TASK-003): list of group names persisted
  expanded across sessions
- `filtersPanel.dockArea` (TASK-003): standard Qt dock area enum
- `predefinedFilters[].group` and `predefinedFilters[].pinned` (TASK-003):
  per-filter additions, with backward-compatible default of empty / false

### Qt 5 / Qt 6 compatibility

- `QtCharts` exists in both Qt 5.9+ and Qt 6.x.  Confirm the klogg CI matrix
  has it linked in (Q1 above).  If absent for any platform, the `QPainter`
  fallback in TASK-002 is mandatory.
- `QTreeView` + drag-drop in TASK-003 is fully Qt 5 / Qt 6 compatible; no
  concerns.

### Dependencies on other work

- TASK-002 benefits from but does not depend on TASK-001 (the generation gate
  prevents stale signals from corrupting the chart's bucket counts during
  rapid search restarts).
- TASK-003 has no upstream dependencies and could be done by anyone today.

### Suggested order

`Filters Panel (TASK-003)` first, `Chart Panel (TASK-002)` second.  Rationale:

1. Filters Panel ships in days, not weeks; immediate user-visible value.
2. Filters Panel provides empirical signal on which filters users actually
   activate, which informs Chart Panel's default visualisation.
3. Chart Panel introduces the timestamp-profile design surface (Phase 3),
   which is best deferred until the simpler features have settled.

---

_Last updated: 2026-04-25_
