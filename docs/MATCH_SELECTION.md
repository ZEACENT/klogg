# Match selection contract

Select All Matches selects text fragments, not result lines. Copy Matches uses the
same accepted selection and clipboard serialization as Select All Matches followed
by Copy. The [pipeline execution record](MATCH_SELECTION_PIPELINE_PLAN.md) tracks
implementation and acceptance; pending verification there is not a claim of a
released feature.

## Shared matching and projection

Filter and Quick Find use the same normalized, unexpanded source text, query
options, occurrence representation, and capture projection:

- When the expression has capturing groups, project participating, non-empty
  captures. Otherwise project the full match.
- Noncapturing groups consume text without creating a capture. Lookarounds do
  not consume text. Neither changes the projection rule.
- Within one occurrence, union overlapping captures in text order. Adjacent
  ranges and distinct occurrences remain separate clipboard entries.
- Unmatched groups and zero-length fragments produce no copyable text. A
  zero-width occurrence can still be a navigation hit or a matching row.
- Navigation advances by the whole occurrence, not the last projected capture.
- Backreferences and capture conditionals keep their capture numbering and
  participation even in predicate-only consumers; suppressing captures must not
  turn a valid expression into an invalid or differently matching one.
- Tabs remain Tabs while matching and copying. A separate coordinate map expands
  them for display. ANSI processing and prefilter removal precede matching;
  changing decoding or either interpretation setting retires affected selections.
- Row screening backends must respect physical record boundaries. Expressions
  without a safe whole-block execution path fall back to per-record matching.
  CRLF framing, malformed encoding and BOM normalization follow the shared decoder.

This intentionally changes the initial Quick Find behavior: navigation now selects
captures, when present, rather than always selecting match zero. Quick Find no
longer matches Tab-expanded spaces while Filter matches the underlying Tab.

## Accepted scope and ordering

- Filter selection includes actual matches from the completed result snapshot in
  the selected search range, including matches in collapsed folder groups.
- Mark-only rows, context-only rows and synthetic headers are not Filter matches.
  Rows which are both marked and matched remain eligible.
- Extraction does not expand groups or change visibility. Hidden eligible matches
  are copied even when they have no current selection geometry.
- Quick Find uses its originating view's presented data rows, independently of
  the Filter predicate. Headers are excluded. Its scope is frozen when accepted;
  later expansion or insertion of marked/context rows does not enlarge it.
- Commands invoked while the Quick Find input has focus confirm its typed pattern,
  including when incremental Quick Find is disabled. Otherwise match commands
  target the Filter pane. Ordinary selection/copy commands use the active view,
  not a stale Quick Find origin from a previous interaction.
- Boolean and inverse Filter predicates do not have a fragment projection and
  remain unsupported for this operation. Ordinary row filtering is unchanged.
- An incomplete, cancelled or superseded Filter execution is not advertised as a
  completed result. Readiness includes the requested execution and source cutoff,
  not just the query generation. Stopping an already completed search does not
  invalidate its result. Presented rows can remain readable for Quick Find while
  Filter matching is incomplete; this does not make them canonical Filter matches.
- Copy preserves accepted result order, then fragment text order. It writes one
  fragment per platform-native separator and retains repeated text. Ordinary Copy
  adds no headings, prefixes or line numbers. The explicit numbered-copy command
  uses captured one-based source-line labels rather than mutable view row numbers.
  If the requested numbering policy changes during serialization, the obsolete
  formatting is discarded and recomputed before clipboard publication.

## Content ownership and lifetime

A match selection is a one-shot snapshot, not a subscription. Appends after
acceptance never extend it. Selecting leaves follow scrolling, but does not stop
capture or alter the transport.

Source adapters provide fixed readable versions:

- Indexed files retain the bytes actually indexed, together with immutable line
  boundaries. Reading a retained version never reopens the original path for its
  text. This is consistency with the indexed bytes, not an atomic filesystem
  snapshot of a file modified concurrently by an external writer.
- Folder results retain scanned bytes in a scan-wide arena, with accepted member
  cutoffs and canonical match membership independent of the visible layout.
- Live sources lease committed capture records and existing spill storage.
  Uncommitted partial ingress is neither finalized nor included by selection.
- Concatenated sources lease their children and namespace repeated child
  occurrences, keeping source labels separate from global presentation positions.

The published selection owns its projected text, identities, source labels and
ordering. Copy does not reread source text. Layout changes remap geometry by stable
identity; folding, visibility, context and mark changes do not retarget fragments
or discard hidden text. A new query, changed interpretation, source replacement,
changed indexed-file tail identity, or eviction of a selected identity retires the
affected selection rather than silently copying unrelated current rows. Live
partial ingress remains outside the committed selection scope.

Directional and incremental Quick Find also read accepted snapshots rather than
mutable source rows. Delayed results retain their request and row identities, so
an older completion cannot replace newer work. Restoring a saved selection on
Quick Find exit validates its source, query and interpretation, and remaps any
layout-only change. A zero-fragment occurrence retains its navigation anchor
without fabricating selected text or reviving stale content.

Ordinary selection gestures replace match selection. Cancelling an operation
never publishes partial success. Copy issued during extraction attaches to that
operation; cancellation or invalidation also discards its pending Copy. Copy of
an already-published selection is serialized asynchronously from shared immutable
payload. Progress remains available through serialization. Completion does not
steal focus from an input the user focused in the meantime.

Acquisition/read/serialization failures leave the clipboard unchanged. Source
retention uses bounded resident storage and private spill files; projected
fragment storage and the final clipboard string are proportional to the selected
output. Cancellation is checked between bounded work units and serialization
chunks; a regex engine call already in progress is not forcibly interrupted.

## Selection consumers and rendering

One non-empty contiguous fragment may be used as a search operand. A batch of
fragments is not implicitly converted into a newline-containing query or Boolean
OR. Find Next/Previous Selected and Add/Replace/Exclude Search therefore use an
explicit operand capability rather than treating every partial selection alike.

Painting resolves stable identities to the current layout, clips spans to the
visible paint window, and composes retained spans with an event sweep rather than
scanning every span at every boundary. Existing layer/blend rules and later-source
ordering within a layer are preserved. Paint clipping or interval merging never
changes clipboard fragment boundaries. Wrapped layout uses bounded prefix probes
rather than repeatedly shaping the remaining suffix. Painting and hit testing
share immutable row geometry qualified by text, font metrics and pixel width.
A viewport-sized cache has an explicit logical-pixel origin; full frame extent
still owns bottom alignment and hit coordinates. Only the cached window's text
is submitted to the painter, including for an unhighlighted row.

Layout remapping is synchronous even when presentation is inactive; it does not
force a paint or change the existing timers and approximately 30 FPS active
refresh policy.

## Commands and platforms

Select All Matches is configurable, with Alt+Return/Alt+Enter defaults (Option on
macOS). Existing Copy bindings remain unchanged. Initial routing may focus the
target view; asynchronous completion does not restore that focus.

Immediate, deferred and direct match copying use the same GUI-thread clipboard
sink and request the same X11 PRIMARY policy. Platform separators, clipboard
behavior and shortcut differences belong in platform abstractions, not in
per-document implementations.

## Acceptance fixture

For this source text:

```text
install_plt_hook() [plt-installed] /vendor/lib64/egl/libEGL_emulation.so:dlsym@@0x7b9c9a0b18
```

Both expressions project `/vendor/lib64/egl/libEGL_emulation.so` in Filter and
Quick Find when their candidate scopes contain that row:

```regex
(?<=\[plt-installed\] )[^:]*
(?:\[plt-installed\] )([^:]+)(?=:)
```

## Regression and platform acceptance

Core tags include `[match-projection]`, `[query-options]`,
`[query-capture-references]`, `[match-selection]`, `[selection]`,
`[quickfind-snapshot]`, `[quickfind-restoration]`, `[filtered-snapshot]`,
`[owner-layout]` and `[owner-layout-copy]`. Source-specific tests cover indexed
versions, capture leases, folder arenas and concatenated snapshots. Deterministic
regressions use completion signals, identities, gates and work counters instead
of timing budgets.

Final local acceptance passed macOS Qt5/Qt6, native ASan/UBSan, locked Linux
ASan/LSan and a real xcb Clipboard/PRIMARY test. The final Qt5 staged application
passed Cocoa/offscreen smoke. The execution record contains exact counts, source
provenance and measurements, and explicitly excludes unrun Windows/TSan runtime
coverage and the failing Homebrew Qt6 packaging variant.

Run `[match-selection-x11]` explicitly with the xcb plugin and a real X server;
ordinary offscreen tests do not establish PRIMARY clipboard behavior. The hidden
`[match-selection-perf]` case measures 40,000-fragment selection/copy using the
best of three samples with `KLOGG_PERF_GATES=1` in an optimized non-sanitizer build.
It includes asynchronous clipboard serialization, not just retrieving a text list.
The hidden `[match-paint-perf]` case records best-of-three warmed wrapped and
unwrapped repaint costs for a 40,000-fragment row; run with Catch's `-s` to print
the measurements. Neither replaces deterministic viewport/sweep work-count tests
or changes the refresh-timer policy. See the execution record for exact completed
checks and outstanding acceptance.
