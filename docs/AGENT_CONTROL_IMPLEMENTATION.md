# Local control implementation record

Status: experimental developer CLI and initial GUI service implemented; the full
control surface and packaged release acceptance are not complete.
The accepted product/architecture contract is in
[AGENT_CONTROL_PROPOSAL.md](AGENT_CONTROL_PROPOSAL.md).

## Delivery gates

- [ ] P0: characterize GUI command owners and scope.
- [ ] P1: shared typed mutations and coherent state observation.
- [ ] P2: staged transactions, dependency validation, atomic publication.
- [ ] P3: asynchronous open/filter/close and file/folder/live parity.
- [ ] P4: local endpoint and CLI, default-on setting, global disable.
- [ ] P5: shell integration, final-package acceptance, user documentation.

Do not treat passing tests for one owner as completion of an entire gate. There
is no control listener until the application operation contract and endpoint
boundary have their own tests.

## Slice 1: color-label command owner

Start at `ColorLabelsController`, the existing shared owner for file, folder, and
live tab labels. It already coordinates GUI selection intents, quick-label state,
and every view of a tab, including later-created result panes. This is a small
observable boundary for introducing typed, selection-independent commands and
revision tracking without creating separate automation state.

### Characterization

Existing coverage includes:

- `tests/unit/colorlabelsmanager_test.cpp`: per-entry match options, case-aware
  moves/removals, distinct bulk texts, caps, cycle state, settings migration.
- `tests/ui/crawlerwidget_test.cpp`: file selection-to-label propagation.
- `tests/ui/foldercrawler_test.cpp`: folder main/results propagation.

New focused characterization in `tests/unit/colorlabelscontroller_test.cpp`
checks that an intent from one pane uses that pane's selection, shares its result
with all views and late registrations, and does not alter either selection.
The fixture uses real retained live data without timers, devices, or a search.

### Implementation and RED/GREEN evidence

- Added `ColorLabelsController::snapshot()` as an owner-thread value observation.
- Added typed Set/Remove/Clear edits with complete-batch validation, optional
  expected revision, explicit errors, and one publication/notification per change.
- GUI selection intents and explicit commands use the same manager mutations and
  publication path. GUI bulk selection retains its existing truncation policy;
  explicit requests fail on resource limits instead of applying a silent prefix.
- Tracked hidden cycle state as well as visible labels for semantic no-op detection.
  Human add/remove ABA changes invalidate a previously observed revision.
- Removed the label manager's dependency on `AbstractLogView`; the value model
  now depends on the existing label-entry type, not the rendering widget.

Evidence recorded on 2026-11-15:

1. The first characterization fixture aborted because it supplied no overview
   widget to `LogMainView`. This was a test setup failure, not a product RED.
   Supplying a real `OverviewWidget` corrected the fixture.
2. Characterization baseline: `[colorlabels]` passed **10 cases / 102 assertions**.
3. New API RED: compiling the added contract tests failed specifically because
   `Edit`, `EditResult`, `snapshot`, `apply`, and `labelsChanged` did not exist.
   This is missing-interface evidence, not a runtime behavioral failure.
4. GREEN: `[colorlabels]` passed **14 cases / 142 assertions**.
5. REFACTOR: the same unit selection passed after removing the model-to-view
   dependency. Existing file/folder integration `[colorlabels]` passed
   **4 cases / 80 assertions**.
6. Additional resource-limit and retired-view tests passed in the combined
   label/filter run: **42 cases / 452 assertions**.
7. Shared `AbstractCrawlerWidget::reviewControls()` access was tested on actual
   folder and live-backed file widgets. The RED compile failed on the missing
   adapter method; both implementations now return their existing GUI owners.
   The combined owner run then passed **43 cases / 476 assertions**.

### Local build environment and limitations

The isolated `build_root` uses Qt 6.10.1, Apple Clang 21, C++17, Debug, no LTO,
tests enabled, and benchmarks/native iOS packaging disabled. Dependencies were
fetched into its own CPM cache; no other build tree was changed.

The initial Vectorscan-enabled build failed in the pinned dependency's
`src/util/supervector/arch/x86/impl.cpp` under
`-Werror,-Wgnu-alignof-expression`. No third-party source was patched. Local
owner tests instead use the documented `-DKLOGG_USE_VECTORSCAN=OFF` configuration.
This does not establish Vectorscan-enabled, sanitizer, Qt 5, or package acceptance.

Commands used after configuration:

```sh
cmake -S . -B build_root -DKLOGG_USE_VECTORSCAN=OFF
cmake --build build_root --target klogg_tests klogg_itests --parallel 6
QT_QPA_PLATFORM=offscreen \
  KLOGG_CAPTURE_COORDINATION_ROOT="$PWD/build_root/Testing/coordination/control-labels" \
  KLOGG_PORTABLE_CONFIG_DIR="$PWD/build_root/Testing/config/control-labels" \
  build_root/output/klogg_tests '[colorlabels]'
QT_QPA_PLATFORM=offscreen \
  KLOGG_CAPTURE_COORDINATION_ROOT="$PWD/build_root/Testing/coordination/control-labels-ui" \
  KLOGG_PORTABLE_CONFIG_DIR="$PWD/build_root/Testing/config/control-labels-ui" \
  build_root/output/klogg_itests '[colorlabels]'
```

Run test processes serially with respect to other development test invocations.

## Slice 2: filter draft observation

`SearchToolbar` exposes a detached `FilterDraftSnapshot`: exact edited text,
case/regex/inverse/Boolean/auto-refresh/keep-results flags, and a semantic revision.
It is explicitly not the submitted query, applied result query, or readiness.

Revision tracking observes the owned line editor because folder operations can
block the combo box's signals. It runs before existing host notifications,
detects human/programmatic ABA, and ignores no-op setters, history repopulation,
and busy/idle presentation. Reads do not focus, edit, emit signals, or search.
The existing synchronous signal-test helper no longer uses a fixed sleep.

- Baseline `[searchtoolbar]`: **20 cases / 207 assertions**.
- Runtime RED: a compiling snapshot scaffold with constant revision failed
  **5 cases / 5 assertions**, specifically on missing revision advancement.
- GREEN `[searchtoolbar][filter-draft]`: **6 cases / 89 assertions**.
- Full toolbar selection: **26 cases / 296 assertions**.
- Targeted file/folder host regressions: **17 cases / 307 assertions**.
- Standalone header, determinism and portability checks passed.

## Slice 3: staged label writes across owners

`ReviewTransaction` now stages writes to existing label controllers and tracks
label/filter-draft read dependencies. It is an owner-thread, scope-bound object,
not a remotely addressable transaction service yet.

- Read-your-writes does not mutate live owners. Repeated reads retain the original
  base/precondition rather than refreshing away a conflict.
- Validate all dependency lifetimes and revisions before installation. A conflict,
  invalid operation, closed target, or exceeded budget publishes nothing.
- Prepare per-view projection values before installation; the view adopts an owned
  label vector by swap. All owner states and all projections are installed before
  any change notification. Late-created panes participate at commit time.
- Mark the transaction terminal before notifying. Reentrant commit cannot replay,
  and a callback closing another committed owner cannot cause a dangling callback.
- Budgets cover owner count, total commands, and text units across staging calls.
  Abort releases unpublished candidates. Unrelated owners do not cause conflicts.

RED: the transaction contract test initially failed to compile because the public
transaction header did not exist. GREEN: the combined transaction/label/filter
selection passed **49 cases / 538 assertions**. After adding cross-owner rendered
projection, cumulative-budget, repeatable-read, and callback-lifetime coverage,
the broader regression run passed:

- Unit selection `[review-transaction],[colorlabels],[searchtoolbar],[highlighter],[match-selection]`:
  **104 cases / 11,572 assertions**.
- File/folder label/filter/search-generation/context/history/keep-results selection:
  **19 cases / 377 assertions**.

Both runs used the existing Qt6/Qt-regex configuration and isolated state roots,
sequentially. Expected error-path logging in existing match-selection tests did
not represent failed assertions.

### Transaction hardening

A runtime test found that a batch or successive staging calls could each stay
under the per-command 1,000-text cap while exceeding the original bulk-action
limit in aggregate. RED failed six assertions; the validator now accounts for
text count as well as text units across the complete action/transaction. GUI
single-selection behavior is unchanged. The focused combined owner selection
passed **78 cases / 11,196 assertions** after this fix and the highlighter cache
fix described below.

## Slice 4: global highlighter observation and coherence

`HighlighterSetCollection::snapshot()` returns owner-local revision, ordered
sets/rules, active set IDs, palette, and label defaults. `HighlighterSet` exposes
rules as detached values. Semantic comparisons exclude matcher caches.

Existing setters, activation, storage reload, and dialog Apply/OK participate in
revision tracking. Copy assignment preserves the recipient's revision and
prepares effective rules before replacement; it never imports a draft counter.
Copies are independent owners. Snapshot reads do not synchronize storage,
compile matchers, save, or publish state.

- Baseline `[highlighter]`: **15 cases / 10,299 assertions**.
- Runtime revision RED: **6 cases failed** on missing revision increments.
- Separate runtime cache RED: **2 cases / 6 assertions failed** when edited
  patterns/modes or reloaded rules retained their old optimized matcher.
- After invalidation fixes, `[highlighter-state]`: **9 cases / 183 assertions**;
  `[highlighter]`: **24 cases / 10,482 assertions**.
- Combined owner/rendering regression: **114 cases / 11,762 assertions**.
- Marks characterization before any mark-owner change: **10 cases / 842
  assertions**. Catch SCENARIO names require a leading wildcard in selectors.

Coordinator review added an individual-set matcher assertion to the actual-dialog
case. It failed even though the combined active matcher was correct: the editor
could publish a set retaining an inherited set-level prefilter. The editor now
invalidates that cache in its common change-notification path before subscribers
copy its draft. The combined highlighter/transaction/label/filter run then passed
**77 cases / 11,188 assertions**, before the aggregate-budget test was added.

All counts above are for Qt6 Debug with the Qt regex backend. Vectorscan-enabled
and other platform acceptance are still outstanding.

## Slice 5: mark-owner observations

`LogFilteredData::marksSnapshot()` observes each file/live filtered model's marks;
`FolderCrawlerWidget::marksSnapshot()` observes the existing per-file folder map
shared across its panes. Snapshots are detached and revisioned. Actual changes
advance revision; duplicate adds, missing removals, empty clears and equivalent
restores do not. Folder truncation removes stored marks; absent members retain
stored marks even when their rows cannot currently be projected.

These are source-model indices, not stable evidence locations. Source generation
and row-identity mapping remain separate pending work. Reading marks does not
open sources, search, select, focus, or synchronize settings.

- Runtime RED: six cases failed on constant revisions.
- Final focused `[marks-state]`: **7 cases / 110 assertions**.
- Characterization and nearby mark/kept-pane regressions: **22 cases / 1,086
  assertions** (Qt6, Qt-regex).

## Slice 6: runtime inventory

Window and tab UUIDs belong to object lifetimes rather than paths, indices or
persisted session IDs. `MainWindow::inventorySnapshot()` returns the ordered,
weakly referenced actual tab inventory, current tab ID, and per-view Session
metadata. Duplicate folder paths remain distinct; collapsed members remain listed.
Display name and rendered tab title are separate. The observation cache is not a
second authoritative registry, and queries have no GUI or settings effects.

Revisions track membership, ordering, selection and exposed metadata, including
ABA during an unfinished drag. They do not claim pane/source/result coverage or
workspace atomicity; a compound legacy action may advance revision more than once.

- Characterization: unit **4 cases / 22 assertions**, integration **3 / 45**.
- Runtime RED: **3 intended revision failures**.
- Focused GREEN: integration **6 / 114**, notification unit **1 / 9**.
- Nearby regressions: unit **6 / 187**, integration **12 / 195**.
- All three changed headers passed standalone compilation; scoped portability,
  determinism and diff checks passed. GUI Qt5 runtime was not run.

## Slice 7: bounded wire contracts

The `klogg_control` module owns four-byte big-endian length framing and the
JSON-RPC 2.0 control profile. The wire codec uses QtCore; subsequent transport
integration adds QtNetwork, without Widgets, log processing or settings. The Qt5/Qt6 container index alias moved to a lightweight header;
existing `containers.h` consumers retain the same alias.

Frames are limited to 1 MiB and 64 messages per feed. Partial reads, coalescing,
truncated EOF, sticky errors and raw UTF-8 boundaries have explicit tests. RPC
uses nonempty string IDs, named object parameters, correlated responses, and
exclusive result/error outcomes. Notifications cannot execute commands; RPC
batches are not transactions. Valid JSON scalars are invalid requests rather than
parse errors. Integer-like domain values remain strings without double coercion.

- Missing-interface compile RED for framing and RPC.
- Edge runtime RED: malformed objects were incorrectly treated as silent
  notifications, and valid scalar roots were misclassified as parse errors.
- Combined framing/RPC GREEN: **15 cases / 276 assertions** on both Qt 6.10.1
  and Qt 5.15.18 in isolated QtCore-only harnesses.
- Normal CMake integration: `klogg_control_protocol_tests` built and passed via
  `ctest --test-dir build_root -R '^klogg_control_protocol_tests$' --output-on-failure`.

## Host execution interruption (resolved)

On 2026-11-15 the host stopped executing CMake, Ninja, ripgrep, and newly linked
local test binaries. Native `ps` reported them in uninterruptible `UN` state for
minutes before any output. Disk space remained available and no swap activity
was reported. This is an observed host execution blocker, not a diagnosed cause;
no OS security settings or permissions were changed to work around it. Pending
build/test commands were stopped rather than duplicated.

At that interruption, the following work had **not reached GREEN**:

- `ReviewControlAdapter` then contained the dispatch interface and an
  unknown-method scaffold. Its new integration cases specify inventory and shared
  draft/label observation, target validation, and query neutrality. The initial
  fixture used an empty folder and entered the existing information dialog; this
  was a setup failure, not behavioral RED. The corrected fixture compiles and
  links with the generated native compiler/linker flags, but executing it is
  host-blocked. These newly registered cases still require runtime RED followed
  by the real adapter implementation; the full test target is not claimed green.
- The separate local transport work adds private runtime descriptors, local
  socket/named-pipe server/client, same-user handshake, limits, and failure-path
  tests. It is not registered in the main CMake target or wired into the GUI.
  The full current transport suite compiles/links against Qt5 and Qt6 with strict
  warnings via native Clang. Only the initial Qt6 ASan/UBSan round trip ran and
  passed (1 case / 11 assertions). An ordinary-build socket-lifetime crash led to
  a `deleteLater` correction; its runtime regression and the expanded 14-case
  suite remain blocked. Native Windows ACL/runtime acceptance was not run.

Execution subsequently recovered: CMake 4.2.1, Ninja 1.13.2 and the normal
protocol test executable all exited successfully. Protocol tests again passed
15 cases / 276 assertions. Work resumed through normal CMake builds; compilation
alone was not substituted for runtime acceptance.

## Slice 8: GUI request adapter and label transaction lifetime

`ReviewControlAdapter` resolves runtime tab IDs against the application's current
window provider, observes actual GUI owners, and serializes explicit state.
`state.get` is intentionally scoped to window/tab inventory. `tab.get` adds the
filter draft and shared labels; it does not claim applied-query, source-generation,
viewport or result readiness yet. Unknown methods and unexpected fields are
rejected rather than dispatched through QObject reflection.

Label set/remove/clear requests use `ReviewTransaction`, either as autocommit or
through explicit begin/get/commit/abort. Transactional tab/label reads capture
repeatable owner dependencies and see staged label writes. Commit validates live
membership as well as weak object lifetimes, so a detached-but-not-yet-deleted tab
cannot receive a delayed publication. Invalid staged edits discard the entire
candidate. Repeated successful commit returns the retained outcome without replay.

Transactions have a 60-second monotonic lease and a 64-record bound. Expiry aborts
unpublished state; terminal records are bounded and evictable, not durable journal
entries. `reset()` discards pending work for the global-disable path, including
safe reentry from a commit observer. Request IDs are correlation IDs, not a
promise of automatic mutation retries or durable idempotency.

- Observation runtime RED: 3 cases / 8 assertions failed against the unknown-method scaffold.
- Transaction RPC runtime RED: 5 cases / 6 assertions failed on missing methods.
- Lease/reset runtime RED: 2 cases / 4 assertions failed on missing expiry/reset.
- Latest adapter + inventory GREEN: **17 cases / 459 assertions** (Qt6, Qt-regex).
- Tests include cross-owner notification atomicity, GUI conflicts, repeatable draft
  reads, unrelated edits, strict revision strings, target retirement and reset reentry.

## Slice 9: headless CLI frontend

The new `kloggctl` entrypoint is a QCoreApplication console frontend. It supports
offline help/version, safe instance listing/selection, bounded raw JSON calls and
state/tab/label/transaction aliases. It does not launch or focus the GUI. Parameters
are preserved as JSON values; 64-bit decimal strings are not coerced to doubles.
Output uses JSON-RPC envelopes and distinct exit codes for invalid arguments,
unavailable/ambiguous targets, definite rejection and unknown outcomes. Unknown
outcomes are not retried automatically. Listing omits endpoint credentials; remote
error details are reduced to a validated domain code rather than echoed blindly.

- Initial runtime RED: 9 cases / 75 assertions failed against the frontend stub.
- A short-read sequential-input regression was separately reproduced and fixed.
- Final isolated GREEN: **10 cases / 528 assertions** on Qt5, Qt6 and Qt6 ASan/UBSan.
- Actual executable smoke: **15 checks** passed for each of those three builds.
- Link inspection confirmed QtCore/QtNetwork only, no QtGui/Widgets.
- Normal application/package integration and real-GUI acceptance remain separate gates.

## Slice 10: private local transport

Private per-user descriptors, bounded local sockets/named pipes, automatic
same-user handshake and a one-request/no-retry client are implemented. Admission,
frame sizes, queued output and handshake time are bounded; GUI callbacks run on
the owner thread. Client outcomes distinguish rejection, not-sent and unknown
outcome. Exceptions do not leak application exception contents onto the wire.

Verified failures and fixes include socket destruction during disconnect, Qt's
unlinking of substituted Unix endpoints, macOS ACL grants bypassing mode bits,
and cleanup mistakenly claiming an endpoint after failed bind. Native PAL
ownership checks now govern endpoint cleanup; authentication includes peer-user
verification, not just possession of a guessed pathname.

- Final isolated transport + codecs: **31 cases / 479 assertions** on Qt5, Qt6,
  and Qt6 ASan/UBSan. This includes 16 transport cases / 203 assertions.
- Normal root CMake targets now include transport, CLI and their tests; all
  three `klogg_control_*` CTest entries passed.
- `KLOGG_CONTROL_DESCRIPTOR_ROOT` supports isolated executable integration.
  Explicit runtime options take precedence; overrides retain private-directory
  validation. A focused discovery RED/GREEN protects this boundary.
- Native Linux and Windows parity remain unrun. Windows rejects NETWORK-logon
  peers using the pipe writer's token before protocol acknowledgment/dispatch;
  it does not claim `PIPE_REJECT_REMOTE_CLIENTS` or zero kernel-level SMB reachability.
- Explicit LeakSanitizer activation is unsupported by this AppleClang runtime;
  ASan/UBSan results are not a LeakSanitizer claim.

## Slice 11: global setting and application composition

`control.enabled` defaults true and preserves explicit false on storage reload.
The Options dialog applies the single switch through the window's configuration
notification, rather than bypassing application observers via a dialog-only mux.
`KloggApp` owns one `ReviewControlService` across its actual window registry;
it starts only on GUI window creation, not secondary file forwarding or CLI
help/version, and stops before application/window-registry teardown.

Disable closes admission and discards unpublished transactions. Re-enable creates
a new control enable epoch; stale instance/transaction IDs cannot silently bind
to it. The setting is global to windows/clients of that GUI instance and persisted
with its profile, not a per-client authorization policy.

- Setting/dialog runtime RED: 2 cases / 2 assertions (missing persistence/UI).
- Settings/dialog characterization: **12 cases / 67 assertions passed**.
- An incremental-build crash exposed public inheritance from generated Designer
  fields. The first build changed `OptionsDialog` layout without recompiling all
  callers; the second build confirmed those missed consumers and restored GREEN.
  The dialog now owns generated UI privately, with an explicit Designer-input
  dependency on its implementation TU; the same characterization stays GREEN.
- Service composition RED failed missing enablement; policy forwarding RED failed
  3 assertions because Apply did not reach the application observer.
- Adapter/service/policy/inventory regression: **19 cases / 518 assertions passed**.
- A real `kloggctl` child process against real GUI owners passed **1 case / 74
  assertions**: discovery, observations, staged writes, human conflict/all-or-none,
  human-state readback, autocommit, and unavailable state after global disable.
- GUI and CLI application targets build normally. Installed-package acceptance
  and shell installation are still separate pending gates.

## Slice 12: bounded snapshot admission foundation

`LogSnapshot::readLineBounded` now checks the exact retained physical record
extent before reading or decoding payload, then checks decoded UTF-16 units.
Providers without extent metadata fail closed. Physical framing counts against
the source-byte budget; synthetic EOF separators do not. This is a lower-layer
primitive, not yet an exposed content-read RPC: aggregate work and serialized
JSON budgets still belong to the future request owner.

Indexed, streaming/CaptureStore, filtered, concatenated, and folder result
snapshots implement metadata-only extents against their frozen leases. Folder
mark rows inspect at most two retained boundary entries, not the original source
file or line payload. These paths share the existing snapshot pipeline rather
than introducing a separate file reader for control clients.

- Primitive RED: 2 cases / 8 failed assertions; GREEN: 2 cases / 13 assertions.
- Provider RED: 2 unit cases / 24 failed assertions and 1 integration case /
  10 failed assertions (missing extent metadata).
- Nearby unit regression: **37 cases / 803 assertions passed**.
- Nearby integration regression: **6 cases / 126 assertions passed**.
- Coverage includes raw-byte rejection before payload access, unsupported
  providers, decoded limits, empty rows, UTF-16 EOF framing, retained leases
  after source deletion/clear, spilled captures, filtered marks, and frozen
  concatenation mappings.
- The UTF-16 fixture explicitly selects both indexing and display encoding;
  forcing index framing alone does not change the display decoder.

## Slice 13: explicit, reversible shell installation

The Options dialog now offers Check, Install / repair, and Remove beside the
control switch. Opening the dialog performs no installer inspection or write.
Checks are read-only; mutations first show exact planned paths in a plain-text,
default-No confirmation. Approved paths are passed back to the installer: a
changed plan rejects before writes rather than silently targeting a different
startup file. Installation never changes local-control enablement.

Unix integration uses a managed user-local launcher plus shell-specific managed
PATH blocks (zsh/ZDOTDIR, bash login file and bashrc, fish/XDG config). Existing
unmanaged launchers, malformed/edited blocks, unsafe ancestors and extended ACL
grants fail closed. Managed replacements keep backups, preserve surrounding
content/line endings, and detect observed concurrent changes. Each file replace
is atomic; multi-file installation explicitly reports partial failure, not ACID.
Removal previews owned changes even when the executable is already on PATH.

Windows integration edits the current user's PATH to reference the bundled
executable directory, preserving native value type and unexpanded placeholders;
it does not copy a bare executable or install a `.cmd` substitute. The native
registry path has not been exercised on Windows. Guidance requires sign-out /
sign-in or restart when Explorer still carries the old environment; opening a
new terminal is not promised to refresh it.

- UI RED: the focused case failed at the missing Check button. An earlier
  uninitialized-settings fixture exception was corrected and was not counted as RED.
- UI/settings GREEN: **14 cases / 141 assertions**.
- GUI policy, transactions, inventory, and actual CLI round trip: **20 cases /
  592 assertions**.
- Root control CTest: **3/3 passed**, including **48 cases / 1680 assertions**
  in the integrated transport/CLI/installer executable.
- Isolated installer Qt5, Qt6, and Qt6 ASan/UBSan: **21 cases / 938 assertions**
  each, including native temporary macOS ACL fixtures, approved-plan changes,
  and an actual `/bin/sh` launcher preserving literal arguments and exit status.
- Shared ancestor/ACL helpers are reused by transport and installer. Qt5.12 split
  compatibility avoids introducing Widgets into the control library.
- Test homes and registry stores are isolated; no actual user shell configuration
  or registry was modified. UNIX fixtures are not presented as Windows-native tests.
- Developer macOS bundles contain the built sibling `kloggctl`; its offline
  version command passed after staging. Relocatable package acceptance remains
  a separate, incomplete gate.

## Slice 14: payload-free panes and explicit asynchronous content reads

`pane.get` exposes the actual main/results pane's availability, current presented
row count, stored top-row anchor, follow/wrap state, visibility and focus. These
are live presentation coordinates, not stable source addresses or a transactional
read dependency. Group headers count in the presented model. Ordinary observation
does not acquire snapshots, read line text, rebuild layout or change focus.

The common host facade delegates to actual view owners and shares a small value
header rather than making all document consumers include the entire view/search
implementation. A real dirty-context regression found that `LogFilteredData`
row counting decoded three lines while calculating widths. Context membership is
now rebuilt independently; only an explicit width query computes/caches widths.

`source.capture` explicitly freezes a pane's existing provider snapshot. Raw
main panes support `presented`; result panes additionally support `matches`.
Main-pane match-only requests reject rather than silently returning all raw rows.
`source.read` submits bounded work against that immutable lease, returning an
operation ID immediately. `operation.get` returns pending/completed/failed/cancelled
state and, on completion, the requested rows with immutable identities and source
line labels. It never reads mutable GUI models on a worker or reopens the original
source path. `source.release` releases a lease, not the source log file.

Limits are fixed for the experimental protocol:

- At most **256 rows** per read, with an explicit zero-based decimal-string start.
- **1 MiB physical source bytes** for the whole batch, admitted from metadata
  before payload access; unknown extents fail closed.
- **262144 UTF-16 code units** and **512 KiB serialized result JSON** per batch.
  Escaping is budgeted; failures discard the whole result, not a silent prefix.
- **16 snapshot leases**, **16 operation records**, and **4 process-wide queued
  or running reads**. Worker permits survive adapter reset until actual retirement.
- Snapshot and operation leases expire after **60 seconds**, including timer-driven
  idle cleanup. Terminal operations may be evicted to admit new work; IDs are not
  durable receipts. Release/expiry/disable does not forcibly interrupt native I/O;
  cancellation discards unpublished output at the next bounded work boundary.

Verification:

- Pane RED: 5 cases / 12 failed assertions; initial GREEN: 5 / 76.
- Dirty-context RED reproduced exactly three unwanted payload decodes; lazy-width
  GREEN and header-light refactoring retained passing characterization.
- Content registry/worker RED: five cases failed at the missing capture boundary;
  the real file RPC separately failed at the unsupported method.
- Final combined unit regression: **67 cases / 1286 assertions** (panes, content,
  bounded snapshot admission, provider lifetime/normalization, installer UI and settings).
- Final GUI integration regression: **41 cases / 1074 assertions**, including
  source-scope rejection, filter contexts, inventory, service and CLI round trips.
- Real `kloggctl` child process acceptance: **1 case / 107 assertions**, now also
  covering pane observation, snapshot capture, asynchronous read, lease release
  and retrieval of the original retained line.
- Isolated content tests: **8 cases passed** on Qt5 and Qt6 ASan/UBSan, including
  idle lease cleanup without new requests. Assertion totals vary with semantic
  polling (observed 225 and 221 respectively). No
  LeakSanitizer claim; `detect_leaks=0` is explicit for the AppleClang runtime.
- Native Windows/Linux content acceptance and full-GUI sanitizer acceptance remain
  unrun. Real live-host and grouped-folder fixtures protect scope/mapping/lifetime
  locally; ordinary state still omits log lines and selection text.

## Developer CLI walkthrough (implemented surface only)

Start the GUI normally and open the documents to review. The CLI never launches
or activates a GUI. Use the executable beside the developer GUI binary (inside
`klogg.app/Contents/MacOS` on macOS), or its full path if shell integration has not
been installed.

```sh
kloggctl instance list
kloggctl --instance INSTANCE state get
kloggctl --instance INSTANCE tab get --tab TAB
kloggctl --instance INSTANCE transaction begin
```

Copy opaque runtime IDs from these responses. Instance and transaction IDs expire
on control disable/re-enable; window and tab IDs belong to their widget lifetime.
None is a persistent session identifier. Use the returned transaction ID to stage
shared labels and inspect the candidate without presenting it:

```sh
kloggctl --instance INSTANCE call label.set --params \
  '{"tab_id":"TAB","transaction_id":"TRANSACTION","label":1,"texts":["timeout"]}'
kloggctl --instance INSTANCE label list --tab TAB --transaction TRANSACTION
kloggctl --instance INSTANCE transaction commit --transaction TRANSACTION
```

The example adds the text `timeout` to one-based label slot 1; it does not read log
contents or run a search. For multi-tab publication, stage the other tabs using
the same transaction before committing. Human edits to participating label
owners, or filter drafts read inside the transaction, cause the whole commit to
reject. Abort unused transactions explicitly; open candidates expire after
60 seconds. The finite in-memory terminal history is not a durable receipt store.

For exact replacement, stage `label.clear` followed by `label.set` in one
transaction. Omitting `transaction_id` on a label write uses a one-command
transaction. `expected_revision`, when supplied, is a canonical decimal **string**,
not a JSON number. The protocol's request ID is only response correlation, not a
deduplication key. Exit code 5 means the write outcome is unknown: inspect the
transaction and current state instead of blindly replaying it.

`tab.get` reports `filter_draft`, not an applied query or completed results.
Read pane metadata and explicitly request retained content separately:

```sh
kloggctl --instance INSTANCE pane get --tab TAB --pane main
kloggctl --instance INSTANCE call source.capture --params \
  '{"tab_id":"TAB","pane":"main","scope":"presented"}'
kloggctl --instance INSTANCE call source.read --params \
  '{"snapshot_id":"SNAPSHOT","start":"0","count":1}'
kloggctl --instance INSTANCE call operation.get --params \
  '{"operation_id":"OPERATION"}'
kloggctl --instance INSTANCE call source.release --params \
  '{"snapshot_id":"SNAPSHOT"}'
```

Use IDs returned by the preceding responses. Poll the operation until terminal;
`operation.cancel` requests cancellation using the same `operation_id`. The
snapshot's row ordinals are not interchangeable with pane ordinals: a folder
presentation can contain headers omitted from a content snapshot. `matches`
scope is supported only on the results pane, using its provider's frozen match
membership, not a new search over the current filter draft.

Marks/global-highlighter mutation and source-addressed navigation/selection are
not yet exposed. `call` does not bypass the server's supported-method list.

## Remaining scope

These slices do not constitute the complete product. Transactional marks/global
highlighter writes, submitted/applied filter state, source-addressed navigation
and selection, asynchronous open/filter/close preparation, and packaged
acceptance remain. The developer CLI supports the observation, bounded content
read and label surface above. Label transactions do not
promise source/file or filter-result transactional mutations, global durability,
or event subscription.
