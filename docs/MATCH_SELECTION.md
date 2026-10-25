# Match selection contract

Select All Matches selects text fragments, not result lines. Copy Matches uses the
same selection and clipboard serialization as Select All Matches followed by Copy.

## Match projection

- Filter searches project participating, non-empty capturing groups when the
  expression has captures; otherwise they project the full match.
- Quick Find projects the full match, preserving its existing navigation behavior.
- Non-capturing groups keep their standard regex meaning. They consume text but
  do not create a capture. Lookarounds do not consume text.
- Within one occurrence, overlapping captured ranges are unioned in text order.
  Adjacent ranges and distinct occurrences remain separate clipboard entries.
- Unmatched groups and zero-length fragments do not produce selections.
- Extraction is independent of highlight colors and the painting pipeline.

## Scope and ordering

- Filter selection includes all completed results in the current search range,
  including matches in collapsed folder groups, not just the viewport.
- Mark-only rows, context-only rows and folder headers are not filter matches.
  Rows which are both marked and matched remain eligible.
- Filter commands reveal matches through the existing visibility controls: a
  Marks-only pane switches to Marks and matches, and folder groups expand. These
  are visible UI state changes, not a hidden second result ordering.
- Quick Find uses its originating view, independently of the filter predicate.
  Commands invoked while its input has focus first confirm the currently typed
  pattern, including when incremental Quick Find is disabled. Otherwise the
  commands target the current Filter pane, regardless of main-view focus.
- Boolean and inverse filters have no defined fragment projection and do not
  support this operation. An incomplete filter search is not presented as all
  results.
- Fragments are copied in result order and then text order, one fragment per
  platform-native line separator. Repeated text is retained; there is no implicit
  deduplication. No line numbers, file headings or log prefixes are added.

## Selection lifetime

A match selection is a snapshot, not a subscription. Later appends do not extend
it. Changing the search, replacing/reordering its results, changing decoding, or
truncating/evicting source data invalidates it rather than retargeting it. Ordinary
selection gestures replace it. Cancelling a batch operation never publishes a
partial selection as a completed Select All Matches result. Ordinary Copy issued
while a batch is pending is deferred until that same snapshot completes; cancelling
or invalidating the batch also discards its pending copy request.

## Commands and platforms

Select All Matches is configurable, with Alt+Return/Alt+Enter defaults (Option on
macOS). The command routes from Filter or Quick Find to its owning log view and
focuses that view before ordinary Copy. Existing Copy bindings remain unchanged.
Platform and Qt-version differences belong in the existing platform/shortcut
abstractions, not in per-document implementations.

## Acceptance fixture

For this source text:

```text
install_plt_hook() [plt-installed] /vendor/lib64/egl/libEGL_emulation.so:dlsym@@0x7b9c9a0b18
```

Both expressions project `/vendor/lib64/egl/libEGL_emulation.so` in Filter:

```regex
(?<=\[plt-installed\] )[^:]*
(?:\[plt-installed\] )([^:]+)(?=:)
```

Single-file, folder and live-stream views share projection, selection and copying.
Source-specific result enumeration and lifetime handling stay with their owners.

## Regression coverage

The unit tags `[match-projection]`, `[match-selection]` and `[selection]` cover
projection semantics, raw/display columns, selection geometry, cancellation and
rendering. Integration tags `[match-selection]` and `[match-selection-stop]`
exercise file, folder and live owners, real command routing, and stopped-result
readiness. Run test executables sequentially when using shared local test roots.

The hidden `[match-selection-perf]` case measures selection and extraction of
40,000 fragments using the best of three samples. Run it explicitly with
`KLOGG_PERF_GATES=1` against an optimized non-sanitizer build. It is not a CI
wall-clock gate; deterministic batch-yield and cancellation tests run in CI.
