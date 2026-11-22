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

## Slice 15: asynchronous global-highlighter transactions

`highlighter.list` observes the actual global collection; `highlighter.replace`
replaces its ordered sets/rules and active IDs. The label palette and label
matching defaults are observed but preserved by replacement. Both methods accept
an optional `transaction_id`; replacement accepts an optional canonical decimal
string `expected_revision`. These are application-instance-global operations,
not tab operations, and reject `tab_id` or unknown fields.

The same `ReviewTransaction` coordinates global state and per-tab labels:

- Capture on the GUI owner thread, including detaching objects that own mutable
  lazy matcher caches. Immutable payload and compiled databases may still share.
- Validate the entire candidate; compile privately using the captured regex
  backend on a worker. A process-wide `AsyncWorkBudget` admits two preparations;
  canceled work retains its permit until the worker actually retires. Content
  reads use the same lease abstraction with their separate four-worker budget.
- Return `preparing` immediately. `QFutureWatcher` resumes final validation and
  publication on the GUI thread even if the client does not poll. Human edits,
  target detachment/closure, changed backend policy, abort, expiry and control
  reset reject unpublished candidates without publishing a label prefix.
- Install all owners and mark the transaction terminal before notifying any
  observer. The application-owned `HighlighterStatePublisher` saves the normal
  settings cache before notifying all real file, folder and live views. It owns
  no duplicate configuration or revision. Persistence is not crash-durable sync.
- Human menus/dialogs use this same publisher. Menus rebuild on their next show,
  never by deleting the currently triggered action. Publication changes no focus,
  selected tab or scroll anchor and preserves the existing 33 ms refresh policy.

`transaction.get` returns terminal status and a structured `error.code` for failed
work. Preparing records retain their admission slot and 60-second lease. Further
staging/transactional reads during preparation return `TRANSACTION_PREPARING`;
commit/get/abort remain available. An implicit highlighter replacement returns a
transaction ID as well, because its final outcome is asynchronous. Terminal
receipts remain finite, in-memory records; no retry/durable replay promise is added.

Admission bounds apply before deep capture/compilation: 128 sets, 1,024 rules,
128 active IDs, 128 palette entries, and 1 Mi UTF-16 units of aggregate semantic
text. Existing malformed IDs/colors remain inspectable when within resource
bounds. Oversized GUI imports return `RESOURCE_LIMIT`; JSON output is additionally
limited to 512 KiB after escaping, with transport-envelope headroom. Colors are
serialized as `#aarrggbb` (or null for an invalid stored color). New rules require
valid colors and use strict boolean/integer fields; `color_variance` is 5..99,
matching the GUI editor. Invalid regex syntax is rejected during preparation,
not silently installed or exposed through exception text.

Verification:

- Captured-backend regression RED reproduced zero matches after changing the
  global engine preference; matchers now use their compiled backend.
- Move-authority RED caught a moved-from prepared object clearing the live owner;
  moving now invalidates the source. Snapshot-address RED caught shared mutable
  cache owners; owner-thread detachment removes that sharing.
- RPC RED: **4 cases / 7 failing assertions**. Initial GREEN: **4 / 135**.
- Bounded-read RED covered oversized imported collections and JSON escaping;
  semantic admission and serialized-output limits now reject them before reply.
- Combined unit regression: **71 cases / 11,257 assertions**. GUI regression:
  **35 cases / 1,158 assertions**, including a real CLI child-process replacement,
  optimistic rejection, no-client-poll completion, reset/expiry, and an observer
  destroying the adapter during publication.
- Qt5 syntax parity passed for 12 changed production/test translation units with
  the configured warning policy. It caught two ambiguous QStringList assignments
  (one pre-existing in the control adapter fixture), corrected with explicit types.
- After the shared work-budget extraction, isolated content acceptance passed
  **8 / 227** on Qt5 and **8 / 221** on Qt6 ASan/UBSan (`detect_leaks=0`). These
  assertion counts depend on semantic polling; neither run is a full-GUI sanitizer
  result.
- A separate full native GUI build with ASan and UBSan then passed the same
  targeted unit selection (**71 / 11,251**) and GUI integration selection
  (**35 / 1,158**), including real CLI IPC and observer-driven adapter destruction.
  No sanitizer diagnostics were reported. Leak detection was explicitly disabled;
  Vectorscan was OFF because of the previously recorded host toolchain issue.
  This is macOS/Qt6 evidence, not Windows/Linux or Vectorscan-enabled acceptance.

## Slice 16: CLI package payload and deployment contracts

The CLI now follows the GUI runtime component and bindir on non-macOS installs,
including build recipes requesting only the GUI target. Windows staging fails
closed if `kloggctl.exe` is absent, and installer/uninstaller payloads include it.
The portable archive uses the same staged release directory. macOS deployment
passes the bundled CLI explicitly to `macdeployqt`; AppImage deployment similarly
includes the extra executable's runtime dependencies.

Final macOS bundle validation checks both executable architectures/runpaths and
runs CLI help/version without developer loader paths. AppImage acceptance runs
those commands directly on the extracted sibling executable, without relying on
AppRun environment initialization. CLI lookup remains the same sibling path used
by shell-installation settings.

The packaging agent ran **149 Python contract tests**, including actual inert
CMake install fixtures and recorded macOS deployment execution. Package-runtime,
CI-quality, repository-hygiene and scoped platform lint passed, as did shell
syntax validation and the targeted workflow actionlint run. Unrelated existing
workflow ShellCheck/concurrency-schema diagnostics remain outside this change.
Native relocated bundles/DMGs, signing/notarization, Linux DEB/AppImage runtime
closure, Windows installer/portable execution, and packaged CLI-to-GUI IPC are
still unverified. These contracts are not a substitute for native acceptance.

## Slice 17: stable pane identities, including kept results

Every actual `AbstractLogView` has a non-persistent runtime pane ID. The common
crawler facade enumerates actual result-tab widgets in their current order and
resolves those IDs without a second registry. File/live and folder widgets retain
their own source-availability rules; an unselected folder main pane is listed but
unavailable. IDs survive page switches, reordering and source updates, but retire
when the pane is removed. They identify widgets, not source incarnations.

`pane.list {tab_id}` reports these IDs, role/title/current-page flags, and the
existing payload-free metadata. `pane.get` and `source.capture` accept either
`pane_id` or the existing `pane: main|results` alias, never both. The alias remains
a live current-page observation; an explicit ID can capture an inactive kept
pane without selecting it. Closed/foreign IDs return `PANE_NOT_FOUND`. Captured
source leases remain valid independently of pane lifetime. Main panes still
reject match-only capture. Pane lists are capped at 128 entries and metadata
responses share the 512 KiB serialized response limit.

This establishes addressing needed by marks, filters and navigation; it does not
yet expose those mutations. A pane ID alone is not a source-row precondition.

Verification: core RED **3 cases / 3 assertions**, RPC RED **2 / 2**. Nearby
normal-build tests passed **40 unit / 754 assertions** and **37 GUI / 1,221**,
including real CLI capture by ID. Full native macOS Qt6 ASan/UBSan passed the same
selections (**40 / 748**, **37 / 1,221**) with no diagnostics and leak detection
disabled. The extended Qt5 syntax probe passed 17 production/test translation
units. Determinism and scoped platform lint passed. Kept-folder tests explicitly
arm search completion observers before triggering work; they do not sleep to
infer readiness.

## Slice 18: mark-owner observation and read dependencies

`ReviewMarksOwner` is a UI-neutral facade over existing authoritative stores:
`LogFilteredData` retains its compressed source-model marks, and
`FolderCrawlerWidget` retains its shared per-file marks. It owns only an opaque
runtime identity, not another mark collection. Pane metadata now includes
`marks_owner_id`: file/live result models have distinct owners, and the main view
uses the current result model's owner; all folder panes share the folder-tab owner.

`mark.list {tab_id, owner_id, transaction_id?}` reads that exact owner without
opening sources, decoding lines, navigating, or changing focus. It returns
`revision`, `scope`, and ordered `groups` of `member_path` (null for a single
source model) and decimal-string `rows`. Coordinates are explicitly
`stored-source-model-zero-based`: these are the existing stored marks, **not**
stable evidence addresses. Hidden marks and marks for absent folder members are
retained and observable. Future source-backed mutations must validate retained
row identities rather than interpreting these paths/indices as proof.

Snapshots admit at most 4,096 marks, 128 members, and 1 Mi UTF-16 member-path units
before copying; serialized replies use the common 512 KiB observation limit.
Transactional reads are repeatable, share the 64-owner budget, and detect mark
revision changes (including ABA) before any staged label/highlighter publication.
Commit also validates that each owner still belongs to the requested document.
Switching away from a kept result model does not rebind its owner ID. Closing its
pane or document retires its membership even if destruction has not happened yet.
Mark writes are not exposed in this slice.

Two shared-model/lifecycle issues were fixed during this work:

- Mark membership edits previously decoded log lines immediately to update width.
  Width projection now measures only surviving additions when layout explicitly
  requests it, retains incremental maxima, and rescans once if the measured
  maximum was removed. No-op edits do not decode. Failed measurements do not
  publish a partial cache. Existing source-width fallback remains unchanged.
- `CrawlerWidget::closeFilteredView` previously left a closing pane in the tab
  registry until `deleteLater`. Closing a pane and destroying its parent before
  that event ran caused its `destroyed` callback to access the already-destroyed
  `filteredViewsData_` member. A native stack sample confirmed that path. Close
  now removes tab membership immediately while retaining the model until view
  teardown; the parent disconnects child callbacks before member destruction.
  This preserves worker/data lifetime without keeping the closed target public.

TDD evidence: width RED **2 cases / 9 assertions**; native owner snapshot RED
**2 / 2**; transactional read RED **3 / 5**; RPC owner discovery RED **2 / 3**.
The added kept-file test reproduced stale membership and a teardown SIGSEGV; the
stuck fatal-handler process was sampled and stopped, not counted as passing.
After the fix, `[control-marks-rpc]` passed **3 / 129**. Broader normal-build
verification passed **71 unit cases / 1,116 assertions** (marks, panes,
transactions, filtered snapshots) and **37 GUI cases / 1,207 assertions**
(control, marks, kept results, and existing final-view/close-view regressions).
The full native macOS Qt6 ASan/UBSan build passed those same selections
(**71 / 1,116**, **37 / 1,207**) with no sanitizer diagnostics; leak detection was
disabled. Qt5 syntax checks passed 19 changed production/test translation units.
Scoped platform, determinism and repository-hygiene lint passed. Native
Windows/Linux acceptance remains unrun.

## Slice 19: source-backed mark preparation foundations

Internal source resolution now maps retained evidence identities to native mark
locations without decoding or reopening paths. Source-model owners use the
source's identity mapping, not segment-local ordinals; live rolling retention
can move a surviving identity to another stored row. Folder owners resolve both
retained scan members (including hidden/nonmatching rows) and indexed main-view
cache members without navigation or LRU changes. Old incarnations are rejected.
`ReviewContentAdapter::resolveRows` admits at most 256 lease ordinals, validates
the entire range before metadata access, rejects synthetic rows, and returns
only value metadata. It neither reads payload nor grants additional content pins.
These are internal seams, not new content or mark-write RPC methods.

Both native projections now support private preparation:

- Folder row/layout construction is a pure value pipeline reused by normal GUI
  rebuilds and private preparation. Captures retain immutable groups and only
  the candidate marks' source leases, avoiding copy-on-write detachment of the
  entire streaming source registry. Bounded candidates and a conservative
  262,144-row projection upper bound are admitted before preparation. An opaque
  publication guard validates owner identity, marks binding/value, groups,
  collapse, visibility, membership restrictions, and relevant retained sources.
  Installation is single-use and silent; the folder widget must publish the
  matching store and release **all** pane guards before notifying observers.
- File/live context membership construction is a pure value step shared with the
  existing GUI path. Private work prepares the native compressed marks/union and
  applicable context vector. Admission limits candidate count, compressed input
  bytes (8 MiB), and worst-case context expansion (262,144 row visits). Publication
  validates owner/revision, matching membership, display inputs, and source
  continuity, then moves prepared values without allocation or notification.
  One changed replacement advances the native revision once; no-ops are neutral.
  Safe sealed append remains admissible unless a context range was clipped at
  the old cutoff. Replacement, rolling rebasing, changed mutable final records,
  and newly requested out-of-cutoff rows are not silently accepted. Existing
  stored marks can still be preserved or removed after a cutoff change.

The guards are owner-thread critical sections, never leases across event-loop
turns; callers must keep native owners alive until guard release. These native
per-owner bounds are **not yet an aggregate transaction budget**. Mark-write
staging, cross-owner coordination, GUI publication wiring, and source-backed RPC
methods remain to be connected; this slice does not expose writable marks.

TDD evidence: source resolution RED **2 cases / 2 assertions**; lease metadata
resolution RED **3 / 6**; private folder preparation RED **3 / 3**; private source
preparation RED **3 / 3**; additional unobserved-row admission RED **1 / 1**.
Before extraction, folder layout characterization passed **51 / 720**, and
context/mark integration characterization passed **16 / 382** under native
ASan/UBSan. Final normal verification passed **111 unit cases / 1,770 assertions**
and **18 GUI cases / 434 assertions**. The same full native macOS Qt6 sanitized
selections passed **111 / 1,766** and **18 / 434**, with no sanitizer diagnostics
and leak detection disabled. Qt5 syntax checks passed 24 production/test
translation units. Determinism, repository hygiene, and scoped platform lint
passed. Windows/Linux runtime acceptance remains unrun.

## Slice 20: source-backed mixed mark transactions and RPCs

`ReviewMarksOwner::prepareReviewMarks` now captures native private work through a
shared, UI-neutral preparation/publication contract. File/live owners install
compressed marks, union and context together; folder owners capture every kept
results pane, validate the participant set, acquire every pane guard, swap the
existing shared marks map, and install all layouts before releasing guards.
Notifications are separate and single-use. File GUI edits and programmatic
publication share the model's `marksChanged` path, which refreshes the actual
owning pane, preserves inactive-document deferral, and never activates a kept
page. Folder publication uses its existing coalesced presentation path without
rebuilding prepared layouts.

`ReviewTransaction` stages Add/Remove/Replace/Clear with read-your-writes. Added,
removed and replacement rows carry retained identities and their resolved native
locations. Both are revalidated before preparation and final publication;
surviving live identities cannot silently move to another stored ordinal.
Mark revisions, source/owner lifetime and relevant projection state participate
alongside labels and global highlighters. Every fallible preparation and guard
acquisition precedes any installation; all owners install and release guards
before callbacks. Net no-ops remain revision-neutral.

One transaction shares a **64 MiB conservative metadata-work allowance** across
native mark projections; this is not an RSS/source-lease guarantee. The existing
two-preparation process admission now covers mixed work and remains held while
completed results await publication/retirement, not only while workers execute.
Mark candidates retain the 4,096-mark, 128-member and 1 Mi member-text bounds;
retained proof history is also bounded to 4,096 distinct identities and 1 Mi
member-path text units across the transaction. Normal bounded owner admission
still applies to Clear; it is not an oversized-state recovery bypass.

New RPC methods:

- `mark.add`, `mark.remove`, `mark.replace`: `tab_id`, `owner_id`, `snapshot_id`,
  and **1..256 canonical decimal-string snapshot row ordinals** in `rows`.
  Optional `transaction_id` joins an explicit transaction; optional
  `expected_revision` checks the captured native revision. Replace replaces the
  entire selected mark owner, not only marks from the supplied source.
- `mark.clear`: `tab_id`, `owner_id`, and the same optional transaction/revision
  fields. It requires no client source lease and rejects extraneous row/path
  arguments.

These methods never accept raw member paths/stored model indices as evidence.
Lease metadata is resolved without payload reads; synthetic, out-of-range,
expired and unavailable sources fail closed. Staging retains value proofs rather
than extending the client's content lease. Autocommit returns a transaction ID
and may report `preparing`; server completion finalizes it without client polling.
Closing a target, reset/disable, expiry and conflicting human edits cannot publish
late work. The real console process exercises source capture, mark mutation,
terminal observation, native mark reads and revision-conflict exit code 4.

A dispatch defect was also reproduced and fixed: unknown methods and rejected
early observation/content routes carrying a valid `transaction_id` previously
left an earlier staged prefix committable. Explicit transaction context is now
bound before every route; an error aborts an open candidate consistently.
Canonical unsigned parsing is shared by mark ordinals and revision checks.

TDD: native owner publication, GUI refresh and mixed mark staging each had
behavioral RED before implementation. GUI refresh RED reproduced all three
current/hidden/kept-pane cases; mark RPC RED showed unsupported methods and
committable rejected prefixes. Final normal acceptance passed **121 unit cases /
2,039 assertions** and **52 GUI cases / 1,895 assertions**, including the real
CLI subprocess. Full native macOS Qt6 ASan/UBSan passed those selections
(**121 / 2,033**, **52 / 1,895**) with no sanitizer diagnostics and leak detection
disabled. Qt5 syntax passed 26 production/test translation units; determinism,
repository hygiene and scoped platform lint passed. The developer GUI and CLI
were rebuilt. Native Windows/Linux and relocated-package execution remain unrun.

## Slice 21: private document preparation and nonblocking source retirement

Native lifecycle foundations are implemented; **`tab.open` and `tab.close` are not
exposed yet**. Normal file/folder opening and session restore now reuse
`Session::PreparedDocument`: allocate an unpublished widget/model graph, arm
loading observers, start the native source, and explicitly adopt into its owning
session/window. Preparation does not insert session membership or history.
Ownership covers factory/setter exceptions, fixing both leaked unpublished views
and phantom window entries left by failed factories. Adoption still permits a
loading file for the ordinary GUI; transaction publication must additionally
validate successful readiness.

`TabbedCrawlerWidget::SelectionPolicy::PreserveCurrent` inserts a background tab
without a transient select-and-restore. An empty workspace naturally selects its
first page without activating its window. FileWatcher registrations now belong to
individual source owners: repeat indexing is idempotent, and cancelling a private
source cannot remove another source's watch of the same path. Polling preserves
these ownership claims.

`OperationQueue::beginRetirement` closes admission, clears pending work and
requests interruption without joining. `LogData::retire` completes a future only
after the existing owner-thread completion path joins the finished worker,
detaches its reader and clears queue state. Late source progress/watch callbacks
are suppressed, including retirement requested reentrantly by a normal loading
observer. `PreparedDocument::retire` permanently rejects start/adoption while the
caller retains the graph until its source has quiesced. This is deliberately **not
full public-document retirement**: kept searches, QuickFind, match selection and
folder scan/cache workers still need their own coordinated drain. GUI graphs must
not be destroyed on worker threads or joined in the commit boundary.

TDD evidence: the queue regression holds the real indexing-data mutation lock
while requesting retirement, proving the request does not join that worker. RED
also reproduced replayed pending work, premature completion, post-cancellation
adoption and phantom factory-failure membership. Focused normal and native macOS
Qt6 ASan/UBSan acceptance passed **13 unit cases / 230 assertions** and **33 GUI
cases / 1,966 assertions**, including real CLI and watcher lifecycle coverage.
Leak detection was disabled. Qt5 syntax passed 36 production/test translation
units. These results precede the additional focus/backend changes below.

`CrawlerWidget::InitialFocusPolicy::Preserve` also suppresses both immediate and
deferred initial-load focus changes. The default retains ordinary interactive
opening behavior. A same-window editor regression exercises both presentation
modes and both policies; RED reproduced the two unwanted focus transfers, and
normal nearby acceptance passed **59 GUI cases / 1,937 assertions**.

Single-pattern `RegularExpression` now captures its compilation backend just as
multi-pattern highlighters do. RED reproduced incorrect plain, inverse and
boolean matching when preferences changed between compilation and matcher
creation. Matchers no longer reread that mutable preference. Native Vectorscan
execution remains unverified in the current Vectorscan-disabled development build.

Combined acceptance after the focus/backend changes passed **59 unit cases /
1,024 assertions** and **91 GUI cases / 3,946 assertions** in both normal and native
Qt6 ASan/UBSan builds, with leak detection disabled. Qt5 syntax passed 37
translation units. These selections include real CLI, native watcher lifecycle,
filtered-search and presentation regression cases.

The next lifecycle boundary is a coordinated, callback-silent install followed by
one publication per affected window. It must defer native tab/current-selection
and global group notifications, reconcile all inventories before mux broadcasts,
and retire extracted graphs asynchronously. Wrapping the current modal/activating
MainWindow open/close methods does not satisfy this boundary.

## Slice 22: coordinated native tab notification barriers

`TabbedCrawlerWidget::beginPublication` now acquires a single-owner notification
barrier. Native Qt tab-bar/stack wiring keeps running, while the outer tab widget's
selection signal and explicit inventory/add/reorder notifications are withheld.
The coordinator can install multiple workspaces, release all barriers, then notify
once per semantic notification kind. Selection publication compares the final
widget and index; an empty/no-op barrier is silent. Normal interactive paths keep
their existing synchronous notifications when no barrier is held.

This is a **tab-facade notification boundary, not a complete workspace transaction
or rollback facility**. Session extraction/adoption, global group changes, cached
inventories, mux reconciliation and document retirement still require the native
workspace participant. Merely holding this barrier does not suppress arbitrary
child-view events or global group notifications.

TDD RED observed 14 failed assertions: synchronous first-tab selection, prefix
inventory callbacks, repeated inventory updates, and ineffective owner claims.
GREEN proves two owners are fully installed before the first tab notification,
coalescing and one-time notification, restored prior signal blocking, and safe
owner/publication destruction from observers. Normal and native Qt6 ASan/UBSan
passed **19 unit cases / 126 assertions** and **32 GUI cases / 1,543 assertions**,
including existing tab behavior and real CLI integration. Leak detection remained
disabled; Qt5 syntax passed 37 translation units. Scoped platform lint passed.
The developer GUI/CLI were rebuilt for isolated relocated-bundle acceptance.

## Slice 23: relocated native macOS Qt/CLI acceptance

A fresh developer GUI/CLI bundle was copied to a private directory and moved under
a path containing spaces. Native x86_64 Cocoa startup and the bundled
`kloggctl --version` / `--help` passed with developer Qt/DYLD search overrides
removed. The complete Qt dependency, architecture, executable runpath and runtime
image checks passed after deployment. The optional offscreen plugin was not
deployed, so its optional package smoke was skipped, not counted as passed.

The local Homebrew SDK needed two deployment-specific repairs in the **private
copy only**: `macdeployqt -libpath=<Qt SDK>/lib` resolved optional PDF/virtual-keyboard
frameworks omitted by its initial search paths, and remaining absolute library
install identifiers were normalized to `@rpath/<relative framework image>` before
local ad-hoc signing. No system SDK files were changed. This verifies the relocated
Qt/CLI payload, not the full release pipeline, native device sidecars, signing
identity/notarization or a mounted DMG. Native Windows/Linux packages remain unrun.

Native relocation also exposed two verifier gaps. Runtime image parsing now
recognizes complete modern/legacy dyld lines without splitting paths at spaces,
and uses path containment rather than a string prefix (a sibling `klogg.app.other`
is not the application). Five failing assertions preceded the fix. Smoke tests
also override inherited portable-settings and capture-coordination roots inside
their private temporary home; changing HOME alone was not sufficient isolation.
That inherited-path regression had its own behavioral RED. The verifier's 11
Python tests passed after both fixes, and the repaired verifier passed the native
relocated Cocoa/CLI run.

## Slice 24: nonblocking filtered-search retirement

`LogFilteredDataWorker::retire` closes full/incremental admission, cancels queued
and coalesced requests, and lets the dedicated dispatcher drain the operation
thread. A named owner-thread completion joins only the dispatcher's final return
before acknowledging retirement. It suppresses queued search progress/completion;
ordinary reusable `waitForDone` behavior is unchanged. Blocking teardown and
asynchronous retirement share one shutdown-request path rather than duplicating
cancellation logic.

`LogFilteredData::retire` additionally cancels pending presentation, rejects late
search requests/source callbacks, and acknowledges completion only after detaching
its source reader. Destruction also completes an outstanding retirement future
without depending on another GUI event turn. The source must still outlive its
filtered model. This is a resource-lifetime primitive, not tab removal or a
replacement for the eventual aggregate document-retirement owner.

TDD uses the existing immutable snapshot read gate: cancellation returns while a
real search is blocked, no replacement starts, and the reader remains attached
until the gate is released and native work has quiesced. RED reproduced premature
completion, renewed admission/generation changes and an undetached reader. Normal
and native Qt6 ASan/UBSan passed **58 unit cases / 1,045 assertions** and **88 GUI
cases / 3,439 assertions**, including filtered snapshots, mixed transactions and
real CLI flows. Leak detection remained disabled; Qt5 syntax passed 39 translation
units. The nearby Python package suites passed 86 tests (5 CLI, 11 verifier,
6 AppImage and 64 source-asset contracts); the source-asset suite uses unittest
discovery so its sibling helper imports are available under isolated Python.

Quick Find also has a terminal, nonblocking retirement request backed by its
existing owned-task completion. It rejects subsequent ordinary/incremental
navigation and suppresses queued results/status messages while retaining the
owner until accepted work releases its captures. Its blocked-read RED reproduced
five failures; normal and native Qt6 ASan/UBSan acceptance passed **13 unit cases /
197 assertions** and **59 GUI cases / 1,937 assertions**.

## Slice 25: whole-document resource retirement

`AbstractLogView::retireSearches` closes navigation, rebinding, match selection
and copy admission, cancels publication and returns owned-task completions.
`FolderSearchEngine::retire` similarly drains its coordinator/scanner without
joining from the initiating GUI call. The existing isolated enumeration quarantine
remains value-owned; it does not retain a widget or mutable source model.

Both concrete crawler widgets implement `retireResources`. Files cover the indexed
source, every filtered model and all view tasks, including panes removed from tab
membership but still awaiting deferred deletion. Folders cover their shared search
engine, every kept/main view and distinct placeholder, pending, current and cached
file sources. Late presentation/source callbacks cannot restart work. The widget
claims pending pane deletion before cancellation callbacks so a pre-existing
`deleteLater` cannot force a blocking destructor while accepted work is draining.
Live transport shutdown is deliberately outside this resource-only interface.

Deterministic RED tests held real work behind the shared owned-task pool gate or
blocked the folder scanner at its started signal. GREEN verified completion
aggregation, closed admission, suppressed publication and safe final destruction.
The shared gate was extracted only after its characterization passed. Combined
normal unit acceptance passed **209 cases / 3,091 assertions**. Native Qt6
ASan/UBSan passed the same unit suite and **169 GUI cases / 4,675 assertions**;
normal broad GUI acceptance passed **166 cases / 4,168 assertions**. Leak detection
remained disabled. Qt5 syntax passed **46 translation units**, alongside scoped
platform-fragility, test-determinism and repository-hygiene lints.

These are lifetime primitives, not yet transactional tab open/close. Session
ownership of retired graphs and coordinated workspace publication are still being
integrated; no lifecycle RPC or nonblocking public close claim follows from them.

## Slice 26: Session-owned retirement and exact document extraction

`Session::retirePrepared` transfers an unpublished file/folder graph to native
owner-thread retirement. Future watchers, rather than a waiting pool task or
polling timer, retain the view and source models until every accepted task has
released its captures. Final destruction preserves view-before-model order. The
entry detaches from its queue before destroying the graph, and the queue is moved
out of Session before a shutdown drain, making destruction-observer reentry safe.
There is no strong callback cycle retaining Session. Application shutdown closes
IPC admission before explicitly draining private graphs while Qt is still alive;
ordinary cancellation does not invoke that blocking fallback.

`WindowSession::extractDocument` removes exact native membership and returns an
owning terminal graph without UI calls, model destruction or worker waits. The
caller must first remove and detach its view from presentation. Foreign-window,
parented, missing and live targets are refused; live transport policy is not
bypassed. Allocations precede ownership transfer. The extracted graph cannot be
restarted or re-adopted and uses the same retirement queue as a cancelled open.

A duplicate-path RED exposed why path-only window membership was insufficient:
closing another window's view removed one shared-session entry and the wrong
window's path entry. Native membership now stores view identity, not filenames;
metadata enumeration also directly resolves that exact record instead of scanning
all documents for an arbitrary matching path. Existing persistence remains
path/spec-based and unchanged. File/folder extraction RED plus duplicate-window
RED produced four failing assertions. Initial GREEN passed **55 unit cases /
1,052 assertions** and **186 GUI cases / 6,434 assertions**, including ordinary
session restore, file/folder/live close, tab batches and CLI flows. Native Qt6
ASan/UBSan passed **55 unit cases / 1,052 assertions**, **187 GUI cases / 6,480
assertions**, and the existing **30 live-restore cases / 407 assertions**.

A final ownership RED caught competing Qt-parent ownership of a private graph.
Retirement now rejects it without consuming the candidate; extraction and
retirement share the same ownership predicate. Additional Android/iOS boundary
tests confirm that extraction never bypasses live transport close policy. Final
normal and ASan/UBSan acceptance passed **56 unit cases / 1,061 assertions** and
**31 live-restore cases / 423 assertions**. Leak detection remained disabled.
Qt5 syntax passed **47 translation units**; scoped platform-fragility,
test-determinism and repository-hygiene lints passed. The obsolete document-id
move assertion/include was removed after switching native membership to pointers.

These APIs are not yet wired into transactional workspace membership or lifecycle
RPCs. Coordinated inventories, mux reconciliation and notification publication
remain required before exposing `tab.open` or `tab.close`.

## Slice 27: prepared global tab-group removal

`TabGroupManager::PreparedRemoval` captures a private candidate, validates the
native mutation identity, installs by nonthrowing ownership swaps without signals,
and publishes explicitly once. No-op removals preserve the mutation identity.
Native mutations and storage reload advance a retained identity, detecting ABA
without a wrapping counter. Public group lookups are read-only so callers cannot
bypass this boundary. Ordinary single-path removal reuses the same producer;
`ungroupAll` no longer clears a list immediately before deleting that group.

The tab-group model alone cannot install Qt presentation. A composition test holds
both tab-facade publication guards, installs the global model, reconciles both
facades using the existing group slot, releases the guards, then notifies. Its
global observer is deliberately registered before either facade: it verifies that
coherence does not depend on Qt slot connection order. This is not yet the full
MainWindow workspace coordinator or a claim of one OS paint event.

Characterization passed **4 unit cases / 27 assertions** and **8 GUI cases / 158
assertions** before extraction. Two behavioral RED cases established the missing
prepared installation boundary. GREEN and boundary coverage include ABA, no-op
stability, all native mutation routes, storage reload, abandoned candidates, owner
loss, and an observer destroying both the prepared update and its owner. Combined
normal and native Qt6 ASan/UBSan acceptance passed **52 unit cases / 989 assertions**;
normal GUI acceptance passed **187 cases / 6,462 assertions**, and ASan/UBSan GUI
acceptance passed **188 cases / 6,508 assertions**. Leak detection remained disabled.
Qt5 syntax passed **49 translation units** with the scoped lints. A final refactor
makes the nonthrowing install contract explicit and avoids detaching a group list
for absent paths. Normal and ASan/UBSan rechecks both passed **52 unit cases / 989
assertions**; the 49-unit Qt5 syntax probe and scoped lints passed again.

Source preparation still needs an authoritative readiness boundary before lifecycle
RPC adoption: native indexing currently treats an unopenable source as an empty
successful load, and owner-thread file-holder acquisition can perform filesystem
IO. Neither a loading-finished boolean nor calling the existing interactive open
method is sufficient for the transactional-open contract.

## Slice 28: strict native file acquisition

`openRegularFileReadOnly` acquires a read-only source, validates the opened object,
and returns handle-derived identity, size and modification time. A real zero-byte
file is distinct from missing/unavailable input. It never creates the source and
rejects already-open input without taking ownership. POSIX nonblocking acquisition
rejects a FIFO without waiting for a writer. The Windows path requests shared
read/write/delete access without backup-privilege flags; a kernel-level directory
open refusal remains an ordinary failure. Native Windows execution is not claimed.

The legacy generic read-only open retains its behavior. Both read-only acquisition
paths now share descriptor-transfer RAII, covering failure before QFile assumes
ownership. The acquired handle continues to identify/read the original object
after its pathname is replaced. A dedicated test helper isolates FIFO/device
rejection so a missing nonblocking guard cannot strand the test runner's pool.
Its process completion is the success condition; the timeout only bounds failure.

Behavioral RED produced **4 cases / 7 failing assertions**. Initial GREEN, including
the existing generic-open contract, passed **5 cases / 89 assertions**. The final
normal and native Qt6 ASan/UBSan nearby suites both passed **52 cases / 995
assertions**, with leak detection disabled. Qt5 syntax passed **52 translation
units**, and **37 existing static-analysis contract tests** plus scoped lints
passed. This is an acquisition primitive: the caller still must dispatch it off
the GUI thread and reuse native indexing in a strict, nonmodal preparation path.
It is not yet a delivered `tab.open` implementation.

## Slice 29: immutable indexing settings

`IndexingSettings` captures default encoding, prefetch capacity, compressed-index
selection and modification-detection policy on the owner thread. Native full,
partial and file-check dispatch pass that value into their workers. Index resets,
fallback codecs and hashing policy no longer reread mutable preferences mid-work.
The encoding detector accepts an explicit fallback while preserving its existing
UTF-8 fast path: legacy callers still consult preferences only when a fallback
is actually needed. Later GUI choices are not overwritten.

The behavioral RED prepared Latin-1 defaults, changed the current preference to
UTF-8 before execution, and observed **106 instead of 4** from native indexing.
GREEN adds explicit detector-fallback coverage. Normal and native Qt6 ASan/UBSan
passed **34 unit cases / 554 assertions**; normal GUI acceptance passed **184 cases /
5,490 assertions**, and sanitizer GUI acceptance passed **185 cases / 5,540
assertions**. Qt5 syntax passed **56 translation units** and the scoped lints passed.
Leak detection remained disabled. These captured inputs are a prerequisite for
strict private indexing, not yet a complete prepared-source adoption interface.

## Slice 30: finite acquired-handle indexing and retained-byte hashes

`FullIndexOperation::prepare` uses the native parser and retained-content builder
against an already acquired regular handle, stopping at the captured byte extent.
It never reopens the source path, emits the legacy completion signal, or schedules
a modal error dialog. Cancellation, invalid input, short reads and line/resource
limits produce typed terminal outcomes and clear the private index instead of
accepting a partial prefix. Later appends cannot extend preparation indefinitely.

The shared engine now derives header/tail hashes from the retained snapshot rather
than rereading the mutable original. A deterministic normal-indexing regression
rewrote the original at the final progress notification: the retained content was
`first`, but the old hashes described `other`. Both ordinary and prepared indexing
now hash the bytes actually indexed, outside the index mutex.

RED covered **5 cases / 10 failed assertions**, including that ordinary-path hash
mismatch. Focused GREEN passed **19 cases / 286 assertions**. Broader normal GUI
acceptance passed **184 cases / 5,487 assertions**; native ASan/UBSan passed **39
unit cases / 637 assertions** and **185 GUI cases / 5,537 assertions**. Qt5 syntax
passed **58 translation units**; determinism, repository-hygiene and scoped platform
lints passed. Leak detection remained disabled. Native Windows/Linux execution
and linked Qt5 runtime remain unverified. This is still a preparation primitive,
not a published lifecycle RPC or complete GUI source-adoption pipeline.

## Slice 31: worker-owned sources and complete-index adoption

`prepareFileSource` acquires, validates and indexes on the worker using captured
settings. It returns a complete index, original handle and acquisition metadata
only after the indexing graph has quiesced. Missing/non-regular sources,
cancellation, invalid paths and resource failures never become empty successes.
The returned QFile has no thread affinity; unpublished results can be discarded
on another worker without a GUI event-loop turn.

`FileHolder::adoptForReader` transfers that handle into a fresh holder without
reopening or restatting the pathname. The logical path is supplied separately:
Qt descriptor adoption clears QFile's filename, which an initial rebinding test
caught. One preparation reader retains the original object, including its changing
size after rotation. Releasing it restores native keep-closed behavior; subsequent
native readers still follow the logical pathname. Rejection preserves ownership.

`LogData::fromPrepared` creates the model and its future worker against the same
complete IndexingData rather than swapping only the model's pointer or copying an
incomplete snapshot. It installs captured metadata/codec and consumes the source
without watcher registration or a loading notification. The later activation
phase owns preparation-reader release. A replacement-path test observes original retained content, then
native reload updates the same model to the replacement; old snapshots remain valid.

The acquisition-holder RED failed **2 cases / 3 assertions**; the worker-result
RED failed **3 cases / 6 assertions**; GUI model adoption failed **1 case / 1
assertion**. Focused GREEN and the subsequent arithmetic extraction characterization
both passed **46 cases / 783 assertions**. Including the overflow guard below,
normal and native ASan/UBSan passed **48 unit cases / 797 assertions**; normal GUI
passed **184 cases / 5,487 assertions** and sanitizer GUI passed **185 cases /
5,534 assertions**. Qt5 syntax passed **63 translation units**, and scoped lints
passed. Session ready-source composition, explicit activation and workspace
publication remain separate unfinished integration work.

## Slice 32: line-width overflow before resource rejection

Review identified a Qt5 failure before the new resource guard: a line exceeding
`INT_MAX` bytes reached an aborting narrowing conversion inside the parser before
its completed block could be checked. Tab expansion could also overflow signed
native-width arithmetic. The extracted arithmetic was first characterized by the
passing **46-case / 783-assertion** preparation suite. A 32-bit boundary test then
reproduced **SIGABRT / exit 134** from the actual narrowing precondition, without
allocating a multi-gigabyte fixture.

The shared parser now saturates at the native overflow sentinel before narrowing
or tab addition. Multi-byte width is divided in the wide domain before checking
character capacity. Ordinary indexing retains its existing overflow-reporting
policy; private indexing returns its typed resource failure. Tests exercise both
32-bit and 64-bit widths on every host and preserve existing small/negative tab
alignment behavior. The combined normal/sanitizer/Qt5 evidence is recorded above;
no Qt-version conditional was added to business or test code.

## Slice 33: private readiness, explicit activation and catch-up

Prepared LogData now has a small owner-thread lifecycle: ready, privately announced,
then native/active. `completePreparedLoading` emits the ordinary loading signal
once after the private view binds; it performs no acquisition, watcher registration
or metadata lookup. Its observers may retire or destroy the graph. Before activation,
the model does not subscribe to global watcher traffic and rejects native reloads.
A deterministic RED delivered another owner's global path signal and proved that
an unpublished source previously started an unwanted check.

`activatePrepared` registers the native owner once and queues an ordinary change
check before releasing the preparation reader. That pin prevents GUI-thread reopening
while admitting the check. The check closes the captured-snapshot-to-watch gap:
appends before watch registration must not disappear just because registration
establishes a later baseline. RED demonstrated both the missing queued check and
missing appended content (**1 case / 2 assertions**). Separate readiness/isolation
RED failed **2 cases / 2 assertions**. The standalone reader-release API was removed;
activation owns that transition instead of exposing another independently mutable
lifetime flag.

Normal and native ASan/UBSan passed **54 unit cases / 915 assertions**; normal GUI
passed **184 cases / 5,485 assertions**, and sanitizer GUI passed **185 cases /
5,538 assertions**. Qt5 syntax passed **64 translation units**; scoped lints passed.
The prepared-source, line-arithmetic and FileHolder headers also compiled as
standalone translation units under both Qt5 and Qt6. Leak detection remained off.
Session composition and workspace publication are still required before exposing
file-open lifecycle commands through the CLI.

## Slice 34: prepared sources use the shared Session view pipeline

Session and WindowSession accept a completed `PreparedFileSource` and reuse the
same file-graph builder as ordinary opening. The prepared branch consumes the
worker result, binds the complete native source/filter/view graph privately, and
uses `startLoading` only to deliver native initial presentation. It neither reopens
the pathname nor starts another index. Session/window membership still changes
only on explicit adoption, while source activation remains a separate publication
step. Private readiness keeps the source alive across notification and performs
no later access to a document that an observer could destroy.

RED failed **2 cases / 2 assertions**. The replacement-path test proves that the
private view sees the acquired original rather than the replacement, preserves
an external editor's focus, and has no inventory entry before adoption. Cancellation
uses the Session-owned retirement queue; absent worker results never call the view
factory. Normal and native ASan/UBSan passed **56 unit cases / 957 assertions** and
**31 live-restore cases / 423 assertions**. Normal GUI passed **184 cases / 5,487
assertions**; sanitizer GUI passed **185 cases / 5,534 assertions**. Qt5 syntax
passed **65 translation units**, scoped lints passed and whitespace checks passed.
Leak detection remained disabled. Coordinated MainWindow publication and lifecycle
RPC wiring are not delivered by this slice.

## Slice 35: exact-widget tab status ownership

Tab status forwarding now targets a guarded runtime widget, not a document path.
Removed widgets disconnect from their former facade while searches retire. RED
reproduced a late source changing its replacement's status and duplicate-path
widgets updating the wrong tab (**1 case / 3 assertions**). Normal and native
ASan/UBSan passed **32 unit cases / 573 assertions**; normal GUI passed **147 cases /
3,864 assertions** and sanitizer GUI passed **148 cases / 3,911 assertions**. Qt5
syntax and scoped lints passed.

## Slice 36: native MainWindow publication boundary

`MainWindow::Publication` installs prepared Session graphs or extracts detached
file/folder graphs under the native tab facade barrier. Every affected inventory
can be finalized before any window notifies observers. Group projections are
refreshed against silently installed group state; removed descendants disconnect
from the former window's selection/scratchpad/configuration forwarding. Release
and notification are separate, and notification tolerates destruction of the
publication object. This is a native membership primitive, not rollback or a
complete workspace transaction. Policy, history, source activation and retirement
remain the caller's responsibility.

RED failed **2 cases / 2 assertions**. Tests cover cross-window observations,
external-editor focus, surviving selection, prepared files, foreign candidates,
owner loss and asynchronous extraction cleanup. Normal GUI passed **152 cases /
3,932 assertions**; native ASan/UBSan passed **32 unit cases / 573 assertions** and
**190 GUI cases / 5,599 assertions**. Qt5 syntax passed **68 translation units** and
scoped lints passed. Leak detection remained disabled.

## Slice 37: implicit document lifecycle RPCs

`documents.open` and `documents.close` now use the native preparation/publication
pipeline. Opening runs regular-file acquisition/indexing or cancellable folder
enumeration on a bounded worker path. GUI construction restores native view
context and follow-on-load policy without touching the currently routed follow
action. Publication preserves a surviving current tab and never raises a window.
Missing files fail instead of opening as empty; empty folders return `EMPTY_FOLDER`
without a modal dialog. Already-open paths in the requested window are reused,
including a retained source whose pathname has disappeared.

The existing bounded operation registry now also accepts owner-thread completions.
Document preparation shares `operation.get` and `operation.cancel`, has two
process-wide preparation permits and a five-minute lifetime, and becomes terminal
before publication observers run. Global reset cancels unpublished work without
joining workers. Close coordinates native group removal and all-window inventory
finalization, then hands detached graphs to Session retirement. Existing live tabs
remain outside lifecycle mutation. Source bytes are never included in lifecycle
responses.

These are **single-command implicit transactions**. Supplying `transaction_id`
is rejected (and poisons a staged prefix as other unsupported commands do).
Mixed explicit open/close/filter/annotation transactions remain unfinished.
Initial RPC RED failed **3 cases / 4 assertions**; initial GREEN passed **3 cases /
63 assertions**. A later existing-path RED failed **1 case / 1 assertion** before
adding the no-acquisition reuse path. Normal nearby acceptance passed **33 unit
cases / 697 assertions** and **157 GUI cases / 4,023 assertions**. The real CLI
suite passed **6 cases / 288 assertions**. Native ASan/UBSan passed **33 unit cases /
691 assertions** and **195 GUI cases / 5,715 assertions**. After adding deterministic
expiry and destroyed-window coverage, focused normal and sanitizer acceptance both
passed **13 cases / 377 assertions** (including real CLI and native publication).
Qt5 syntax passed **70 translation units** and scoped lints passed. The actual
`klogg` GUI target was rebuilt successfully; no new packaged-release or linked Qt5
runtime acceptance is claimed. Leak detection remained disabled.

## Slice 38: capability inventory and private snapshot search preparation

`capabilities.get` exposes the protocol version, supported method names, feature
flags and selected limits without reading log content. It advertises explicit
label/mark/global-highlighter writes, filter-draft read dependencies and implicit
file/folder lifecycle support. Applied filters, explicit lifecycle transactions,
source navigation, live lifecycle, event subscription and durable transactions
are explicitly unsupported. This is a **method/feature/limit inventory**, not a
complete parameter/result schema or a promise that every request is admissible.

`prepareFullSearch` is a worker-only synchronous preparation primitive over a
retained immutable `SearchableLogData::ReadSnapshot`, a value-owned pattern and
captured `SearchSettings`. Buffer size, matching concurrency, block-scan policy
and regex backend are captured before dispatch, rather than read from mutable
preferences during execution. The primitive reuses `FullSearchOperation` and
returns only after its native tasks drain. Only `Ready` carries a complete
`CompletedSearchResult`; cancellation, invalid input, compilation failure and
execution failure never expose a successful partial prefix. The result retains
its source snapshot and leaves publication identities zero. Cancellation remains
cooperative at native chunk boundaries, and the caller must keep the cancellation
flag alive until the call returns.

This is private search preparation, **not applied-filter publication or a new
search RPC**. It does not compose filter results or lifecycle changes into explicit
annotation transactions. The coalescing fixture repair stops the dispatch consumer
and reopens admission only through a private test visitor; related trace fixtures
use the same helper. Production shutdown and retirement remain terminal and reject
new work.

Recorded verification (cases / assertions; logs under `$CLAUDE_JOB_DIR/tmp`):

- Capability discovery RED: **1 / 1** in `capabilities-red.log`; normal
  capabilities/document/CLI/window acceptance: **14 / 417** in
  `capabilities-green.log`.
- Focused search preparation: **8 / 128** in `search-preparation-green.log`;
  combined search acceptance: **57 / 741** in `search-preparation-refactor.log`.
- Native ASan+UBSan: **68 unit cases / 1,009 assertions** in
  `search-preparation-asan-unit.log` and **198 GUI cases / 5,769 assertions** in
  `search-preparation-asan-ui.log`.
- Qt5 syntax: **73 translation units**, with `FAILED: []` in
  `search-preparation-qt5.log`. This pass predates the last capabilities edits;
  it is not linked Qt5 runtime acceptance or verification of those final edits.

These recorded runs do not establish full release-artifact acceptance.

## Slice 39: compiled query ownership and native search publication

Complete search results now retain their compiled query. This fixes an existing
native cache sequence: search A, search B, restore A, then update A previously
used B's matcher. The regression failed all four expected-row assertions before
the fix. Snapshot preparation also retains its compiled query, avoiding GUI-thread
recompilation when adopting a privately prepared result.

`LogFilteredDataWorker::PreparedResult` is a move-only, preallocated handoff. Its
mutable publication stamp is private; ownership is not inferred from
`shared_ptr::use_count()`. Adoption revokes previous execution identities without
joining blocked work or emitting progress. Native cache restoration and prepared
adoption share result stamping and matcher installation. A first subsequent
update may rebuild the prefix through the existing native fallback; adoption does
not claim to seed the old worker's mutable accumulation state incrementally.

`LogFilteredData` now has capture, worker preparation and guarded publication
stages. Preparation builds matching rows, the marks union and bounded context
projection without changing the model. Admission checks the owner, query
generation, mark revision, visibility/context and exact source cutoff. Source
append is relevant to a full query and requires fresh preparation. Installation
preserves the model identity and stored marks, cancels stale pending publications,
and installs the complete source/result/projection before any signal. Callers
install every owner, release every guard, then explicitly notify. Notification is
one-shot and tolerates an observer destroying the model or publication object.

This is a **native filtered-model boundary**, not UI query-draft publication,
folder-search-result publication, a `filter.apply` RPC, or mixed explicit
lifecycle/filter/annotation transactions. The file and live-source model cases are
covered separately; folder orchestration still belongs at its owning boundary.

Initial RED/GREEN evidence:

- Missing retained matcher: **1 case / 3 failed assertions**; stale cache matcher:
  **1 / 4 failed assertions**. Nearby GREEN: **58 / 763**.
- Nonblocking result adoption RED: **1 / 1 failed assertion**; nearby GREEN:
  **60 / 787** before the ownership handoff refinement.
- Native model publication RED: **4 cases / 6 failed assertions**; initial focused
  publication/search/mark-preparation GREEN: **20 / 367**.

The platform lint also identified two QWidget-local `data` shadowing names and
four direct worker-signal spies in earlier control/retirement work. Locals now use
`sourceData`, and folder-engine completion observations use scoped QObject
receivers with queued delivery. The retired-folder no-navigation test explicitly
asserts its established path before attempting the rejected selection.

The expanded pre-review acceptance passed **83 unit cases / 1,188 assertions**
in both normal and ASan+UBSan builds, **197 GUI cases / 5,732 assertions** normally
and **198 / 5,785** under ASan+UBSan. Qt5 syntax passed **75 translation units**.
These runs predate the following reentrant-notification hardening.

A focused review identified that incremental updates preserve query generation.
A notification observer could start a new update in the same model or another
installed owner, and the old publication would still report terminal progress.
Two deterministic regressions reproduced **3 failed assertions across 2 cases**.
Publications now retain the installed completed-result identity, rejecting stale
notification before touching current timers. Native terminal publication also
checks the request ticket after its results callback before emitting completion
status. Post-fix acceptance passed **88 unit cases / 1,306 assertions** in both
normal and ASan+UBSan builds, **197 GUI cases / 5,732 assertions** normally and
**198 / 5,776** under ASan+UBSan (`search-reentry-*.log`). Qt5 syntax passed
**75 translation units**; test-determinism, platform-fragile and repository-hygiene
lints passed. These checks do not establish native Windows/Linux, linked Qt5,
Vectorscan-enabled or full packaged-release acceptance.

## Slice 40: quiet revisioned filter-draft installation

`SearchToolbar::installFilterDraft` compares the expected draft value and revision,
then installs the full text/options as one semantic revision. Stale edits,
including human ABA, are rejected. No-op installation preserves the editor cursor
and selection and does not advance the revision. Installation blocks both the
combo and its actual editor, as well as the option buttons: it does not focus the
input, emit ordinary edit/option signals, or auto-submit a search. Internal
completer case sensitivity and predefined-filter selection remain coherent.
Normal human option notifications are restored on return.

Detached `FilterSpec::regularExpressionPattern` and the ordinary toolbar query
builder now share the existing native flag mapping. The latter still reads the
widgets directly to preserve existing blocked-setter behavior. This is a shared
file/folder toolbar primitive, not an applied-query/model transaction: its host
must install applied query state in the same non-yielding publication. No new RPC
or capability flag is exposed by this slice.

RED reproduced **3 failed cases / 6 failed assertions**, while the existing native
pattern characterization passed. Initial focused GREEN passed **12 cases / 158
assertions**. Qt5 syntax passed **78 translation units**; test-determinism,
platform-fragile and repository-hygiene lints passed. Final toolbar/transaction
acceptance, including post-install human notifications, passed **40 cases / 744
assertions** in both normal and ASan+UBSan builds. Adjacent GUI acceptance passed
**182 cases / 5,302 assertions** normally and **183 / 5,348** under ASan+UBSan
(`filter-draft-final-*.log`, `filter-draft-asan-*.log`).

## Slice 41: coordinated current-pane crawler query publication

`CrawlerWidget` now composes native model preparation with quiet toolbar and view
publication for a **nonempty replacement in the current pane**. Capture retains
value dependencies, not mutable GUI owners. Worker preparation delegates to the
native search/projection pipeline. Admission checks the tab/pane identities,
search state and bounds, draft revision, and both views' match-selection operation
revisions. Pending or newer human match-selection work is rejected rather than
cancelled by the programmatic publication.

Installation adopts the complete result, desired visibility, draft, applied
pattern, native auto-refresh state, pane title and status presentation before
notifications. Existing marks and pane identities remain intact. Native and
prepared queries share applied-pattern propagation; native hidden-tab deferral
is unchanged. All owners release their model guards before notifications, which
use the installed-result identity and lifetime guards. Recent-search history uses
the committed text even if an earlier observer edited another owner's draft.

The model capture has a desired-visibility overload: native submission can move a
Marks-only pane to Marks-and-Matches without a later separate visible mutation.
Conflict detection still uses the original visibility. Context budgeting and
private projection construction use the desired visibility. Reapplying unchanged
normalized context does not discard the privately built projection during native
terminal UI reconciliation.

At this slice, this was a native boundary, **not a filter RPC or complete explicit
mixed transaction**. Empty-query policy, creation of a new kept-results pane and
folder result publication were not admitted by this current-pane replacement
primitive. Slices 42 and 43 below add native reset and the bounded implicit RPC;
continuous-append admission and combined staged mark/filter projections still
need deliberate composition before that broader surface is exposed.

Recorded TDD evidence:

- Desired visibility RED: **1 case / 2 failed assertions**; nearby GREEN:
  **29 cases / 517 assertions**.
- Crawler publication RED: **3 cases / 4 failed assertions**; initial focused
  GREEN: **22 cases / 471 assertions**. The initial fixture failure from reading
  uninitialized `SavedSearches` was corrected before recording behavioral RED.
- Context retention RED: **1 case / 2 failed assertions**. Final expanded
  acceptance, including multi-owner, failure, stop/refresh and observer destruction,
  passed **109 unit cases / 2,013 assertions** in both normal and ASan+UBSan builds.
  GUI acceptance passed **197 / 5,726** normally and **198 / 5,772** under ASan+UBSan.
- Qt5 syntax passed **80 translation units** after replacing a Qt6-only test
  comparison between QFlags and its enum with the explicitly typed flags value.
  The final assertion-only adjustment was rerun normally: **19 / 457**.
  Test-determinism, platform-fragile and repository-hygiene lints passed. The actual
  GUI executable was rebuilt with this native boundary.

## Slice 42: native empty-filter reset publication

The coordinated current-pane boundary now accepts empty text as a native reset.
It prepares an empty-match projection using the configured empty-filter policy,
then clears the applied pattern and restores `NoSearch` rather than compiling
an empty regex and reporting a completed match-all search. The configured
show-all-when-empty presentation can mirror source rows without turning them
into regex matches. Existing marks and pane identities survive publication.

Reset uses the same guarded capture/preparation/install/release/notify sequence
as replacement. The empty-filter configuration is a captured dependency checked
again at admission; changes invalidate the prepared reset rather than publishing
with stale presentation policy. Source and GUI dependency conflicts leave the
draft and visible model unchanged. This slice remains a native primitive; the
RPC delivery is recorded separately below.

Verified reset acceptance passed **114 unit cases / 2,251 assertions** in both
normal and ASan+UBSan builds. GUI acceptance passed **197 cases / 5,726 assertions**
normally and **198 cases / 5,773 assertions** under ASan+UBSan. These sanitizer
results cover the native reset slice, not the later RPC integration.

## Slice 43: implicit current-pane filter RPC

`filter.apply` now exposes the native current-pane publication boundary for file
and live tabs. `ReviewControlAdapter` resolves the tab and validates the request;
`reviewfiltercodec` merges a strict patch into the captured toolbar draft;
`ReviewFilterAdapter` prepares privately and publishes through the existing native
boundary. It does not create a kept-results pane or accept folder targets or
explicit filter transactions.

The request accepts only `tab_id`, `filter`, and optional `expected_revision`.
The patch accepts string `text` and boolean `match_case`, `use_regexp`, `inverse`,
`boolean`, `auto_refresh`, and `keep_results`. Omitted fields retain their draft
values. Unknown fields, wrong types and embedded NUL are rejected. The revision
is a canonical unsigned decimal string, not a JSON number; a stale revision is
`REVISION_CONFLICT`. The resulting `keep_results:true` is `UNSUPPORTED_OPTION`,
even if inherited from the draft rather than supplied in the patch.

The wire text limit is **32,767 UTF-16 units**. Native draft admission additionally
checks the actual toolbar editor's `maxLength`, preventing silent truncation from
making the installed draft disagree with the prepared query. Oversized wire
patterns fail with `RESOURCE_LIMIT`; a native draft that cannot be installed is
not admitted.

Accepted work returns `operation_id` and `status:"pending"` through the shared
content/document operation registry. `operation.get` observes terminal status;
`operation.cancel` requests cooperative cancellation. Publication proceeds
without polling. A successful result contains only `window_id`, `tab_id`,
`pane_id` and `updated:true`: it does not include log content or create a snapshot
lease. Use explicit `source.capture` and `source.read` for content.

Admission uses `unchanged-snapshot` source validation plus the captured GUI
owners' dependencies. A changed source, including continuous append, can cause
`CONFLICT`; file/live support does not promise eventual publication during a
continuously changing stream. Empty text follows Slice 42's native `NoSearch`
reset, not a completed match-all regex. The RPC updates the current results pane
of the addressed tab without activating a tab or creating a new pane.

`capabilities.get` now advertises `filter.apply`, `applied_filters:true`,
`implicit_filter_kinds:["file","live"]`, the unchanged-snapshot policy and filter
limits. `filter_kept_pane_creation`, `explicit_filter_transactions` and
`applied_filter_observation` remain false. `filter.get` and applied-query
attribution are not delivered; `tab.get.filter_draft` remains draft-only.

Recorded TDD evidence:

- Initial RPC behavioral RED: **4 cases / 8 failed assertions**.
- First RPC GREEN, normal builds: **28 unit cases / 732 assertions** and
  **5 GUI cases / 356 assertions**.
- Editor-capacity regression RED: **1 case / 4 failed assertions**; addressed by
  checking the actual toolbar capacity and aligning the wire maximum to 32,767.
- Final normal and ASan+UBSan acceptance each passed **99 unit cases / 1,957
  assertions** and **211 GUI cases / 8,854 assertions**. This includes the real
  independent CLI process, publication metadata, expiry without polling,
  cancellation, target closure, human edits, pending-delivery resource limits,
  unsupported folder targets and rejection of explicit transaction prefixes.
- Broad GUI acceptance exposed settings-Apply test cleanup leaking Info logging
  into live-presentation tests. The focused two-test sequence reproduced **4
  failing assertions**; restoring the test entry point's Warning logging policy
  passed **2 cases / 154 assertions**, followed by both broad GUI passes above.
- Qt 5 syntax checks passed **83 translation units**, with the final two amended
  test units rechecked. Both new filter headers passed standalone Qt 5 and Qt 6
  syntax checks. Platform-fragility, test-determinism and repository-hygiene lints
  passed. The normal GUI executable was rebuilt with the RPC.
- Sanitizer runs used `ASAN_OPTIONS=detect_leaks=0`; no LeakSanitizer, linked Qt 5
  GUI runtime, native Windows/Linux runtime or new full packaged-release/IPC
  acceptance is claimed.

## Slice 44: per-result-pane applied-query observation

Applied-query attribution now travels through the existing pane inventory and
`pane.get`/`pane.list` responses as `applied_filter`. It includes `text`,
`match_case`, `use_regexp`, `inverse`, `boolean` and native `complete` readiness.
It is available for retained file/live/folder result panes; main panes omit it.
No new `filter.get` endpoint or parallel pane registry is introduced.

File/live values come from each result model's `currentRegExp_`, not the current
widget-wide query. Folder panes now retain their accepted pattern alongside
existing per-pane completion state, with reset/invalid-query paths clearing it.
Changing the draft, selecting an old pane or deferring presentation does not
rewrite another pane's attribution. Observation does not compile patterns,
capture a source or decode content. Completion is not a running/stopped/error
classification and does not promise a later content lease.

The native behavioral RED was **2 cases / 3 failed assertions**, covering file,
live and kept-folder attribution. Native GREEN passed **23 cases / 535
assertions**. The wire observation RED was **1 case / 1 failed assertion**.
After encoding and an additional no-content-read test, focused GREEN passed
**38 unit cases / 934 assertions** and **11 GUI cases / 763 assertions**.
A synchronous-close regression then reproduced **1 case / 1 failed assertion**:
Qt removes a tab and notifies observers before the owned-pane vector erases its
old entry. Query lookup now resolves the actual view identity rather than the
transient tab/vector index, preserving surviving-pane attribution in callbacks.

Final normal and ASan+UBSan acceptance each passed **113 unit cases / 2,218
assertions** and **212 GUI cases / 8,906 assertions**. Qt 5 syntax checks passed
83 translation units, with the final two changed units rechecked; three control
metadata headers passed standalone Qt 5 and Qt 6 checks. Platform-fragility,
test-determinism and repository-hygiene lints passed. The normal GUI was rebuilt.
LeakSanitizer remained disabled, and no native Windows/Linux, linked Qt 5 GUI or
new full packaged-release acceptance is claimed.

This supersedes Slice 43's unavailable applied-observation capability:
`applied_filter_observation` is now true. The toolbar draft remains a separate
value, and folder filter writes, kept-pane creation and explicit filter
transactions remain unsupported.

## Slice 45: private kept-pane creation for file/live filters

`filter.apply` now accepts `keep_results:true` for file/live tabs, including when
inherited from the draft. Preparation leaves the old pane and draft visible.
Successful publication creates/selects a new results pane, returns its new
`pane_id`, retains the old model/query/marks, and consumes the keep toggle. The
new model starts with no marks, matching native Keep semantics. Empty text still
creates a pane and applies native `NoSearch` display policy, not a match-all
completed search. Folder writes and explicit filter transactions remain absent.

A GUI-owned `FilterUpdateStaging` separates unpublished model/view lifetime from
the value-only worker request. A fresh destination model is created before its
capture; the existing owner-bound model preparation builds the final query,
context and visibility projection. Acquisition creates/configures the private
view and preallocates model registration. Publication validates the old active
pane's identity, generation, draft, selection and relevant settings, installs the
destination silently, interrupts the old search and binds the new pane without
ordinary tab-switch facade notifications. Notifications follow guard release.
Unpublished cleanup stops/deletes the view before model, pattern and source.

The implementation reuses targeted view configuration instead of calling the
broad source/history/watcher configuration path during staging. UI-baseline
capture and target-model capture were separated after initial GREEN so a new
pane no longer copies an old mark projection only to throw it away. No duplicate
pane registry, relaxed owner-ID validation or worker-owned GUI object is added.

TDD and acceptance evidence so far:

- Native Keep RED: **1 case / 2 failed assertions**, file and live admission.
- GUI staging characterization: **27 unit cases / 821 assertions** and
  **11 GUI cases / 763 assertions**.
- Initial native Keep GREEN: **28 cases / 888 assertions**.
- Expanded native normal and ASan+UBSan acceptance: **34 cases / 1,030 assertions**,
  covering context/visibility, empty policy, queued old-search cancellation,
  source conflicts, failed preparation and unpublished-owner cleanup.
- Kept-pane RPC RED: **1 case / 2 failed assertions**; first GREEN including a
  visible MainWindow and destructive kept-notification observers: **35 unit cases
  / 1,045 assertions**, **12 GUI cases / 882 assertions**.
- A scoped read-only review found no concrete ownership/admission/publication
  defect. Arbitrary Qt focus/child callback destruction and fault-injected global
  allocation rollback were not verified and are not promised.
- Final post-refactor normal and ASan+UBSan acceptance each passed **121 unit
  cases / 2,510 assertions** and **213 GUI cases / 9,050 assertions**. This includes
  independent CLI Keep calls and detached worker preparation after all GUI owners
  disappear, for both file and live sources.
- Qt 5 syntax checks passed **83 translation units**; three control metadata
  headers passed standalone Qt 5 and Qt 6 checks. Platform-fragility,
  test-determinism and repository-hygiene lints passed. The normal GUI was rebuilt.
- ASan runs disabled LeakSanitizer. Native Windows/Linux, linked Qt 5 GUI runtime,
  Vectorscan-enabled runtime and new full packaged-release acceptance were not run.

This supersedes earlier slices' rejected Keep option and false
`filter_kept_pane_creation` capability. Unchanged-snapshot source admission and
five-minute/two-preparation operation limits remain unchanged.

## Slice 46: typed complete-folder result handoff

`FolderSearchEngine::searchFinished` remains the legacy terminal notification,
not proof of success. The engine now records current-generation terminal metadata
and offers `takeCompletedSearch(generation)`, a thread-safe, consume-once handoff
with explicit Ready, Cancelled, CompileFailure, EnumerationFailure,
RetentionFailure, ResultsConsumed and Failed outcomes.

The receipt retains the enumerated membership order, including zero-match
content leases. It does not introduce a second owner of file groups: successful
handoff moves the existing pending slots. Stale generations cannot take the
current receipt. If a streaming consumer has already taken a group, a bulk claim
is rejected without destroying that consumer's remaining groups. Legacy
`takeResults` and `takeCompletedGroup` remain available for their existing paths.
This distinction is required before a private folder candidate can safely treat
an empty completed scan differently from failed or cancelled enumeration.

Initial behavioral RED: **4 cases / 9 failed assertions**. First native GREEN:
**123 cases / 1,541 assertions**. The competing-consumer preservation regression
then produced **1 case / 1 failed assertion** and was fixed by leaving remaining
slots with the existing consumer. Normal and ASan+UBSan unit acceptance each
passed **135 cases / 1,715 assertions**. GUI acceptance passed **126 cases / 2,998
assertions** normally and **127 cases / 3,048 assertions** with sanitizers. Four
Qt 5 translation-unit probes and four standalone headers on both Qt versions
passed. An explicit `QStringList{}` default argument resolved the platform lint
without suppression; the normal GUI/tests were rebuilt, the 135-case unit suite
and two affected Qt 5 units were rechecked, and all three repository lints passed.
LeakSanitizer and native Windows/Linux runtime acceptance were not run.

This is an internal result-handoff primitive, not delivered folder filter writes.
Captured scanner execution policy, private whole-result layout preparation,
silent model installation and folder GUI publication are still required. It does
not add an atomic filesystem-wide snapshot guarantee or complete exception
containment for scanner thread-launch/allocation failures.

## Slice 47: caller-captured folder execution policy

Folder requests now capture `ScanSettings` before dispatch: the existing
`SearchSettings` value plus the default encoding MIB. Folder validation, regex
compilation, block-scan admission and both encoding-detection paths consume that
same value. Synchronous convenience entry points capture on their caller too;
the coordinator never recaptures preferences after enumeration. The native
hardware-based file-worker pool, capped at 16, remains unchanged.

`CompletedSearch::requestSettings` records requested policy, not the actual
fallback regex backend, detected per-file codec or worker-pool size. Explicit
encoding fallback `-1` still means automatic fallback; it does not authorize a
later read of mutable global configuration. Existing BOM/detection precedence is
unchanged.

The semaphore-controlled regression changes preferences while enumeration is
pending. Initial RED exposed two missing backend/block-scan values; the extended
encoding RED exposed one missing fallback value (`-1` instead of Latin-1 MIB 4).
After the fix, the normal folder suite passed **125 cases / 1,563 assertions** and
the application and both test binaries built. Normal GUI acceptance passed
**125 cases / 2,952 assertions**. ASan+UBSan acceptance passed **131 unit cases /
1,651 assertions** (also including indexing-preparation coverage) and **126 GUI
cases / 2,998 assertions**. Four Qt 5 translation-unit probes, four standalone
headers on both Qt versions, all three repository lints and whitespace checks
passed. LeakSanitizer, Vectorscan-enabled runtime, linked Qt 5 GUI runtime and
native Windows/Linux acceptance were not run.

This remains an internal preparation prerequisite. Folder filter writes,
whole-result silent installation and scanner thread-launch exception containment
are not delivered by this slice.

## Slice 48: private whole-folder result publication primitive

`FolderSearchResults` now has owner-bound capture, worker preparation and guarded
silent installation for a complete `FolderSearchEngine::CompletedSearch`. Only
`Ready` receipts with complete, ordered membership are accepted. The existing
`MaxPreparedLayoutRows` bound covers the candidate groups/records; the existing
marks capture also bounds shared-mark state. No GUI command or folder filter RPC
is enabled by this primitive.

Preparation reuses the native layout builder and immutable source leases,
including zero-match members. Installation preserves the model object, shared
marks binding/values, visibility and encoding choices. It replaces the source
membership/indexes and layout together, restricts marks presentation to the new
enumerated membership, and preserves native re-search collapse by file path.
Excluded marks remain in the shared store. Old streaming buffers/cursors and
file-content caches are cleared so neither delayed arrivals nor cached marked
text can contaminate the completed replacement. Old immutable read snapshots
remain usable independently.

All acquisition precedes mutation: the guard takes the model lock and then the
file-cache lock. Installation is consume-once and emits no signal; the owning
coordinator releases every guard before issuing the final layout notification.
Freshness is deliberately strict for a complete replacement: a different
published layout, including a new empty search, invalidates the capture. Changed
marks values or binding also reject acquisition. Engine/request freshness remains
the coordinating owner's responsibility. Dropping an uninstalled guard leaves
the model unchanged, and immutable preparation outlives its capture owner.

Behavioral RED: **1 case / 1 failed assertion**. Initial GREEN passed **126 cases /
1,591 assertions**; expanded ownership, empty/failure, collapse, stale-streaming,
cache and retained-mark coverage passed **129 cases / 1,697 assertions**. Expanded
normal and ASan+UBSan acceptance each passed **139 unit cases / 1,927 assertions**;
GUI acceptance passed **125 cases / 2,949 assertions** normally and **126 cases /
2,998 assertions** with sanitizers. Six Qt 5 translation-unit probes, five
standalone headers on both Qt versions and all three repository lints passed.
A follow-up regression exposed pending collapse being discarded by native empty
completion without a layout notification (**1 case / 1 failed assertion**).
Acquisition now compares that captured state directly too. After this refinement,
the application and normal/sanitizer test binaries rebuilt; normal and ASan+UBSan
unit acceptance each passed **140 cases / 1,935 assertions**. The two affected
Qt 5 units, all three lints and whitespace checks were rechecked successfully.
GUI suites above preceded this final acquisition-only refinement and were not
rerun afterward. LeakSanitizer and native Windows/Linux acceptance were not run.

The registry bookkeeping and cache clearing were extracted into shared helpers;
legacy `setResults`, streaming and marks-only preparation keep their existing
semantics. This does not promise fault-injected global allocation rollback,
filesystem-wide atomic capture, or completed folder GUI/RPC publication.

## Slice 49: prepared folder visibility changes

Native characterization confirms that folder search resets Marks-only visibility
before the Keep decision. A pane with zero currently visible rows can still be
kept when the reset reveals existing matches. Both the retained pane and the new
pane then use Marks-and-matches. Empty search also resets visibility but bypasses
Keep consumption and pane creation. This ordering is now protected by a semantic
completion test (**1 case / 20 assertions**) without presentation sleeps.

The existing marks-layout capture and complete-search capture now accept an
optional candidate visibility. Omission preserves the captured choice, so existing
annotation transactions keep their behavior. Preparation builds the desired
layout privately; guarded installation publishes its visibility and rows together,
without calling the notifying/rebuilding public `setVisibility` path. Acquisition
still validates the original visibility/context against concurrent edits. This
supports preparing the retained-pane projection separately from the destination
query while acquiring all guards before any visible state changes.

Model RED: **1 case / 8 failed assertions** across both projection and full-search
replacement. Normal GREEN passed **141 unit cases / 1,966 assertions** and **126
GUI cases / 2,970 assertions**; the application and both test binaries rebuilt.
ASan+UBSan passed **141 unit cases / 1,966 assertions** and **127 GUI cases /
3,021 assertions**. Four Qt 5 translation-unit probes, five standalone headers
on both Qt versions, all three repository lints and whitespace checks passed.
LeakSanitizer, linked Qt 5 GUI runtime, Vectorscan-enabled runtime and native
Windows/Linux acceptance were not run.
This extends internal publication primitives only; folder GUI staging and filter
RPC are still unfinished.

## Slice 50: implicit folder filter application

`filter.apply` accepts folder tabs through the same strict patch decoder,
revision check, two preparation permits and cancellable operation registry as
file/live tabs. The worker re-enumerates nonempty queries. It owns enumeration
directly (no detached private enumerator) and uses the same per-member scanner,
serial matcher creation, bounded file pool and retained bytes as native folder
search. Once cancellation is observed it stays latched; unexpected scanner and
partial thread-launch failures join every started worker and cannot publish a
partial success. A vanished root is a preparation failure, not an empty folder.
Individual inaccessible/binary files still follow native folder scan semantics.

The GUI validates the captured tab, pane, draft and relevant human changes
before installing. Keep may require two models: reveal matches on the retained
pane and install the destination layout. An empty query instead uses the prior
membership for marks, remembers collapse by path and does not enumerate or
consume Keep. No selection of a source file is synthesized. A focus transfer
from the outgoing results or Stop control is rejected rather than forced. Native
pane/query metadata is current immediately; inactive presentation remains
deferred. The RPC result has the same IDs and no log content, regardless of tab
kind. Folder scans have no filesystem-wide point-in-time guarantee.

The integration regression drives a real `kloggctl` process against the GUI:
apply to a background folder after adding a file, observe matched membership,
keep a pane, reset in place, then inspect the retained pane's applied filter.
Additional tests cover cancellation, human edits, closure, focus, budget sharing,
partial launch, lost folder roots, retained marks and context. This is an
implicit operation, not an explicit transaction composed with other RPCs.

Local macOS Qt 6 acceptance: **162 unit / 137 GUI cases** passed normally
and **162 unit / 138 GUI cases** with ASan+UBSan, including the subprocess CLI
and coexisting file/live filters. Twelve affected translation units passed Qt 5
syntax checks; five standalone headers passed on Qt 5 and Qt 6. Platform,
determinism, repository hygiene and whitespace checks passed. Native Windows and
Linux, linked Qt 5 GUI, Vectorscan-enabled runtime, LeakSanitizer and release
package verification were not run for this slice.

## Developer CLI walkthrough (implemented surface only)

Start the GUI normally and open the documents to review. The CLI never launches
or activates a GUI. Use the executable beside the developer GUI binary (inside
`klogg.app/Contents/MacOS` on macOS), or its full path if shell integration has not
been installed.

```sh
kloggctl instance list
kloggctl --instance INSTANCE call capabilities.get --params '{}'
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
kloggctl --instance INSTANCE call pane.list --params '{"tab_id":"TAB"}'
kloggctl --instance INSTANCE call pane.get --params '{"tab_id":"TAB","pane_id":"PANE"}'
kloggctl --instance INSTANCE call source.capture --params \
  '{"tab_id":"TAB","pane_id":"PANE","scope":"presented"}'
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

Copy `marks_owner_id` from the relevant pane to inspect its stored marks:

```sh
kloggctl --instance INSTANCE call mark.list --params \
  '{"tab_id":"TAB","owner_id":"MARK_OWNER"}'
```

Add `transaction_id` to make that observation a repeatable read dependency of a
mixed transaction. File/live kept result pages can have different mark owners;
folder pages share one. Returned stored row indices and optional member paths
describe GUI state, not validated evidence addresses. No log text is returned or
decoded.

To change marks, use a retained `source.capture` snapshot from the relevant pane.
The row strings below are **snapshot ordinals**, not the stored indices returned
by `mark.list`:

```sh
kloggctl --instance INSTANCE call mark.add --params \
  '{"tab_id":"TAB","owner_id":"MARK_OWNER","snapshot_id":"SNAPSHOT","rows":["0","2"],"expected_revision":"REVISION"}'
kloggctl --instance INSTANCE transaction get --transaction TRANSACTION
kloggctl --instance INSTANCE call mark.remove --params \
  '{"tab_id":"TAB","owner_id":"MARK_OWNER","snapshot_id":"SNAPSHOT","rows":["2"]}'
kloggctl --instance INSTANCE call mark.clear --params \
  '{"tab_id":"TAB","owner_id":"MARK_OWNER"}'
```

Use `mark.replace` with the same source-backed arguments to replace the selected
owner's complete mark set. Add `transaction_id` to stage any of these operations
with labels/highlighters. For a replacement larger than 256 evidence rows, stage
Clear followed by bounded Add batches in one transaction. Inspect the returned
transaction until terminal; a `preparing` response is acceptance, not successful
publication. A released/expired snapshot cannot supply new edits, but releasing
it after successful staging does not erase the transaction's value proofs.

Observe or replace global highlighter sets independently, or add
`transaction_id` to combine replacement with staged labels:

```sh
kloggctl --instance INSTANCE call highlighter.list --params '{}'
kloggctl --instance INSTANCE call highlighter.replace --params-file replacement.json
kloggctl --instance INSTANCE transaction get --transaction TRANSACTION
```

Example `replacement.json` (this replaces all sets, not just one):

```json
{
  "sets": [{
    "id": "review-errors",
    "name": "Review errors",
    "rules": [{
      "pattern": "ERROR|FATAL",
      "ignore_case": false,
      "use_regexp": true,
      "match_only": true,
      "foreground": "#ff000000",
      "background": "#ffffff00",
      "variate_colors": false,
      "color_variance": 15
    }]
  }],
  "active_set_ids": ["review-errors"]
}
```

Copy the returned transaction ID and query until no longer `preparing`; inspect
`status` and `error.code`, not only CLI exit code. A successful request can report
accepted work whose later compilation or optimistic validation fails. Polling is
for observation, not to drive the commit. Use `expected_revision` from
`highlighter.list` when replacement must not overwrite a newer human edit.
Empty `sets` and `active_set_ids` arrays remove all sets; the label palette is
preserved. Active-set IDs must refer to unique IDs in the replacement.

### Apply a filter to the current results pane

Read `tab.get` first if you need the draft revision, then use the generic call
surface (there is no dedicated filter subcommand):

```sh
kloggctl --instance INSTANCE tab get --tab TAB
kloggctl --instance INSTANCE call filter.apply --params \
  '{"tab_id":"TAB","expected_revision":"REVISION","filter":{"text":"ERROR|FATAL","match_case":false,"use_regexp":true,"inverse":false,"boolean":false,"auto_refresh":false,"keep_results":false}}'
kloggctl --instance INSTANCE call operation.get --params \
  '{"operation_id":"OPERATION"}'
kloggctl --instance INSTANCE call operation.cancel --params \
  '{"operation_id":"OPERATION"}'
```

Replace `REVISION` with the canonical unsigned decimal string returned in
`filter_draft.revision`, or omit `expected_revision`. Replace `OPERATION` with the
returned ID. Poll until terminal and inspect `status` and `error.code`; the
cancel command is an alternative when you want to abandon pending work, not a
required completion step. A pending response is not successful publication.

`filter` is a strict patch, not a complete replacement object: omitted fields
retain current draft values. Explicit `keep_results:false` selects replacement;
`keep_results:true` requests a kept pane. File/live tabs retain the original
model and marks; folder tabs share marks across panes. A nonempty folder search
re-enumerates the directory and consumes Keep, creating a pane only if the
old pane has visible rows after resetting Marks-only visibility. Empty folder
filters reset in place and preserve Keep. The response identifies the destination
pane. Supply a file/live/folder tab ID, not `transaction_id` or a pane selector. The operation captures the
addressed tab's current results pane and rejects conflicting changes rather than
following a later pane switch. Continuous append may prevent unchanged-snapshot
admission; re-observe before deciding whether to retry.

To reset, submit `{"tab_id":"TAB","filter":{"text":"","keep_results":false}}`
to `filter.apply`. This restores native `NoSearch` and the configured empty-filter
presentation; it is not an empty regex search with completed match-all results.
No apply response contains log content. After success, use the returned `pane_id`
with explicit `source.capture` and `source.read` as above. For metadata-only
query attribution, pass that `pane_id` to `pane.get` and inspect `applied_filter`.
`pane.list` includes the same value for each retained result pane. No separate
`filter.get` method is exposed: `tab.get.filter_draft` is still not a receipt for
the query that produced the current results.

### Open and close documents without activation

Obtain a `window_id` from `state get`, then open a regular file or folder:

```sh
kloggctl --instance "$INSTANCE" call documents.open --params \
  '{"window_id":"WINDOW_ID","path":"/absolute/path/to/log.log","kind":"file"}'
kloggctl --instance "$INSTANCE" call operation.get --params \
  '{"operation_id":"OPERATION_ID"}'
kloggctl --instance "$INSTANCE" call operation.cancel --params \
  '{"operation_id":"OPERATION_ID"}'
```

Use `"kind":"folder"` for a directory. File requests may include `"follow":true`
to enable native follow-on-load when file watching is enabled. A new open returns
`pending`; completion happens without polling. Inspect the eventual operation
`status` (`completed`, `failed`, or `cancelled`) and `error.code`. A completed open's
`result` contains `window_id`, `tab_id`, and `opened:true`, never log content. An
already-open path returns a completed operation without selecting or reopening it.

```sh
kloggctl --instance "$INSTANCE" call documents.close --params \
  '{"window_id":"WINDOW_ID","tab_id":"TAB_ID"}'
```

Close returns a completed operation with `opened:false`; it closes the tab, never
the file on disk. Closing the current tab allows native replacement selection;
a surviving current tab is preserved. Live capture tabs are rejected. These
commands are implicit transactions and do not accept `transaction_id` yet.

Source-addressed navigation/selection is not yet exposed. `call` does not bypass
the server's supported-method list.

## Remaining scope

These slices do not constitute the complete product. Source-addressed navigation
and selection, mixed explicit lifecycle/filter
transactions, full parameter/result schema discovery, and full release-artifact
acceptance remain. `capabilities.get` provides method/feature/limit inventory.
Implicit current-pane filter publication is delivered for file/live tabs, with
unchanged-snapshot admission rather than continuous-append cutover guarantees.
Relocated macOS Qt/CLI startup is verified in Slice 23; device sidecars,
mounted/signed release artifacts and native Windows/Linux packaging are separate
remaining gates.
The developer CLI supports observation, bounded content reads, implicit file/folder
open/close, file/live/folder filter application/reset and private kept-pane creation, and mixed
source-backed mark/label/global-highlighter transactions. `tab.get.filter_draft`
is still draft-only. Explicit transactions do not yet compose lifecycle or
filter-result changes with annotations. No operation promises global durability
or event subscription.
