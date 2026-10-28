# Unified match pipeline: defects and implementation plan

## Status and how to use this plan

- Status: P0-P7 implementation and requested local macOS/Linux acceptance are
  complete. The final verification section supersedes intermediate checkpoint
  qualifications and explicitly records unrun environments and packaging limits.
- Recorded: 2026-10-26; reconciled against code and completed checkpoints on 2026-10-27.
- Reviewed baseline: `a5383e3d` on `worktree-feature-req`.
- Feature baseline: `438733c4`; GCC portability follow-up: `bbd61b35`.
- Related request: [issue #82](https://github.com/ZEACENT/klogg/issues/82).
- This document is the canonical task list and execution record. The
  [match-selection contract](MATCH_SELECTION.md) describes the unified behavior,
  including intentional changes from the initial feature.
- Read alongside [incremental search architecture](INCREMENTAL_SEARCH_ARCHITECTURE.md),
  [portability guidance](PORTABILITY.md), and the [build guide](BUILD.md).

The original passing suites did not cover the defects recorded below. The defect
register preserves that baseline; the disposition table, acceptance map and
execution log describe the current implementation and its evidence. A passing
intermediate suite is not final-tree or cross-platform acceptance. Symbol names
are search anchors, not permanent line-number references.

Execution rules:

1. Follow the phase dependencies; do not replace this plan with scattered widget
   invalidation calls or a renamed version of the existing monolithic loop.
2. For each behavior change, demonstrate RED, implement GREEN, then simplify.
   For behavior-preserving refactoring, establish passing characterization first.
3. Check a task only after its exit criteria pass. Add the implementation commit,
   exact test command, result, and any remaining limitation to the execution log.
4. Keep commits coherent and buildable. Do not commit disabled regressions or
   weaken assertions to make a phase appear complete.
5. Revalidate the baseline and relevant symbols before starting: other work may
   change these paths. Do not overwrite unrelated changes.

## 1. Goals, non-goals, and behavioral decisions

### Goals

- The same query options and source text produce the same occurrences and
  projected fragments in Filter and Quick Find.
- Selection, painting, and copying consume one consistent match result rather
  than independently interpreting mutable view coordinates.
- File, folder, and live sources implement the same read/identity contracts;
  their actual storage and scope differences remain in source adapters.
- Presentation layout does not determine which Filter results are extractable.
- Large operations remain cancellable through extraction and serialization,
  without reducing the existing 33 ms / approximately 30 FPS presentation cadence.

### Decisions to implement

| Concern | Target contract |
| --- | --- |
| Trigger | Only an explicit Select All Matches or Copy Matches command creates an operation. Search completion, incoming data, and ordinary Select All do not. |
| Live behavior | One-shot selection, not an incremental subscription. Record the requested cutoff when accepting the command; never silently include later arrivals. |
| Follow | Preserve the existing request to leave follow scrolling when selecting. Do not stop capture or change device/transport state. |
| Projection | Filter and Quick Find both use participating non-empty capture groups when captures exist, otherwise the full match. Do not redefine noncapturing groups or lookarounds. |
| Occurrence versus fragment | Preserve the overall occurrence/navigation anchor separately from projected fragments. Several captures are not several navigation hits. |
| Overlap | Union overlapping captures within one occurrence. Preserve adjacent fragments, separate overlapping occurrences, and repeated text as separate copy entries. |
| Empty captures | Do not create copyable fragments for unmatched or zero-length captures. A zero-width occurrence can still matter for navigation or a whole-line highlighter predicate. |
| Filter scope | Actual Match rows from the completed result snapshot in the selected range. Match-and-Mark remains eligible; mark-only, context-only, and synthetic headers do not. |
| Quick Find scope | Its explicitly resolved target scope, independent of the Filter predicate. Preserve the current view-scope policy initially, including which folded rows that scope exposes. Freeze it at acceptance instead of rescanning mutable display indices. |
| Unsupported extraction | Boolean and inverse Filter predicates remain unsupported for fragment selection. Keep their ordinary row-search behavior; do not invent a projection during this refactor. |
| Copy | One fragment per existing platform-native separator, deterministic source/result order then text order; no implicit deduplication, headers, prefixes, or line numbers. Existing explicit copy-with-line-number behavior remains a separate policy. |
| Deferred Copy | Bind to a selection operation ID and target, not to whatever selection is current later. Cancel/invalidate discards that operation's pending copy intent. |
| Focus | Initial command routing may focus the target. Completion must not override a later user focus change. |
| Unsupported/failed work | Preserve the clipboard and report a nonmodal reason. Never publish a partial or mixed-version result as a successful Select All Matches operation. |

Changes to Quick Find projection and its use of normalized rather than
Tab-expanded matching text are intentional compatibility changes. Update tests
and user-facing documentation in the phase that activates them. Unification must
include Quick Find navigation and highlighting, not only the new batch command.
Do not silently change unrelated configured-highlighter predicate semantics.

Non-goals: continuous live extraction, new regex syntax, arbitrary capture-group
selection UI, changing ordinary whole-row Select All, replacing every search
backend, lowering refresh frequency, or clearing unrelated analyzer style warnings.

## 2. Defect and architecture-debt register

All entries were **OPEN at the recorded baseline**. The table below retains the
original defects and required outcomes; it does not describe the current code as
still containing the old implementation. Current dispositions follow the table.
"Semantic migration" and "architecture debt" distinguish deliberate baseline
choices from accidental bugs.

| ID | Kind / urgency | Reproduction or evidence | Required outcome | Owning phase |
| --- | --- | --- | --- | --- |
| D01 | Safety / immediate | Select matches, Escape, then Jump to End of Line. `AbstractLogView::maxLineLength` dereferences `max_element` on an empty selection. | Define empty-selection navigation as a safe no-op; protect other empty callers without fabricating a selected row. | P0 |
| D02 | Consistency / merge blocker | Insert result rows before a selected row, or change a row's text without changing row count. `continueMatchSelection` primarily checks shrinking counts; `Selection` rereads current text using old coordinates. | Stable row identity, fixed readable content, and atomic publication; no silent retargeting or mixed-version copying. | P2-P4 |
| D03 | Readiness / merge blocker | After one completed search, start a same-generation incremental update and select before it completes or after it is stopped. `matchSearchStates_` records generation, while the worker advances a separate operation ID. | Authoritative readiness for the requested result version/cutoff, not a remembered completed query generation. | P3 |
| D04 | Text consistency / merge blocker | Select ERROR from an ANSI Plain line, then switch to Strip/Render. `CrawlerWidget::applyConfiguration` changes text interpretation without an equivalent selection revision contract. | Format revision invalidates pending and active match selection before stale coordinates can be used. | P1-P4 |
| D05 | Coordinate correctness / merge blocker | Quick Find ERROR in a live row containing a leading Tab. `StreamingLogData::doGetExpandedLineString` returns unexpanded text although the selection consumer assumes expanded columns. | Shared normalized matching text and explicit display mapping across all sources. | P1, P3 |
| D06 | Responsiveness | A long line with many short matches creates many spans. `composeLineHighlights` scans every span for every boundary interval; selection extraction being asynchronous does not bound paint work. | Separate semantic fragments from paint geometry; clip early and use bounded/scalable composition while preserving z-order. | P6 |
| D07 | Responsiveness / lifetime | Complete a large selection and Copy. `finishMatchSelection` hides progress before synchronous `Selection::getSelectedText` rereads and concatenates source rows. | Cancellable serialization from owned snapshot data, no source reread, and a small UI-thread publication step. | P4 |
| D08 | Routing | Use Quick Find in folder pane A, leave it, switch to B, focus a toolbar, then Copy/Select All. `FolderCrawlerWidget::activeView` can fall back to stale `qfSavedFocus_`. | An explicit command context; saved Quick Find origin is valid only for that interaction and a live intended target. | P5 |
| D09 | Selection capability | Select two ERROR fragments, then Find Next Selected, Replace Search, Add to Search, or Exclude. `isPortion()` admits a multi-fragment newline-joined operand into line-based search. | Explicit selection capabilities; do not treat multiple fragments as one contiguous search operand. Audit all `isPortion` consumers. | P5 |
| D10 | Focus ownership | Start a large operation, focus an input and type without submitting; completion calls `setFocus()` on the log view. | Background completion preserves subsequent user focus. | P5 |
| D11 | Clipboard platform policy | Ordinary MainWindow Copy uses `sendTextToClipboard(text, true)`; deferred Copy uses the view path with the default policy. X11 primary selection depends on timing. | One clipboard sink and explicit command policy shared by immediate and deferred Copy. | P4-P5 |
| D12 | Semantic migration | `(?:prefix)(wanted)` yields `wanted` in Filter selection but `prefixwanted` in Quick Find. Text spaces also differ. | One query/occurrence/projection contract, with differences only in explicit options or input scope. | P1, P5 |
| D13 | Architecture debt | Folder selection calls `expandAll` and changes visibility before scanning view rows. Extraction depends on presentation side effects. | Enumerate canonical result rows; optional reveal is a separate presentation action, not a prerequisite for correctness. | P3, P5 |

D06 can expose a pre-existing compositor limitation; do not describe it as
necessarily introduced by match selection. Likewise, distinguish newly exposed
empty-selection paths from the history of the unsafe navigation helper.

### Implementation disposition before final acceptance

This checkpoint table preserves the qualifications that drove final acceptance.
All D01-D13 repairs subsequently passed the frozen-tree checks recorded at the end
of Section 8. Section 6 maps scenarios to tests; environment limits remain explicit.

| ID | Current repair and evidence | Remaining acceptance qualification |
| --- | --- | --- |
| D01 | Empty-selection guard and real End-of-Line actions after Escape, empty batch projection and zero-fragment navigation pass (`[empty-navigation]`). | Final frozen-tree platform rerun remains in P7. |
| D02 | Indexed/capture/folder/composite snapshots, stable identities, immutable captured payload and synchronous layout remapping are checkpoint-green. Latest Quick Find restoration also passed its focused checkpoint. | Final-tree broad/sanitizer matrix remains pending. |
| D03 | `CompletedSearchResult` execution identity and `LogFilteredData::isSearchComplete()` replace crawler completion bookkeeping; pending/stopped Matches are unavailable while PresentedRows remain readable. | Re-gated Stop fixture requires the final sanitizer rerun; no sanitizer defect was reported at the earlier review checkpoint. |
| D04 | Source interpretation notifications invalidate affected work; retained decoding is frozen. Codec/query/source restoration checks are checkpoint-green. | Final integrated platform reruns remain pending. |
| D05 | Unexpanded normalized matching text and `MatchTextColumns` provide shared Tab/Unicode display mapping, including live and header-gapped navigation. | Cross-platform acceptance is not inferred from native tests. |
| D06 | Event sweep, bounded wrapping probes, shared immutable frame layout and viewport-window drawing have deterministic work proofs. Dense wrapped paint improved from 22.539 s to 84.071 ms; selection/copy was 193 ms. | Frozen-tree rerun remains in P7; no universal 33 ms paint budget is claimed. |
| D07 | Worker extraction and cancellation-aware serialization use shared owned payload; Copy performs no source-text reread. Formatting changes during Copy are revalidated before publication. | Final lifecycle/sanitizer acceptance remains pending. |
| D08 | Current-view routing and interaction-scoped Quick Find origins are checkpoint-green in both owners; source replacement recreates and guards the finder. | Full close/switch delivery matrix remains a P7 acceptance concern. |
| D09 | Explicit search-operand capability replaces ambiguous command checks; multi-fragment payload is not a newline-composed operand. Selection/label/geometry consumers retain their distinct semantics. | Final-tree regression rerun remains pending. |
| D10 | Actual typed text and input focus survive extraction and gated serialization, including obsolete-format retry. | Final frozen-tree regression remains in P7. |
| D11 | Immediate/deferred/direct Copy shares GUI-thread publication and explicit PRIMARY policy. Native command tests pass. | Real X11 PRIMARY and Windows acceptance remain unverified. |
| D12 | Shared query construction, normalized text, occurrence anchors and captures-or-full projection are checkpoint-green. Uniform capture semantics repair numeric references and conditionals; no suppression heuristic remains. | Final platform/backend reruns remain pending. |
| D13 | Canonical Matches extraction does not reveal collapsed or Marks-only results. Frozen Quick Find scope survives later layout expansion; owner layout/Copy tests are checkpoint-green. | Final-tree acceptance remains pending. |

## 3. Pipeline and ownership

```text
UI command + explicit interaction context
    |
    v
[S0] Resolve QuerySpec, ScopeSpec, target, and copy intent
    |
    v
[S1] Check authoritative readiness and acquire a readable snapshot
    |
    v
[S2] Enumerate stable rows and apply row eligibility
    |
    v
[S3] Produce normalized search text and display-coordinate mapping
    |
    v
[S4] Enumerate occurrences under the shared query semantics
    |
    v
[S5] Project fragments and normalize overlaps within each occurrence
    |
    v
[S6] Build and atomically publish an immutable MatchSnapshot
    |                                      |
    v                                      v
[S7a] Resolve RowId to view geometry    [S7b] Serialize snapshot fragments
    |                                      |
    v                                      v
Merge / clip / compose / paint         Platform clipboard sink
```

Cancellation, progress, resource limits, operation identity, and lifetime checks
belong to the executor across stages. They are not extra per-document matching
branches. Stages are responsibility boundaries, not a requirement for a class,
virtual interface, or allocation at every arrow.

| Stage | Owner and output | Must not do |
| --- | --- | --- |
| S0 | UI/document command adapter; immutable query, scope, target, and intent | Decide capture semantics from whether the caller is Filter or Quick Find |
| S1 | Search/result owner plus source adapter; completed scope version and stable read lease | Infer completeness from row count or query generation alone |
| S2 | Canonical row source and eligibility policy; ordered RowIds with roles | Derive extraction scope from painted rows, folder headers, or forced expansion |
| S3 | Shared text-normalization contract; normalized text plus explicit coordinate map | Match expanded Tabs in only one entry point, or use stale ANSI/encoding interpretation |
| S4-S5 | Shared matching core; occurrences and projected spans | Read QWidget focus, mutate visibility, or write the clipboard |
| S6 | Executor/snapshot builder; owned immutable content and selection metadata | Publish partial success, accept stale callbacks, or retain only mutable view coordinates |
| S7a | View projection and renderer | Change clipboard fragment boundaries to optimize paint |
| S7b | Serializer plus platform sink | Read the mutable source again or run an unbounded read/assembly loop on the UI thread |

### Shared concepts, not mandated C++ API names

- `QuerySpec`: pattern, literal/regex mode, case options, and explicit projection.
  Backend choice is an execution policy, not a semantic change.
- `ScopeSpec`: source/result identity, source range, eligible row roles, requested
  cutoff, and deterministic ordering. Filter versus Quick Find differences live here.
- `RowId`: source identity plus source incarnation and stable record identity.
  A path alone is not identity; a displayed row number is never identity.
- `ReadableSnapshot`: fixed ordered rows and a content-stability guarantee,
  including the mutable final line's byte/record boundary and normalization version.
- `MatchOccurrence`: stable row identity, overall match/navigation anchor,
  occurrence ordering, and projected spans. Use explicit text-space types/metadata.
- `MatchSnapshot`: immutable fragment content (or offsets into immutable owned
  text blocks), identities, ordering, and geometry metadata. Copy cannot call back
  into the current log source to reconstruct its payload.
- `SelectionCapabilities`: distinguish empty, whole-row, one contiguous fragment,
  and multiple fragments. Clipboard support is not search-operand support.

The conceptual names above are contracts, not missing classes to introduce.
The implementation uses the following existing/shared types:

| Concept | Implemented representation / owner |
| --- | --- |
| Query and projection | `RegularExpressionPattern::toRegularExpression()`, `QuickFindPattern`, `MatchProjection`, `MatchOccurrence` and `projectMatchOccurrence()`; all query consumers preserve capture semantics. |
| Scope and readable version | `SnapshotScope::{Matches, PresentedRows}`, `LogSnapshot` and `SnapshotRow`; canonical membership is separate from frozen presented-row positions. |
| Stable row identity | `LogRowId`, `rowForIdentity()`, `presentedRowForIdentity()` and `containsRowIdentity()`; hidden rows are not treated as destroyed rows. |
| Source ownership | `IndexedContentSnapshot`, `SearchableLogData::ReadSnapshot`, CaptureStore leases, folder `ScanArena`/`ScannedFileContent`, and concatenated child snapshots. |
| Completed Filter readiness | `CompletedSearchResult` plus worker generation/operation/request identities and `LogFilteredData::isSearchComplete()`. |
| Text coordinates | Shared snapshot decoding/normalization and `MatchTextColumns`; raw UTF-16 offsets are distinct from Tab-expanded display columns. |
| Extraction and publication | `MatchSelectionJob` / `MatchSelectionStep`, with the shared AbstractLogView orchestration in `matchselection.cpp` owning cancellation, generation checks and GUI publication. |
| Immutable match result | `Selection::CapturedPortion` and shared immutable `Selection` payload/layout objects; stable occurrence anchors are retained even with no fragments. A separate `MatchSnapshot` class is unnecessary. |
| Command capability and lifecycle | `Selection::canUseAsSearchOperand()`, owner `canUseSelectionAsSearchOperand()`, `hasMatchSelection()` and `refreshMatchSelection()`. Quick Find results also carry `rowIdentity` and `requestId`. |

Matching/data code does not own widgets or clipboard APIs. Source acquisition and
command routing remain on the owner thread; matching, retained-row reads and
serialization use immutable worker inputs. The view resolves current geometry and
publishes through the platform clipboard sink.

### Search backend compatibility

Filter can retain its optimized bulk row screening; Quick Find can retain its
directional traversal. Both must use the same semantic query and occurrence
contract. Optimized screening must not discard rows accepted by the authoritative
matcher. Unsupported expressions must fall back without changing results.

Build differential tests for accelerated and Qt paths. Sharing a small capture
helper while navigation, highlights, and selection still interpret text differently
is not sufficient to close D12.

## 4. Snapshot, revisions, and operation lifecycle

### Content guarantees

Recording a row count, file size, file handle, watcher event, or revision number
alone does **not** make mutable file bytes immutable. Saving fragment text while
scanning also does not prove that all rows came from the same accepted version.

The implemented source strategies are:

- **Live:** reuse immutable CaptureStore payload/file leases for committed records.
  Inspection on 2026-10-27 established that `partialLine_` is uncommitted ingress,
  not a visible/searchable row: do not expose or finalize it to select matches.
  `finishInput()` commits and seals an unterminated record; later input is a new
  record. Preserve that boundary and distinguish a committed empty record from
  no record. A line number reused after clear/reconnect needs a new incarnation.
- **File:** retain the actual indexed bytes with immutable boundary metadata and
  bind search/extraction to that indexed content version. This is the portable
  strategy selected after the 2026-10-27 source audit: no native immutable file
  snapshot API exists in the current PAL. It guarantees consistency with the bytes
  indexed, not that an uncooperative external file had one atomic wall-clock state
  during initial indexing. An open descriptor does not protect against in-place
  writes. Future native snapshot optimizations belong in the platform abstraction
  and must preserve the same indexed-version contract.
- **Folder:** combine the fixed result membership/order with each contributing
  source's stability guarantee. Headers are layout objects, not snapshot records.

The command records its requested cutoff and query at acceptance. Acquisition can
be asynchronous, but must not silently substitute a later version or extend that
cutoff. If the source cannot supply the required guarantee, abort/reject with a
clear reason instead of declaring a mixed-version result complete. Do not release
P2 with ordinary supported sources permanently falling into a nonfunctional stub;
source coverage and acquisition cost are explicit phase gates.

The retention policy below bounds resident source storage and scopes leases to
owners/readers. Exhaustion must fail cleanly, not silently truncate selection.
Cancellation, failure, invalidation, replacement and teardown release their owned
leases; an in-flight reader keeps its immutable content until it finishes.
Selected-output-proportional memory and non-preemptible regex calls are declared
policies, not claims of a result-spill cap or forcible regex interruption.

#### Implemented retention policy

- **Indexed files:** per indexed incarnation, byte storage has an 8 MiB resident
  capacity budget and real line boundaries have a separate 1 MiB budget. Each
  store spills to its own private temporary file, for at most two spill handles.
  For N retained bytes and L real boundaries, spill storage is approximately
  N + 8L bytes. Append versions share append-only storage; old incarnations stay
  alive only while leased. No native clone/reflink or lazy-retention optimization
  is claimed.
- **Folder scans:** one scan-wide payload arena and one scan-wide packed boundary
  arena use the same 8 MiB + 1 MiB capacity budgets and at most two spill handles,
  not one file descriptor per scanned member. Streaming scanners stage at most
  8,192 boundary offsets (64 KiB) per active scanner; the fast path reuses its
  bounded-file boundary vector. Per-member span maps grow by interleaved batches,
  not by source line. Row lookup reads at most two offsets plus the requested
  record. Both arenas are released after the last scan/member/snapshot lease.
- **Live capture:** reuse the existing CaptureStore payload and spill leases;
  do not create a duplicate full-history spool. Published row-offset versions are
  immutable; appending clones the active published index before mutation under
  the store mutex. Uncommitted ingress is excluded.
- **Selection:** immutable shared payload/layout handles make copying an accepted
  Selection value constant-size. Fragment payload and the final clipboard string
  remain proportional to selected output; no result-spill implementation or
  absolute output-size cap is claimed. Worker serialization checks cancellation
  per fragment and within long UTF-16 chunks.

These capacity figures exclude result/layout metadata, member/span descriptors,
active parser/decoder work buffers, returned line strings and selected output.
Indexed content and boundary-store failures are explicit failed operations, not
partially successful extraction. Resource regressions cover forced spills,
interleaving, boundary-batch transitions, injected storage failure and last-reader
cleanup. Cancellation cannot interrupt a regex engine call already in progress.

### Separate revision domains

| Identity/version | Meaning |
| --- | --- |
| Query generation | Different search criteria or an explicit abandonment of the old query |
| Search operation ID | A particular execution/update within that query generation |
| Result version / completed watermark | Exact result membership and source extent proven complete |
| Source incarnation / content version | Replacement, mutation, clear/reconnect, and stable readable content |
| Text-format revision | Encoding, ANSI interpretation, and other changes to searchable text |
| View-layout revision | Mapping RowIds to rows/fragments on screen |
| Selection operation ID | One extraction request and its associated deferred Copy intent |

Do not bump query generation for every append to work around D03: incremental
search intentionally retains it. Transport existing execution identity and
completion evidence across the owner boundary instead of duplicating status maps.
A growing live source need only be complete through the requested fixed cutoff;
"all matches" must not wait forever for global input quiescence. Later work beyond
that cutoff does not block publication. Conversely, do not silently substitute an
older completed prefix when the accepted command requested a newer cutoff; reject
or explicitly wait for the requested extent with cancellable progress.

### Default lifecycle policy

| Event | Pending extraction/serialization | Published active selection |
| --- | --- | --- |
| Append beyond fixed cutoff | Continue against the accepted snapshot | Remains unchanged; no incremental selection |
| Layout-only reorder, fold, context/header insertion | Continue by RowId; no reselection from display indices | Remap by RowId; hidden rows may remain selected without being painted |
| Query, source incarnation, or searchable-text format changes | Cancel and discard deferred Copy | Clear active match selection; do not silently reinterpret it |
| Destructive clear/truncate/eviction affecting selected identities | Cancel/invalidate affected operation | Clear active selection under this plan; owned payload may live until readers exit, but must not be published by a stale Copy completion |
| View hidden, target replaced, document closed | Preserve existing cancellation intent; cancel pending work and disallow focus/clipboard callbacks | Hiding alone need not destroy a stable completed selection; replacement/close invalidates it |
| Explicit ordinary selection or a newer match-selection command | Cancel predecessor and its deferred Copy | Replace according to the new user action |
| Read/regex/resource/serialization failure | Report failure, publish no partial result, preserve clipboard | Do not replace an unrelated valid selection with partial work |

These are explicit defaults, not a proposal for an invisible persistent export
buffer after Clear Log. Pure presentation changes are different from content loss.
If retention/reveal behavior must change during implementation, record the new
contract and regression before changing code.

Logical states:

```text
Idle -> Acquiring -> Extracting -> Ready -> Serializing -> ClipboardPublished
                   \             \           \
                    +-------------+-----------+--> Cancelled / Invalidated / Failed
```

No terminal callback may revive an obsolete operation. Copy while extraction is
pending attaches intent to that operation; Copy after Ready serializes that same
snapshot. Atomic publication must revalidate target identity and operation token.
The executor must contain exceptions and marshal UI effects to the owning thread.
A timer budget around a regex call does not make the regex call itself preemptible;
keep expensive work off the GUI thread and document cancellation boundaries.

## 5. Phased task checklist

Dependencies:

```text
P0 -> P1 -> P2 -> P3 -> P4 -> P5 -> P7
                         \-> P6 ----/
```

P6 may be developed independently once snapshot/geometry interfaces are stable.
Local test execution across trees must still be coordinated and serialized where
shared roots require it. Phase labels are new to this follow-up, not the completed
phase numbers of the original feature implementation.

Checked P0-P6 tasks below have implemented code and referenced native checkpoint
coverage. A checked task is not final-tree/platform acceptance. Explicit coverage
qualifications are recorded alongside their evidence; source/output retention
policies are those in Section 4. The final verification section records the
completed local scope and the explicitly unrun environment variants.

### P0 - Safety baseline and deterministic reproductions

- [x] P0.1 Recheck the defect anchors at the working revision. Record existing
  test coverage versus missing regressions for D01-D13.
- [x] P0.2 Reproduce D01 through the real navigation action after Escape and after
  a selection with zero projected fragments. Implement the safe empty behavior.
  The real End-of-Line action passes after Escape, empty batch projection and a
  zero-fragment Quick Find occurrence (`[empty-navigation]`).
- [x] P0.3 Add/reuse test seams for controlled source mutation, result publication,
  layout remapping, operation completion, and clipboard publication. Prefer
  injected adapters/observations over production test-only timing switches.
- [x] P0.4 Establish passing characterization for ordinary row selection, Quick
  Find navigation, highlighter predicates, and existing Boolean escaping.

Exit: D01 regression is RED then GREEN; unrelated selection/navigation behavior
is preserved. Missing tests are tracked, not falsely marked as passing coverage.

### P1 - Shared query, text, occurrence, and projection semantics

- [x] P1.1 Share query construction through `RegularExpressionPattern` and the
  `MatchOccurrence` representation, retaining the full-match anchor independently
  from projected fragments. Preserve capture numbering in predicates as well as
  extraction; `CaptureMode`/`DontCaptureOption` policy was removed.
- [x] P1.2 Centralize matching-text normalization and explicit display mapping.
  Match decoded/ANSI-normalized text, not Tab-expanded display text. Define offsets
  in the same units as the shared matcher and handle Unicode boundaries correctly.
- [x] P1.3 Share captures-or-full projection across Filter and Quick Find,
  preserving overlap, ordering, zero-width, and predicate semantics.
- [x] P1.4 Make highlighting, directional navigation, and batch extraction consume
  the common occurrence contract; remove conflicting entry-point policies as
  consumers migrate. Scope/traversal strategies remain explicit.
- [x] P1.5 Add backend parity tests for supported expressions and fallback cases;
  do not sacrifice Vectorscan's proven bulk-search optimization.

Exit: identical query/text inputs produce identical occurrences and fragment
payloads through both entry points. Navigation advances by occurrence, including
zero-width cases, without loops or skipped later hits. No Qt-version branches are
added to business logic or tests.

### P2 - Stable source identity and readable snapshot ownership

- [x] P2.1 Specify RowId/source incarnation, accepted cutoff, content and format
  versions, and the lease/mutation contract. Prove that layout indices cannot
  substitute for identity.
- [x] P2.2 Implement the file snapshot/read strategy; test in-place same-size
  rewriting, replacement at the same path, and truncation during acquisition/read.
- [x] P2.3 Freeze committed live records using existing storage leases, without
  finalizing partial ingress. Test split input, sealed empty/unterminated records,
  retention, clear/new incarnation and owner-independent reads.
- [x] P2.4 Define owned immutable payload blocks and measured memory/spill limits.
  No mutable-source read may be necessary to serialize completed fragments.
- [x] P2.5 Verify terminal-path release: last-reader cleanup, typed read/acquisition
  failures, abandoned folder generations, cancellation and owner destruction.
  Snapshot acquisition is synchronous on the owner thread: it returns a retained
  lease or fails before dispatch; there is no independently cancellable acquisition
  task. `[match-lifetime]` gates dispatched extraction/serialization and verifies
  hide/destruction, no late clipboard publication and expiry of every observed
  snapshot lease. Source-specific ownership tests complement this shared executor
  proof; final sanitizer acceptance remains in P7.

Exit: D02's content-stability component is proven for file and live adapters,
including same-row-count mutations. A handle or a watcher-based heuristic alone
is not accepted as proof. Record resource costs and any unsupported source capability.

### P3 - Canonical scopes, row eligibility, and authoritative readiness

- [x] P3.1 Provide canonical ordered row enumeration for file, folder, and live
  scopes. Keep eligibility based on roles/copyability, not document-type switches.
- [x] P3.2 Enumerate Filter matches independently of collapsed groups or
  Marks-only presentation. Preserve fixed Quick Find scope semantics separately.
- [x] P3.3 Replace per-widget remembered completion state with authoritative
  query/operation/result-cutoff readiness. Reject stopped/incomplete snapshots;
  allow a completed fixed prefix even while later live data arrives.
- [x] P3.4 Test same-generation update start/stop/completion and late terminal
  signals, including result rows inserted before marked/context rows.
- [x] P3.5 Freeze folder membership/order with source snapshot leases. Confirm
  noncopyable headings cannot enter the matching or serialization stages.

Exit: D03 and D13's extraction coupling are closed. Folder folding does not alter
Filter payload; scope differences, not matching differences, explain Quick Find
versus Filter candidate rows. D02 remapping tests pass without forced expansion.

### P4 - Cancellable executor, immutable selection, and copy serialization

- [x] P4.1 Move extraction orchestration out of view-specific branches. Use one
  executor with typed scope/query inputs, cancellation, progress, and lifetime ownership.
- [x] P4.2 Build immutable captured `Selection` payloads through `MatchSelectionJob`;
  validate operation tokens and publish only complete results. Replace
  row-count-only safety checks with source snapshot/identity contracts.
- [x] P4.3 Serialize owned fragments asynchronously or in bounded cancellable
  work units; keep progress visible through serialization. Source read counters
  must remain zero during Copy of a completed snapshot.
- [x] P4.4 Route immediate Copy, deferred Copy, and Copy Matches through one
  serialization/publication contract. Keep native clipboard calls in the platform
  sink and on the required thread, with explicit primary-selection policy.
- [x] P4.5 Apply the lifecycle table centrally. Ensure cancel/error/close/new-query
  paths cannot publish partial data, update the clipboard late, or leak leases.
- [x] P4.6 Preserve teardown ordering: stop/join workers before their source owners
  are destroyed. Do not introduce normal UI-thread joins for routine appends.

Exit: D02/D04/D07 and the shared part of D11 are closed. Copy always reflects
snapshot content, even if mutable-source reads would now fail. Cancellation works
inside long rows and during serialization; an old Copy intent never migrates to a
new operation. Resource exhaustion is an explicit failure, not truncated success.

### P5 - Command contexts, selection capabilities, and presentation integration

- [x] P5.1 Resolve a command context once: document, target view/scope, Quick Find
  origin lifetime, query, copy policy, and operation ID. Retire stale origins on
  interaction exit/target changes; never let hidden pane A capture pane B's actions.
- [x] P5.2 Remove completion-time focus stealing. Test typing into another input
  while extraction or serialization completes. Actual key input and focus survive
  extraction completion and gated serialization, including superseded formatting
  (`[match-executor]`, `[match-copy-format]`).
- [x] P5.3 Replace ambiguous `isPortion()` checks with capabilities at every consumer:
  Find Next/Previous Selected, Replace/Add/Exclude Search, labels, menus, hit testing,
  crop, keyboard movement, mouse replacement, and copy-with-line-number actions.
  Default: only one non-empty contiguous fragment is a search operand; disable
  multi-fragment composition rather than inventing implicit Boolean OR semantics.
- [x] P5.4 Treat reveal/expand as an optional presentation action after scope
  capture, never as a requirement for extraction. Preserve intentional visible
  reveal behavior unless a separately recorded UX decision changes it.
- [x] P5.5 Wire both Filter and Quick Find to the shared pipeline in single-file,
  folder, and live owners. Remove obsolete completion maps and scattered special
  invalidation paths once the authoritative contracts replace them.
- [x] P5.6 Verify configurable Alt+Return/Alt+Enter and existing Copy bindings,
  Return/keypad Enter, focus routing, and X11 primary-selection parity through
  the platform/shortcut abstractions.
  Shortcut registration/configured dispatch and native routing pass. The separate
  real xcb test passed Clipboard/PRIMARY parity; native Windows runtime is not claimed.
- [x] P5.7 Update MATCH_SELECTION.md and user documentation for unified projection,
  text-space changes, snapshot lifetime, and unsupported extraction modes.

Exit: D08-D12 are closed, D13 reveal is decoupled, and all affected existing
selection consumers have explicit supported shapes. No temporary dual semantic
path is left as the default implementation.

### P6 - Paint geometry and scalable highlight composition

- [x] P6.1 Derive paint geometry from immutable fragments and current RowId layout.
  Merge overlapping/adjacent same-style selection intervals without changing the
  semantic fragment list or clipboard separators.
- [x] P6.2 Clip to visible wrapped/unwrapped segments before expensive composition;
  preserve Tab/Unicode mapping and selection across wrapped rows.
- [x] P6.3 Remove the boundary-times-all-spans scan for dense rows. Use a verified
  event sweep or equivalent scalable algorithm; define equal-layer tie order and
  preserve the existing highlight blend/z-order contract.
- [x] P6.4 Add deterministic composition/work-count tests plus local-only
  best-of-N paint/selection/copy performance measurements. Cover dense single rows,
  many rows, overlapping occurrences, and off-viewport matches separately.
  Sweep/viewport/append, bounded prefix shaping, actual text submission and cache
  size tests pass. Optimized 40,000-fragment checkpoint: 22.912 ms unwrapped,
  84.071 ms wrapped, 193 ms selection/copy; final frozen-tree rerun remains in P7.

Exit: D06 is closed with evidence for painting, not only extraction. Copy output
is unchanged by geometry merging. Existing refresh cadence and active/inactive
presentation policy are unchanged.

### P7 - Cross-scenario acceptance, cleanup, and documentation handoff

- [x] P7.1 Run the acceptance matrix below and full relevant unit/UI suites on the
  final integrated revision, not only intermediate per-phase binaries.
- [x] P7.2 Run macOS Qt5/Qt6 coverage, native ASan/UBSan, and local Docker parity
  using the current locked CI environments. Run supported Windows checks and a
  real X11 clipboard test; mocks alone do not verify the primary selection.
- [x] P7.3 Run changed-line clang-tidy, standalone-header checks, locked-version
  cppcheck, repository hygiene, platform, and test-determinism checks. Do not broaden
  suppressions or add platform conditionals in business/tests to hide failures.
- [x] P7.4 Run relevant optimized local performance gates and package/Cocoa smoke
  where affected. Record unrun environments explicitly instead of claiming parity.
- [x] P7.5 Remove unused compatibility wrappers, duplicated policies, obsolete state,
  and stale tests. Update architecture references to describe shipped code, and
  retain this document as a completed execution record with commit/evidence links.

Exit: every D01-D13 entry has a regression or appropriate architecture/performance
proof and an implementation commit. No unresolved merge blocker is hidden behind
an overall green suite. User-visible semantic changes are documented.

## 6. Required acceptance matrix

Use real source adapters for integration tests, and deterministic fake snapshots
for mutation, cancellation, failure, and resource boundaries. Execute every
applicable case through both Filter and Quick Find; assert equal fragments only
when query options and candidate scope are equal.

| Test ID | Required scenario and observable result | Closes |
| --- | --- | --- |
| R01 | Empty source, no projected fragments, and Escape followed by end-of-line navigation: no crash or fabricated selection. | D01 |
| R02 | Literal/regex, case options, captures/no captures, noncapturing groups, lookarounds, nested/optional/empty captures, repeated and overlapping occurrences: identical core results across entry points/backends. | D12 |
| R03 | Tabs, Unicode including non-BMP text, ANSI modes and encoding changes: one text-space contract; display geometry and copied text agree. | D04, D05, D12 |
| R04 | Insert/reorder rows before selected data without shrinking row count; add headers/context, collapse/expand groups: identity and Filter payload remain stable. | D02, D13 |
| R05 | Same-size file rewrite, same-path replacement, unterminated tail append, split UTF-8 continuation, rolling eviction, clear/reconnect: accepted content is stable or operation explicitly invalidates, never retargets. | D02 |
| R06 | Completed generation followed by pending/stopped/completed same-generation update; old completion arrives late: readiness matches the requested cutoff and execution identity. | D03 |
| R07 | Mark-only, context-only, Match-and-Mark and noncopyable headers: Filter eligibility is exact; Quick Find scope differences are explicit and documented. | D13 |
| R08 | Copy before/after completion, Copy Matches, cancellation, query replacement, read failure, resource exhaustion, close and late callbacks: same payload/policy or no publication, never partial success. | D02, D07, D11 |
| R09 | Completed snapshot copied after mutable-source access is made unavailable: zero new source reads. Cancel during serialization: no clipboard change. | D07 |
| R10 | Quick Find in pane A, leave, switch to B, focus toolbar, invoke ordinary actions; also close A before completion: correct target and safe lifetime. | D08 |
| R11 | Type into another input during work: completion preserves that focus and typed text. | D10 |
| R12 | One contiguous fragment versus multiple fragments/rows: menus, search-operand actions, labels, navigation, cropping and replacement follow explicit capabilities. | D09 |
| R13 | Dense long row, wrapped row, overlapping occurrences and off-viewport spans: bounded/scalable paint work with unchanged colors and clipboard entries. | D06 |
| R14 | Linux X11 Clipboard/Selection, macOS and Windows Copy, Return/keypad Enter, remapped shortcuts: immediate/deferred paths have identical intended platform effects. | D11 |
| R15 | Append continuously after acceptance: fixed cutoff finishes without waiting for global quiescence; no new fragments join the selection and capture continues. | D02, D03 |
| R16 | Close/hide/switch while acquiring, extracting, serializing or delivering completion: no UAF, stale focus, late clipboard effect, or leaked source lease. | D02, D07, D08, D10 |

### R-to-test evidence map

These are test-entry anchors; the qualification column preserves the pre-final
checkpoint boundaries, superseded by the frozen-tree results in Section 8. This
is not a claim that every matrix permutation has run on every platform. Unit and UI executables are distinct; hidden X11/performance cases
must be requested explicitly and are not covered by an ordinary full CTest pass.

| Requirement | Existing evidence anchors | Current qualification |
| --- | --- | --- |
| R01 | `[empty-navigation]`, `[selection-navigation-bounds]`, `[selection-stable-navigation]`, empty-source cases in `[match-selection]` | Native checkpoint-green, including the real End-of-Line action after Escape, empty projection and a zero-fragment Quick Find hit. |
| R02 | `[match-projection]`, `[query-options]`, `[query-capture-references]`, `[quickfind-occurrence]`, `[filtered-backend-parity]`, `[patternmatcher]`, `[folder-normalization]` | Native checkpoint-green, including reference/conditional semantics and backend fallbacks. |
| R03 | `[match-projection]`, `[raw-portion]`, `[source-snapshot]`, `[live-read-snapshot-integration]`, `[folder-framing]`, `[file-interpretation]`, `[filtered-interpretation]`, `[quickfind-snapshot]` | Native checkpoint-green; final platform reruns pending. |
| R04 | `[selection-row-remap]`, `[owner-layout]`, `[owner-layout-copy]`, `[quickfind-layout]`, `[selection-stable-navigation]`, `[quickfind-restoration]` | Payload, geometry and latest saved-selection restoration checkpoints pass. |
| R05 | `[indexedcontent][indexing]`, `[live-read-snapshot]`, `[live-read-snapshot-integration]`, `[folder-snapshot]`, `[concatenated-snapshot]` | Real retained-content mutation/lease coverage passes; no atomic external-filesystem snapshot claim. |
| R06 | `[filtered-snapshot]`, `[filtered-cancellation]`, `[filtered-presented-readiness]`, `[owner-readiness]`, `[match-selection-stop]` | Native checkpoint-green; corrected gated Stop fixture still needs final ASan rerun. |
| R07 | Filter eligibility cases in `[match-selection]`, `[filtered-snapshot]`, `[folder-snapshot]`, `[folder-inverse]`, `[owner-canonical-scope]`, `[quickfind-snapshot]` | Canonical Matches and header-gapped PresentedRows are separately covered. |
| R08 | `[match-executor]`, `[match-executor-error]`, `[match-copy-format]`, `[filtered-worker-failure]`, `[indexedcontent]`, `[folder-retention-failure]`, MainWindow cases in `[match-selection]` | Failure/cancellation/publication components pass; final integrated lifecycle acceptance pending. |
| R09 | `[captured-portion]`, `[selection-serialization]`, `[selection-copy-isolation]`, `[match-executor]` | Owned payload, zero source-text rereads and chunk cancellation are checkpoint-green. |
| R10 | `[owner-routing]`, `[match-source-switch]`, `[quickfind-request]`, `[quickfind-restoration]`, MainWindow match-command routing | Native checkpoint-green; not an exhaustive platform close/switch delivery matrix. |
| R11 | Typed-input/focus cases in `[match-executor]` and gated `[match-copy-format]` | Focus and actual typed text survive extraction and serialization, including obsolete-format retry. |
| R12 | `[selection-capability]`, `[match-operand-actions]`, `[selection]`, existing crawler/folder label and ordinary-selection UI tests | Native checkpoint-green; operand capability is distinct from painting/label capability. |
| R13 | `[highlighter][compose]`, `[highlight-viewport]`, `[match-layout-work]`, wrapped selection cases; hidden `[match-paint-perf]` / `[match-selection-perf]` | Deterministic work/behavior tests pass; optimized best-of-three results still need recording. |
| R14 | `shortcut_bindings_test.cpp` match-command case, configured shortcut dispatch in MainWindow, hidden `[match-selection-x11]` | Native registration/routing pass; real xcb PRIMARY, Windows and final platform runs remain pending. |
| R15 | Fixed-prefix and accepted-version gates in `[filtered-snapshot]`, live crawler one-shot test in `[match-selection]`, `[owner-layout-copy]` | Fixed-cutoff components pass; sustained append plus full command-path acceptance is not separately recorded. |
| R16 | `[match-lifetime]`, `[quickfind-snapshot]`, `[quickfind-request]`, `[match-source-switch]`, `[quickfind-restoration]`, `[folder-drain]`, `[folder-abandonment]`, `[concatenated-snapshot]`, `[filtered-worker-failure]` | Gated hide/destruction of dispatched extraction and serialization proves no late copy and expiry of all observed leases. Source-specific lifecycle/routing tests cover adapter variation; synchronous acquisition has no separately cancellable task. Final sanitizer run pending. |

Use signals, barriers, revision changes, operation tokens, and read/work counters
for correctness/liveness assertions. Arm observers before triggering work. Do not
use fixed sleeps or wall-clock performance assertions in normal CI. Local budgets
must use best/min/median sampling under the repository performance-gate mechanism.

## 7. Code and test entry points

| Responsibility | Starting points |
| --- | --- |
| Shared extraction executor and GUI publication | `src/ui/include/matchselectionjob.h`, `src/ui/src/matchselectionjob.cpp`, `src/ui/src/matchselection.cpp`, `src/ui/include/abstractlogview.h` |
| Selection storage/consumers | `src/ui/include/selection.h`, `src/ui/src/selection.cpp`, `src/ui/src/abstractlogview.cpp` |
| Shared projection and search semantics | `src/ui/include/matchprojection.h`, `src/ui/src/quickfindpattern.cpp`, `src/ui/src/quickfind.cpp`, `src/ui/src/highlighterset.cpp`, `src/regex/` |
| Query/result execution identity | `src/logdata/src/logfiltereddataworker.cpp`, `src/logdata/src/logfiltereddata.cpp` |
| Source snapshots and text | `src/logdata/include/logsnapshot.h`, `src/logdata/src/indexedcontent.cpp`, `src/logdata/src/folderscanarena.cpp`, `src/logdata/src/logdata.cpp`, `src/logdata/src/streaminglogdata.cpp`, `src/logdata/src/capturestore.cpp` |
| Scope and command adapters | `src/ui/src/crawlerwidget.cpp`, `src/ui/src/foldercrawlerwidget.cpp`, `src/ui/src/mainwindow.cpp`, `src/ui/src/quickfindwidget.cpp` |
| Painting | `src/ui/src/highlightcompose.cpp`, `src/ui/src/abstractlogview.cpp` |
| Platform policy | `src/utils/include/platform/platform_clipboard.h`, `src/utils/include/platform/platform_input.h`, shortcut registry under `src/settings/` |
| Core regression fixtures | `tests/unit/match_projection_test.cpp`, `tests/unit/match_selection_test.cpp`, `tests/unit/quickfind_snapshot_test.cpp`, `tests/unit/selection_test.cpp`, `tests/unit/indexedcontent_test.cpp`, `tests/unit/logfiltereddataworker_test.cpp`, `tests/unit/linehighlight_compose_test.cpp` |
| Integrated commands and sources | `tests/ui/crawlerwidget_test.cpp`, `tests/ui/foldercrawler_test.cpp`, `tests/ui/mainwindow_test.cpp` |

Preserve the Qt5/Qt6 container-index abstraction and C++17 compatibility. New
headers must be self-contained. Put OS/Qt-version variation in the existing
platform layer, not in test bodies or document-specific branches.

## 8. Evidence and execution log

### Historical baseline, not acceptance of this plan

At the feature plus portability revision, macOS Qt6 and locked Linux Docker
ASan/LSan unit/UI pairs each passed 1,080 / 242 cases. At `a5383e3d`, the independent
construction/parser cleanup passed 58 macOS cases / 880 assertions and 67 Docker
cases / 955 assertions; locked CI cppcheck 2.13 passed 135 translation units.
Host cppcheck 2.21 retained 41 pre-existing diagnostics after six were removed.
These results do not prove the new contracts or close any D01-D13 entry.

The previous 40,000-fragment extraction benchmark did not establish dense-row
paint scalability or slow-source copy responsiveness. Do not reuse it as the sole
acceptance evidence for P4/P6.

### Historical phase checkpoints (superseded by final verification)

| Phase | State | Commit(s) | RED / characterization evidence | GREEN and regression evidence | Remaining limitations |
| --- | --- | --- | --- | --- | --- |
| P0 | Safety repair checkpoint-green | Uncommitted | Real End-of-Line action reproduced SIGSEGV after selection clear | Empty-navigation/projection: 8 cases, 28 assertions; later native full suite passed | Separate zero-fragment End-of-Line action and final-tree acceptance remain unrecorded |
| P1 | Shared semantics implemented; checkpoint-green | Uncommitted | Projection, normalization, block-boundary, default option and capture-reference regressions reproduced differences | Shared construction/normalization/projection and backend parity pass; uniform capture semantics replaces the invalid suppression policy | Final cross-platform/backend acceptance pending |
| P2 | Snapshot ownership implemented; checkpoint-green | Uncommitted | Indexed mutation, live leases, folder framing/retention and row-lookup work reproduced gaps | File/live/folder/composite ownership and bounded source stores pass; concatenated checkpoint: 5 cases / 107 assertions | Latest storage-observer refinement and full terminal/platform matrix need final acceptance; declared output-memory policy is not an unimplemented spill requirement |
| P3 | Canonical scopes/readiness implemented; checkpoint-green | Uncommitted | Same-generation updates, completed interrupts, stale terminal tokens and presentation/completion coupling reproduced | Owner canonical scope/readiness and worker result/presentation regressions pass | Final sanitizer rerun includes corrected gated Stop fixture |
| P4 | Shared executor/copy lifecycle implemented; checkpoint-green | Uncommitted | Mutation, cancellation, empty diagnostic retry, changed Copy format, graph failure and saved-selection restoration reproduced | Executor/source checkpoint: 108 cases / 1,646 assertions; repaired worker failures and immutable payload tests pass; latest restoration batch: 69 cases / 1,059 assertions | Final broad/sanitizer lifecycle and real X11 acceptance pending |
| P5 | Owner integration implemented; checkpoint-green | Uncommitted | Focus stealing, invalid operands, stale routing, layout invalidation and lost anchors reproduced | Owner routing: 15 cases / 294 assertions; owner layout/Copy: 4 cases / 276 assertions; stable anchors and real reset-routing restoration pass | Typed-input-through-serialization case, real platform shortcut/clipboard coverage and final-tree run remain pending |
| P6 | Geometry/sweep implemented; checkpoint-green | Uncommitted | Dense compositor visited 32,768 spans for 128 inputs; offscreen spans created unnecessary events; append remapped every selected row | Differential sweep/work-count, viewport clipping, immutable paint geometry and append-proof tests pass | Hidden optimized best-of-three paint/selection/copy measurements need recorded results |
| P7 | Final acceptance pending | Uncommitted | Review REDs are retained below as historical evidence, not current defect statuses | Earlier native Qt6 CTest: 39/39; unit 1,232 / 55,865, UI 261 / 7,860, Vectorscan 14 / 68; locked cppcheck: 138 TUs passed | These precede newest restoration/storage-observer refinements; final native/sanitizer/Docker runs, Qt5/Windows/X11 evidence, remaining analyzer/package checks and commit reference pending |

### Integrated lifecycle checkpoints (2026-10-27, uncommitted)

These checkpoints used macOS Qt6, Vectorscan enabled, RelWithDebInfo without
sanitizers. They are intermediate evidence, not final cross-platform acceptance.

Build command: `cmake --build build_root --target klogg_tests klogg_itests -j 4`.
Executables ran with `QT_QPA_PLATFORM=offscreen` and separate checkpoint
`KLOGG_CAPTURE_COORDINATION_ROOT` / `KLOGG_PORTABLE_CONFIG_DIR` directories.

| Command after `build_root/output/` | Observed result | Interpretation |
| --- | --- | --- |
| `klogg_tests '[selection-stable-navigation],[match-executor-error],[match-layout-work],[indexed-line-ending],[folder-interpretation]'` | 13 cases: 7 passed, 6 failed; 65 assertions: 58 passed, 7 failed | Six new stable-anchor cases reproduced navigation/remapping defects. Empty-diagnostic failure termination, append work proof, one-CR framing and folder interpretation notifications passed. |
| `klogg_itests '[owner-layout],[owner-layout-copy],[match-layout-work]'` | 5 cases: 1 passed, 4 failed; 262 assertions: 244 passed, 18 failed | Native file preservation proof passed. Published owner selections lost payload on layout changes; folder pending Copy completed unsuccessfully. Crawler Copy initially failed fixture setup and required a corrected rerun before counting as behavioral RED. |
| `python3 scripts/lint_header_self_contained.py src/logdata/include/logsnapshot.h src/logdata/include/indexedcontent.h src/logdata/include/folderscanarena.h src/ui/include/matchselectionjob.h` | Passed | All four new headers compile independently. |
| `python3 scripts/lint_repo_hygiene.py` and `python3 scripts/lint_test_determinism.py` | Passed | Intermediate source/test checkpoint. |
| `python3 scripts/lint_platform_fragile.py` | Passed after the targeted refinements | Replaced reference-count-based COW mutation decisions with explicit published state, made capability checks backend-aware, and fixed six Qt5-ambiguous QStringList assignments. |

### Independent review follow-up

The integrated diff received an independent high-effort review. Its findings
require their own RED/GREEN evidence rather than inheriting the earlier passes:

| Review item | Regression / repair boundary | State |
| --- | --- | --- |
| Completed search interrupted by pane switching loses readiness | Worker cancellation distinguishes idle completed results from unfinished requests, including cache hits | Implemented; native checkpoint-green (`[filtered-cancellation]`) |
| Pending/stopped search cannot provide PresentedRows | Separate presentation snapshot availability from canonical Matches readiness | Implemented; native checkpoint-green (`[filtered-presented-readiness]`) |
| Marks beyond the previous search cutoff render blank | Current retained presentation source covers appended marks; canonical result membership stays frozen | Implemented; native checkpoint-green (`[filtered-presented-readiness]`) |
| Capture records lose a second terminal CR | Carry explicit framing state instead of normalizing ingress twice; cover disk reload and legacy codecs | Implemented; normalization/framing checkpoints green |
| Folder display override changes marked-row boundaries | Frozen scan framing owns row location; display override only decodes content | Implemented; `[folder-framing]` also passes in the latest 69-case checkpoint |
| Live copied line labels start at zero | One-based captured source labels, independent of zero-based stable row identity | Implemented; native checkpoint-green (`[live-snapshot-labels]`) |
| New numbered-copy intent is lost during serialization | Validate serialization policy before clipboard publication and rerun obsolete formatting | Implemented; native checkpoint-green (`[match-copy-format]`) |
| Every marked folder row rescans the file prefix | Shared bounded packed boundary storage; deterministic source-byte work counter | Implemented; `[folder-row-locator]` / `[folder-arena]` checkpoints green; latest observer refinement awaits final runtime window |
| Predicate capture suppression changes backreferences and conditionals | Uniform capture semantics; remove `CaptureMode`/`DontCaptureOption`, not heuristic reference parsing | Repository RED: three cases/nine failing assertions; repair is native checkpoint-green (`[query-capture-references]`) |
| Saved Quick Find selection restores stale geometry/content or loses a zero-fragment anchor | Remap and validate restoration against current source/query/interpretation; preserve anchors and real reset-routing order | Implemented; latest restoration and integration filter passes 69 cases / 1,059 assertions |

A separate worker audit identified exceptional TBB graph unwinding, message
ownership, cancellation backpressure and stale terminal-token publication paths.
The repairs now drain graph work before captured state is destroyed, propagate
worker failure without requiring a user Stop, and reject retired execution
publication. Gated `[filtered-worker-failure]` and stale-terminal tests have native
checkpoint coverage. Final repaired sanitizer acceptance is still distinguished
from the historical RED reproductions below.

An earlier, pre-repair broad UI checkpoint ran 261 cases / 7,826 assertions: 257 cases passed
and the four newly added owner-layout/Copy cases failed. The corrected Copy
fixture subsequently reached its intended RED: two cases, 48 assertions, four
failures at the unsuccessful-completion assertion. Stable Selection navigation
passed in a 100-case focused run; one new view test exposed an overbroad fake
append-preservation proof, corrected to exclude reappearance of old hidden IDs.
The corrected view navigation test subsequently passed. The owner layout/Copy
GREEN checkpoint passed all four cases / 276 assertions.

The combined review RED batch ran ten cases / 225 assertions: one case passed
(the corrected view navigation) and nine cases failed for the intended copied
format, readiness, marked-source cutoff, capture label/CR, and folder framing/work
properties. In particular, 32 adjacent near-EOF marks read 16,777,216 source bytes
against a 4,608-byte direct-row allowance.

Native Qt6 ASan/UBSan then ran `[filtered-producer-failure]` and
`[filtered-matcher-failure]` separately, with `detect_leaks=0` as required by Apple
ASan. The producer case aborted with a `stack-use-after-scope` in TBB's queued
buffer-node task during graph destruction. The matcher case reached its injected
failure but failed the `terminalBeforeUserStop` assertion: producer backpressure
required cleanup interruption before the search could terminate. These are RED
reproductions, not sanitizer acceptance of the repaired implementation.

### Latest completed verification checkpoints

These results supersede the stale "RED prepared" descriptions above, but they do
not collapse several revisions/environments into one final acceptance result.
The coordinator will append final runtime commands, results and the implementation
commit reference after the frozen-tree runs complete.

Configurations: Homebrew Qt 6.10.1 and Qt 5.15.18; RelWithDebInfo, LTO disabled.
Normal Qt6 enables Vectorscan. Qt5 and native ASan configurations disable it.

| Checkpoint | Completed evidence | Scope / limitation |
| --- | --- | --- |
| Earlier full native Qt6 CTest | 39/39 tests passed; unit: 1,232 cases / 55,865 assertions; UI: 261 / 7,860; Vectorscan: 14 / 68 | Predates the newest restoration and storage-observer refinements. Not final-tree runtime acceptance. |
| Latest native focused restoration/integration | `klogg_tests '[quickfind-restoration],[match-selection],[indexedcontent],[folder-framing],[filtered-snapshot]'`: 69 cases / 1,059 assertions passed | Includes current saved-selection restoration, zero-fragment Stop and real reset-routing regression. The completed log was inspected during this reconciliation. |
| Final coverage characterization | `klogg_tests '[empty-navigation],[match-copy-format],[match-executor],[match-lifetime]'`: 6 cases / 115 assertions passed | Actual empty-result End-of-Line, typed text/focus during extraction and serialization, and gated hide/destruction with lease-release observations. Production was unchanged. |
| Repaired native ASan/UBSan review window | Unit: 112 cases, 111 passed and one Stop-fixture timing failure; UI: 7 cases / 308 assertions passed; no sanitizer defect reported | Stop fixture has since been gated; final sanitizer runtime rerun remains pending. Apple ASan uses `detect_leaks=0`, so this is not leak-sanitizer acceptance. |
| Final native compilation | All three native `ci_build` configurations passed | Compilation is not execution of final test suites. |
| Static checks on frozen source | Locked cppcheck 2.13 passed 138 TUs; all 29 changed headers compile independently; hygiene, determinism, platform and diff checks passed | These checks preceded the latest documentation-only edits. Frozen-source audit hash: `5a16c18f1af4363ca8087b0ff05c00dfda0e31da6b6ede47dcb61b308dc7c063`; this is not a Git commit reference. |
| Qt5 package recipe | Exact CI `macdeployqt` plus static-bundle fixup passed | Final package refresh is underway; no additional Cocoa runtime smoke result is claimed here. |
| Qt6 package recipe | Exact Homebrew recipe reported nine closure/install-ID failures | Not a passing package result; no manual repairs are claimed. |
| Locked Linux Docker | Earlier runtime results invalidated by stale first-party objects | A clean first-party rebuild/full acceptance is running; no pending outcome is predicted. |

### Final-window findings and targeted follow-up

The first final native runtime window used `ctest --output-on-failure --parallel 2`:
Qt6 passed 39/39 (unit 1,235 cases / 55,990 assertions; UI 261 / 7,860;
Vectorscan 14 / 68), and ASan/UBSan passed 38/38 (unit 1,234 / 55,896;
UI 262 / 7,878). The exact Qt5 package recipe also passed Cocoa and offscreen
runtime smoke. These runs predate the rendering follow-up below.

Qt5 passed 37/38 registered tests; its unit process crashed in an ASCII-decoder
test. LLDB confirmed a null `codecForName("US-ASCII")` result, not a changed
production decoder contract. A portable MIB-3 test codec now exercises identical
ASCII/non-ASCII assertions without depending on optional Qt codec inventory;
`[filtered-backend-parity]` passed 6 cases / 248 assertions on Qt5.

The optimized dense-paint harness exposed a real P6 gap even though its process
returned success: 40,000-fragment selection/copy took 194 ms, but wrapped repaint
took 22.539 s (unwrapped 22.654 ms). Stack sampling located repeated whole-suffix
font shaping in `WrappedString` before compositor clipping. The deterministic
`[wrap-work]` RED measured 1,098,606 UTF-16 units for 8,192 input units against a
131,072-unit allowance. Exponential prefix bracketing made all 22 wrapping cases /
1,998 assertions pass and reduced wrapped repaint to 1.348 s, still unacceptable.

A second `[match-paint-window]` RED observed a 5,080-pixel cache for a 513-pixel
bound, submission of all 16,384 text units, and rebuilding of unchanged layouts
at both head and tail. The follow-up separates full logical frame extent from a
bounded pixel window, shares immutable text/font/width-qualified row layouts,
and clips actual text drawing as well as composition. This checkpoint passed
64 unit cases / 2,828 assertions plus all 261 UI cases. Best-of-three repaint
became 22.912 ms unwrapped / 84.071 ms wrapped; selection/copy remained 193 ms.
No lower timer cadence or weaker glyph-boundary/scrolling contract was used.

Focused follow-up review reproduced prepaint hit testing retaining a tail column
(15,640) after a head scroll, and reusing the old map after a same-sized source
replacement. Refreshing the transform independently from layout reuse and retiring
the map on source/interpretation updates made `[match-paint-window]` pass all 32
assertions. A pre-existing map-cache test was made explicitly paint-free during
its source swap; it now tests the intended timing boundary instead of demanding
a redundant rebuild after a paint has already published valid geometry (13
assertions passed). Frozen-tree broad acceptance follows below.

### Final accepted source and local verification (2026-10-27)

P0-P7 implementation and the requested local macOS/Linux acceptance are complete.
All D01-D13 repairs are accepted for this scope. The frozen code/build-input digest
is `3e5504e7fdf81010ef2ac32a1d3d5769c8f302527e8d17853a8f3b9832472275`.
Documentation-only reconciliation follows that freeze. The implementation is
recorded by the commit containing this completed execution record. Independent
fixes are `4ca93b9f` (direct Foundation linkage) and `d4be8fb3` (bounded wrapping
probes), preceding the integrated pipeline commit.

| Final environment / check | Result |
| --- | --- |
| macOS Qt 6.10.1, Vectorscan, RelWithDebInfo | CTest 39/39; unit 1,238 cases / 57,993 assertions; UI 261 / 7,861; Vectorscan 14 / 68; 183.31 s |
| macOS Qt 5.15.18, fallback backend, RelWithDebInfo | CTest 38/38; unit 1,237 / 57,898; UI 261 / 7,870; 177.91 s |
| macOS Qt6 ASan/UBSan, fallback backend | CTest 38/38; unit 1,237 / 57,900; UI 262 / 7,879; 216.25 s |
| Locked Linux Jammy GCC 11.4 / Qt 5.15.3 / ASan/LSan | CTest 40/40; unit 1,238 / 57,939; UI 261 / 7,877; Vectorscan 14 / 68; 773.12 s |
| Real Linux xcb Clipboard and PRIMARY | `[match-selection-x11]`: 1 case / 27 assertions |
| Optimized local selection/copy and dense paint | 2 hidden cases / 52 assertions; best of three: selection/copy 196 ms, unwrapped paint 22.996 ms, wrapped paint 92.134 ms |
| Final Qt5 staged application | Exact CI deploy/fix recipe, static x86_64 closure verification, Cocoa and offscreen runtime smoke passed |
| Static analysis | Changed-line clang-tidy clean; locked cppcheck 2.13 strict sweep: 138 first-party translation units; 30 changed headers compile independently |
| Repository checks | Hygiene, platform-fragile, test-determinism and `git diff --check` passed |

Every registered CTest entry passed with no entry skipped. Existing test-level
headless/device qualifications remain those of the suite; these counts do not
claim interactive device coverage. Native trees ran serially with respect to
Docker, with two CTest jobs within each native tree. Linux used one CTest job.
Apple ASan used `detect_leaks=0:abort_on_error=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`; it is not Apple LSan coverage.
Linux used the CI ASan/LSan options and existing suppression files, unchanged.

Reproduction entry points:

```sh
cmake --build build_root --target ci_build -j 4
cmake --build build_qt5 --target ci_build -j 4
cmake --build build_root_asan --target ci_build -j 4
cmake -E chdir build_root ctest --output-on-failure --parallel 2
cmake -E chdir build_qt5 ctest --output-on-failure --parallel 2
# Set the Apple sanitizer options above for this command.
cmake -E chdir build_root_asan ctest --output-on-failure --parallel 2
QT_QPA_PLATFORM=offscreen KLOGG_PERF_GATES=1 build_root/output/klogg_tests \
  '[match-selection-perf],[match-paint-perf]' -s
python3 scripts/verify_macos_qt_bundle.py --app /path/to/staged/klogg.app \
  --expected-architecture x86_64 --smoke cocoa --smoke-if-present offscreen
```

The locked Linux build image was
`ghcr.io/zeacent/klogg-ci-env@sha256:07070d0a24230a9f26dc69841d6f769d33b03fa0095ed1a5428fc16111cb3de5`;
the analysis image was
`ghcr.io/zeacent/klogg-ci-env@sha256:77491e1d3acd3661659b49cd5d2d6ef17b9c0dbd9b3e03557c3f9957a98cf2de`.
Run `ctest --output-on-failure --parallel 1` inside its configured build tree,
then invoke `klogg_tests -platform xcb --warn NoTests '[match-selection-x11]'`
against a real X server, retaining the CI locale and sanitizer environment.

Linux provenance was explicitly repaired: archived source timestamps had allowed
old objects to survive an earlier sync. The accepted run followed a ccache-disabled
rebuild of all 254 first-party objects; later frozen deltas rebuilt only verified
dependent objects. Source hashes, compile records and preserved-object hashes were
checked. Earlier stale-binary results are not acceptance evidence. Analysis build
output was moved beneath excluded `build_root`, not hidden by weakening identity
contract tests.

The final Qt5 staged input executable SHA-256 was
`a059a537bb5b2930cfd342f1050f63dbeb4007d488fc66d2bbed9cb223811517`.
Local raw evidence is retained under `klogg-native-final-jmcavl0q` in the host temp
directory and `/tmp/klogg-feature-linux-accept.T0Umed`; the durable results,
commands, source digest and environment identifiers are recorded here.

### Explicit limits and packaging disposition

- Native Windows runtime and TSan were not run locally. Portable Windows runtime
  verifier/packaging contracts ran as registered tests; that is not a Windows GUI
  or sanitizer claim.
- The Homebrew Qt6 deployment recipe still fails nine Qt framework closure/install-ID
  checks. Its legacy fix-up does not correctly handle that host's Qt6 framework
  layout. This is an unqualified packaging variant, not a passing result; no manual
  bundle repair was used to claim parity. The final Qt5 CI-recipe package passed
  static and Cocoa/offscreen verification, and Qt6 passed native tests/sanitizers.
- Source retention uses the declared bounded resident storage and private spill
  policy; indexed/scanned bytes can require substantial temporary disk space.
  Selected output remains proportional in memory. A regex call in progress is not
  forcibly preempted.
- R15 is verified through fixed-cutoff/accepted-version gates and real live
  append/clear ownership tests, not a long-duration soak. R16 uses gated shared
  executor teardown plus source-specific lifecycle tests, not every platform's
  Cartesian product. Synchronous owner-thread acquisition either returns a lease
  or fails before dispatch.
- The 33 ms refresh policy is unchanged. The dense-row measurements above are
  observations, not a claim that every 40,000-fragment frame fits a 33 ms budget.

### PR #85 review and CI follow-up (2026-10-28)

- Batch folder-result transfer now preserves retained sources for zero-match files,
  matching streaming transfer. Marked-row snapshots remain readable after source
  deletion without adding canonical matches or visible matching groups.
- Filter presentation contains retained-source read failures at its text/color
  boundary. Snapshot consumers still receive `SnapshotError`; an unavailable row
  is not fabricated as warning text. The focused regression covers text, expanded
  text, length, ANSI colors, batch reads, and strict snapshot failure.
- Windows retention fixtures query open-file handle sizes instead of potentially
  stale NTFS directory metadata, and remove temporary files through their owning
  `QTemporaryFile` before proving retained snapshots survive source deletion.
  Exact storage bounds and child-destruction assertions remain enforced. Portable
  source-mutation checks guard these fixture contracts on non-Windows hosts.
- Restore the worker-entry diagnostic lost during dispatch unification. Update
  structural tests for shared Quick Find submission and scope the trace-writer
  assertion to the function rather than a neighboring class.
- Replace Quick Find's cross-thread notification functor with a receiver-bound
  named queued slot carrying a built-in Qt string value. The first-party macOS
  TSan job reported the functor's publication through uninstrumented Qt. Preserve
  queued owner-thread delivery and cancellation on receiver destruction without
  adding sanitizer suppressions. Characterization tests cover both owner-side
  acquisition failures and worker-side read failures.
- A subsequent native TSan run exposed the same uninstrumented handoff at a
  `QtConcurrent` task vtable. Quick Find, extraction, and serialization now share
  an owned-task bridge: the existing bounded Qt pool schedules work, a C++ mutex
  publishes task construction, and standard futures synchronize results and
  exceptions. Qt transports only the completion event. Cancellation, pool limits,
  and owner teardown waits are preserved; no extra unbounded thread pool or
  sanitizer suppression was introduced.
- Defer the optional removal of three obsolete cppcheck suppressions: those bytes
  are part of the authenticated analysis-environment policy. Removing them without
  genuine requalification rejected Static analysis, Coverage, and CodeQL before
  execution. No lock, receipt, signature, or integrity check was weakened. The
  existing compatibility test now runs first in `scripts/run_ci_quality.py`.

The initial local acceptance omitted the complete Python quality gate and used
analysis tools without the authenticated environment consumer. The required
follow-up entry point is `python3 scripts/run_ci_quality.py`; direct native tests
or analyzer runs are not a substitute for that gate. Focused review regressions
were observed failing before the fixes and passing afterward.

Final follow-up runtime acceptance uses macOS Qt 6.10.1 with the fallback regex
backend: native, ASan/UBSan, and first-party TSan each passed all 38 CTest entries.
ASan initially hit an ADB helper startup timeout in an unchanged fixture, without
an ASan/UBSan report; that case passed 53 assertions alone, then the entire 38-entry
ASan suite passed again. The initial timeout is not claimed as a passing run.
The owned-task bridge also compiled and executed with native Qt5 and the locked
Linux GCC/Qt5 image. Changed-line clang-tidy and locked cppcheck on the new bridge
passed. Optimized best-of-three performance remained 196 ms for selecting/copying
40,000 fragments, with dense paint at approximately 22 ms unwrapped and 87 ms
wrapped. The complete quality gate passed all lint stages and 2,607 Python tests
(with 10 existing platform/tool-dependent skips). Windows runtime confirmation
remains a hosted-CI responsibility.

### Integrated retirement acceptance (2026-10-29)

The runtime/analysis freeze is `d70d3c78`, including `bc294a25` and the semantic
merge of `e00976f8`. Windows x86 configuration, CI, package feeds, and release
paths are retired; Windows x64 validation and non-Windows Qt5 coverage remain.
Overlapping folder and diagnostic tests retain the match-selection contracts.
Historical disabled source-material entries remain unchanged to preserve their
authenticated identity, not to advertise active x86 support.

The follow-up review's capture-lifetime concern was independently reproduced:
a blocked captured-object destructor could coexist with an already-ready result.
The callable is now reset before either result or exception publication. The
same defect explained the Linux/MSVC retained-snapshot assertion, which libc++
cleanup timing had hidden. The deterministic regression covers both outcomes.
Locked clang-tidy-18 findings were fixed without suppressions, preserving index
lock/lease destruction order and aggregate defaults.

| Integrated check | Result |
| --- | --- |
| Complete repository quality gate | All lint stages passed; 2,568 Python tests, 12 platform/tool-dependent skips |
| macOS Qt6 native, ASan/UBSan, first-party TSan | Each CTest suite passed 38/38; the TSan run lacked CI's O1 project profile and is not CI-parity evidence |
| Locked Linux GCC/Qt5 ASan/LSan | CTest 40/40 with CI sanitizer options and existing suppressions |
| Locked GCC13/Qt6.9.3 coverage profile | CTest 39/39; line 81.6%, branch 46.5%; unchanged floors 61.1%/32.6% passed |
| Locked clang-tidy-18 | Prior clean verdict invalidated: `/usr/local/src` did not match the authenticated header filter; canonical-path rerun required |
| Locked strict cppcheck | 156 first-party compilation entries passed |
| Optimized best-of-three 40,000-fragment performance | Selection/copy 233 ms; dense paint about 28 ms unwrapped / 102 ms wrapped |

Source bytes were verified when synchronizing an isolated clone of the retained
Linux build volume; changed files received fresh timestamps, and retired tracked
files were removed. Original acceptance volumes were not modified. Native suites
and Linux runtime suites ran serially with respect to one another, with two CTest
jobs per suite. The unchanged macOS ASan ADB-helper startup timeout recurred once;
its isolated 53 assertions and complete ASan rerun passed, without a sanitizer
report. That failed attempt is retained as such. Hosted Windows x64 validation
and the updated-head GitHub checks remain required after publication.

### Updated-head review and validation corrections (2026-10-29)

Hosted checks at `6b8545eb` exposed gaps not covered by the preceding acceptance:

- The filtered presentation regression injected only `SnapshotError`, while
  retained indexed storage can throw `IndexedContentError` and decoding can throw
  standard allocation/length exceptions. The text and ANSI presentation boundaries
  now contain `std::exception`; strict snapshots still propagate the original
  type. Parameterized failure injection produced 18 failing assertions before
  the fix; all 440 assertions in 26 filtered-snapshot cases then passed.
- The owned-task capture guard's reference member violated the locked analyzer's
  policy. A non-owning pointer preserves the same before-readiness destruction
  ordering, including exceptions. No analyzer suppression was added.
- The previous locked clang-tidy clean verdict was incomplete: the authenticated
  `HeaderFilterRegex` requires `/klogg/src/`, but the local clone used
  `/usr/local/src/`. The analyzer silently filtered header diagnostics. Keep the
  signed policy inputs unchanged and configure the local analysis checkout and
  compile database at `/klogg`. Before accepting a local result, run
  `python3 scripts/check_clang_tidy_scope.py --source-root /klogg/src --clang-tidy /usr/bin/clang-tidy`.
  The guard rejects the relocated worktree and verifies all actual first-party
  headers plus dependency exclusion. The locked-tool canonical-path check passed
  for 192 headers; analysis reruns are recorded separately below when complete.
- The Windows support-policy fixture set `WIN32` using a `-D` cache argument.
  CMake's Windows-host normal variable shadows that value, so its non-Windows
  scenario incorrectly exercised Windows x86 rejection. Set the scenario's normal
  variable explicitly from a test-only parameter after host initialization. A
  host/target cross-product reproduces the original mismatch on macOS too: two
  RED subcases, then all nine contract tests passed (three native-tool skips).
- Hosted macOS TSan completed all 1,241 unit cases, then found a mimalloc shutdown
  race with a TBB worker's TLS destructor. The local TSan compile database also
  revealed a parity gap: `KLOGG_CI_MACOS_TSAN_O1=ON` without
  `CMAKE_PROJECT_INCLUDE=.../cmake/CiMacosTsanProfile.cmake` did not activate the
  CI optimization profile. The existing
  `python3 scripts/verify_macos_tsan_profile.py build_root_tsan/compile_commands.json`
  guard correctly rejected that configuration. Local reruns must load the actual
  profile and pass this guard before building; neither successful test summaries
  nor cache intent prove process-exit safety.
- A shared process-scoped `TaskSchedulerLifetime` now outlives Qt applications
  and their task producers, then explicitly finalizes TBB. External TBB callers
  must be native-joined first, as the indexing/search workers already are; the
  save graph runs synchronously on the UI thread. Finalization failure emits a
  clear diagnostic and exits unsuccessfully rather than entering allocator
  cleanup with live workers. Application, grep, unit/UI, and search benchmark
  processes share this owner. Grep now returns through its event loop instead of
  bypassing stack destruction with `exit`. A dedicated executable covers worker
  TLS completion, no-work shutdown, retained-handle failure, unjoined-producer
  failure, and application teardown after native producer joins. The original
  raw-release-only implementation failed the TLS and failure-path assertions.
  An eight-entry process ownership guard also passes.
- The first locked Qt5 ASan rerun passed 40/41 entries but rejected the new test's
  direct Qt-pool TBB caller. That test assumed more than production guarantees:
  Qt5.15.3 `QThread::wait()` observes Qt's finished state before detached pthread
  TLS teardown, so application/pool destruction is not a native join. The positive
  regression now exercises the actual joined producer boundary and verifies both
  producer and TBB-worker TLS completion. A separately gated, deliberately
  unjoined native producer must still fail explicitly. No sleeps, finalization
  retries, version-conditionals, or sanitizer suppressions were introduced.

These corrections do not change the Windows x86 retirement, the strict required
checks, authenticated receipts, or sanitizer suppression policy. The arm64 job
cancellation at this head was a hosted-runner acquisition failure, not a source
failure. The ordinary Intel macOS runner lost communication during dependency
setup, before compilation; its job log was unavailable. Neither infrastructure
failure was converted into a skipped requirement.

### Follow-up acceptance at `336ec85a` (2026-10-29)

All production fixes were frozen at `336ec85a`. The final locked analyzer found
only two unchecked C stdio return values in the unsuccessful-shutdown diagnostic.
Their deliberate best-effort handling is now explicit: failure to print cannot
change the unsuccessful exit. This behavior-preserving cleanup was rebuilt and
rechecked in all three native lifecycle configurations and in the complete locked
Linux suite. No warning suppressions or policy changes were used.

| Check | Verified result |
| --- | --- |
| Complete quality gate at clean `336ec85a` | 2,579 Python tests, 12 platform/tool-dependent skips; all lint stages passed |
| Native Qt6, including search benchmark contracts | CTest 40/40 |
| Native ASan/UBSan | CTest 39/39 |
| Native first-party TSan with the actual CI project profile | CTest 39/39; 549 C/C++ commands verified; first-party 139/139 and required dependency 54/54 instrumentation retained |
| Final diagnostic cleanup characterization | Five lifecycle cases passed in native, ASan/UBSan, and TSan executables |
| Locked Linux GCC/Qt5 ASan/LSan, final synchronized source | CTest 41/41, including lifecycle and benchmark contracts |
| Locked clang-tidy-18 | Full PR sources, affected-header fan-out, and standalone headers inspected against `306afd95`; the only two remaining diagnostics were fixed and the entire follow-up diff against `6b8545eb` passed the source/standalone-header recheck |
| Locked strict cppcheck | All 158 selected first-party compilation entries passed |
| Scope/provenance preflight | 193 actual first-party headers included, dependency examples excluded; all 1,413 tracked source files byte-verified in the isolated clone |
| Process-exit smokes | Native, ASan, and TSan grep produced only the matching row and exited successfully; one 50 MB regex case, bounded folder search, and synthetic process/integrated live-capture runs exited successfully |

Runtime suites ran serially across native/sanitizer/container trees, with two
CTest jobs within each suite. Native acceptance used Qt 6.10.1 and the fallback
regex backend; locked Linux used Qt 5.15.3 with Vectorscan. The TSan profile guard
verified effective `-O1`, retained debug information, sanitizer scope, and no LTO
before testing. The earlier missing-profile and header-filter claims remain
explicitly invalidated above rather than being relabeled as passing acceptance.

Windows runtime, coverage, CodeQL, and all required updated-head hosted checks
remain publication gates. The earlier coverage result is historical, not a rerun
of this final follow-up. A repository-required approving review remains separate
from fixing actionable comments. Do not merge or bypass that requirement.
