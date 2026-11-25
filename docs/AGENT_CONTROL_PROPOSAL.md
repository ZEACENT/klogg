# Local CLI control and transactional log review

**Status: accepted product/architecture design; implementation in progress.**
Developer builds provide the initial local endpoint, `kloggctl`, GUI observations
and label transactions. This document describes the full target, not a claim that
all illustrated commands are available. Verified slices, usable scope and
outstanding command/installer/package gates are tracked in the
[implementation record](AGENT_CONTROL_IMPLEMENTATION.md).

## 1. Product decisions

The product owner clarified the following requirements after the initial study:

| Decision | Accepted direction |
| --- | --- |
| D1. Scope | Prioritize bidirectional read/write access to file/folder open and close, tab filters, marks, highlighters, and color labels. Observe and annotate already-open live tabs; defer device/capture lifecycle control. |
| D2. Enablement | Local control is enabled by default, with one global user setting to disable it. No per-client pairing, caller-specific permission policy, or authorization popup. |
| D3. Content reads | Provide explicit bounded log reads. Ordinary state queries report metadata and locations without automatically including log text. |
| D4. Ownership | Human and programmatic operations are equivalent and can overwrite one another. No AI-specific layer, annotation ownership, or protected human namespace. |
| D5. Transactions | Prepare changes without visible intermediate state or interruption. Account for concurrent human operations using transactional rules; only commit publishes the combined presentation change. Do not activate or raise the application. |
| D6. Shell discovery | Bundle the CLI and put a shell-integration installation button beside the global control setting. Installation must be explicit, inspectable, and reversible. |

The external harness owns analysis, model access, and any uploading of returned
content. klogg remains provider-neutral and does not require a model API key,
Python, Node.js, MCP, a cloud service, or an always-running daemon.

The target is a shared review workspace, not an AI overlay: a person can edit
what an agent set, and the agent can read and change what the person set. Equality
of operations does not mean a stale transaction can overwrite intervening work.
A fresh transaction can deliberately replace the currently committed state.

## 2. Existing command-line surfaces and their responsibilities

These are code observations, not a claim that a general GUI-control API exists.

| Surface | Current behavior | Correct architectural role |
| --- | --- | --- |
| `klogg` startup CLI | `CliParameters` parses files, session/new-session, multiple-instance, follow, window geometry, help/version/debug. `main.cpp` launches the GUI or forwards files to the primary instance. | Application bootstrap and desktop file association. Keep compatible; not a tab-state query/mutation protocol. |
| Single-instance forwarding | `KloggApp::sendFilesToPrimaryInstance` sends CBOR version/files; `MessageReceiver::receiveMessage` emits `loadFile`. | Legacy launch forwarding. Transport delivery is not observable file readiness or a transaction. |
| `klogg_grep` | A `QCoreApplication` constructs `LogData` / `LogFilteredData`, searches the first supplied file using the pattern, prints matching lines, and exits. | Independent headless search frontend over shared data/search implementation. It neither attaches to nor controls the GUI. |
| Test executables / CTest | Test-runner arguments select fixtures and cases; tests exercise models/widgets and synchronization seams. | Developer verification. Test invocation is not a supported external application API. |
| Proposed `kloggctl` | A console client discovers a GUI instance and submits typed requests. | Control and observation of the existing workspace; not a second search engine or headless viewer. |

Key sources:

- `src/app/cli.h`: GUI/console argument parsing currently shares one parser with
  a `console` switch. `-e` / `--pattern` belongs to console mode, not the GUI CLI.
- `src/app/main.cpp`: secondary-instance forwarding and initial file/session load.
- `src/app/kloggapp.h` and `src/app/messagereceiver.h`: one-way CBOR file forwarding.
- `src/app/klogg_grep.cpp`: headless load/search/print path.
- `src/app/CMakeLists.txt`: three executable targets; `ci_build` includes
  `klogg_grep`, while the main `install(TARGETS ...)` rule installs `klogg`.
  A build target is not proof of inclusion in every release package.
- `tests/unit/cli_test.cpp`: parser tests, not a running-GUI control API.
- `packaging/linux/klogg.desktop`: desktop entries launch `klogg %F`,
  `klogg --load-session %F`, and `klogg --multi %F`.
- `tests/ui/CMakeLists.txt` and `tests/ui/qtests_main.cpp`: `klogg_itests` links
  the actual GUI library, constructs QApplication, and passes test selectors to
  Catch. Tests construct/manipulate widgets within their own process.
- Root `CMakeLists.txt`, `klogg_add_runtime_test`: registers offscreen Qt tests
  with isolated configuration/coordination roots; `klogg_smoke` runs `klogg -v`.
- `scripts/verify_ubuntu_deb.sh` and
  `packaging/linux/appimage/generate_appimage.sh`: packaged version/startup
  smoke, not tab/filter/mark control.

For example, the UNIX `klogg_itests_filewatcher_low_fd` test invokes
`klogg_itests` with the Catch tag `[.filewatcher-lifecycle]`. `-platform offscreen`
selects Qt's offscreen platform; it does not add a public GUI command language.
Existing test-only accessors and diagnostic environment flags are not protocol
contracts and must not become the production control API.

### Assessment

Separate process entrypoints are reasonable because startup, independent batch
search, and manipulation of a live workspace have different lifetimes and owners.
Unifying their business logic is more important than making them one executable.
`kloggctl` should link Qt Core/Network and a small protocol library, not Widgets or
the log search engine. `klogg_grep` can continue to reuse the data/search pipeline.

Do not extend the `console` boolean into a three-mode mega-parser, or route control
commands through QApplication initialization. Preserve the old launch interface.
An optional launcher alias may delegate to the same control client where useful
for package ergonomics; it must not introduce another implementation.

The existing grep entrypoint is small, not already a complete automation product:
it consumes `filenames.front()`, emits plain text, and lacks the workspace,
structured errors, bounded reads, and transaction contracts described here.
That is a reason not to repurpose it, not a reason to fix unrelated grep behavior
inside this feature.

## 3. Runtime pipeline and ownership

Follow the existing [pipeline ownership guidance](MATCH_SELECTION_PIPELINE_PLAN.md#3-pipeline-and-ownership):
stages are responsibility boundaries, not a mandate for one class, thread, queue,
or source-file pair per stage. Keep common behavior on one path and isolate
single-file, folder, and live differences at their source boundaries.

```text
GUI intent adapter              CLI parser -> local IPC decoder
        \                         /
         typed application commands / queries
                         |
         resolve targets + validate contract
                         |
         snapshot + transaction read/write sets
                         |
         prepare unpublished changes / async work
                         |
         validate revisions + commit on GUI thread
                         |
         publish one coherent state change
                         |
         existing projections / rendering / persistence
```

| Stage | Owner and output | Must not do |
| --- | --- | --- |
| Decode | Frontend adapters produce typed commands and explicit target references. | Interpret source data, call arbitrary slots, or mutate widgets. |
| Resolve/validate | Application service resolves instance/window/tab/pane/source scope and validates capabilities, parameters, and enablement. | Use current focus to guess a mutation target. |
| Snapshot | Transaction owner captures immutable review state and dependency revisions; source adapters supply existing `LogSnapshot` / `LogRowId` contracts. | Treat the settings file as live state or clone whole multi-gigabyte logs. |
| Prepare | Existing search/source workers and model owners create unpublished candidate results and changes. | Run GUI setters, expose provisional tabs, or emit intermediate presentation. |
| Commit | One coordinator validates the complete dependency set, then installs prevalidated changes at a GUI-thread linearization point. | Wait for I/O/search, show a modal, pump events, or publish half the changes. |
| Publish | Existing model/view owners project the committed state and notify observers using one committed change set. | Rerun matching independently or invalidate unrelated live refresh work. |
| Persist/respond | Existing persistence owners save normal application state; protocol encodes outcome and durability status. | Claim crash-durable transactions that the persistence layer does not provide. |

GUI and CLI operations use the same semantic mutations, invariants, and revision
tracking. Ordinary GUI actions and standalone CLI writes are short autocommit
operations; an explicit CLI transaction stages several of those operations.
Do not force unrelated GUI actions through RPC or a general-purpose event bus.

### Reuse and necessary refactors

- `Session::openedDocuments` provides document metadata, but path/document IDs
  are not unique runtime tab addresses. Add stable tab/pane identities.
- File/live and folder tabs can contain multiple kept-result panes. A tab does
  not have just one filter, result generation, selection, or mark scope.
- `LogSnapshot` and `LogRowId` already provide immutable content, source labels,
  identity remapping, and presented-row mapping. Adapt them, do not duplicate them.
- File marks currently belong to filtered models; folder marks are per-file/tab
  state shared across result panes. Expose explicit scope and preserve current
  semantics unless a characterized shared-model migration is necessary.
- `ColorLabelsController` already owns one tab's labels and synchronizes all its
  views. Extract typed read/write operations from selection-driven private methods
  there, instead of adding separate automation label storage.
- `HighlighterSetCollection` owns persistent global sets and activation state.
  Expose that scope honestly. A global-set change includes all affected dependency
  revisions; do not pretend it is isolated to the requested tab. Reading a tab
  reports its effective highlights and their actual scope.
- Search toolbar text being edited, submitted query, and each pane's applied query
  are distinct states. Use a typed `FilterSpec`, not a sequence of UI toggles.
- Folder open/reveal needs a nonmodal, unpublished preparation path. A pending
  reveal can still display the old file; requested and displayed locations differ.
- Live observation must fan out at its owner, not replace the GUI's callback.
- Session persistence is not an observation API. `MainWindow::writeSettings`
  currently puts `0UL` into the top-line tuple; query actual view state instead.

Relevant owners are `session.h/.cpp`, `crawlerwidget.h/.cpp`,
`foldercrawlerwidget.h/.cpp`, `abstractlogview.h/.cpp`, `searchtoolbar.h`,
`logfiltereddata.h`, `logsnapshot.h`, `colorlabelscontroller.h/.cpp`,
`highlighterset.h`, and `livelogcontroller.h`.

## 4. Transaction contract

### Scope and lifecycle

A transaction is limited to one GUI process, may target multiple file/folder tabs,
and can include shared highlighter definitions. There is no distributed commit
across GUI processes and no transactional control of the filesystem or devices.

```text
begin -> staged commands/reads -> preparing -> ready -> commit -> committed
                       \             \          \-> conflict / failed
                        \-------------> abort / expiry
```

Offer both an explicit lifecycle (`transaction begin/commit/abort/get`) and a
single-document `transaction apply --file changes.json --wait` convenience command.
Both use the same engine. A lone mutation is a one-command transaction.
Nested transactions, savepoints, SQL, and arbitrary scripts are not initial goals.

Within a transaction, reads see the captured base state plus its own staged writes.
Outside reads and GUI interactions see only committed state. Track reads and
writes server-side; a client must not have to guess the dependencies to validate.
When a read enumerates tabs or rules, track collection/predicate revisions too,
so insertion/removal cannot silently invalidate an assumption (a phantom).
New targets and sources discovered during preparation add validated dependencies;
expired/unavailable snapshots fail explicitly rather than reading a new revision
under the old transaction identity.

### Optimistic isolation: humans remain interactive

Do not hold a workspace lock while the agent thinks or workers search. The person
can continue scrolling, editing, opening tabs, and adding labels normally.

At commit, validate relevant read and write dependencies against their captured
revisions. Any incompatible change aborts the entire transaction with
`REVISION_CONFLICT`; the person's committed state remains. The agent can read
again and submit a fresh replacement. Do not automatically rebase or force-retry
a stale replacement, since that changes the user's observed-state assumptions.

Track semantic scopes, not one noisy global revision. For example:

- A human filter edit conflicts with a transaction that reads/replaces that filter.
- A mark change conflicts if the transaction read/replaces that mark collection.
- Scrolling another tab need not conflict with a label edit in the target tab.
- Closing a tab conflicts with changes to that target.
- A global highlighter edit conflicts with transactions relying on those rules.
- A tab close must also validate presentation/selection dependencies where closing
  the active tab changes the selected successor.
- Source replacement/encoding changes invalidate line evidence. Immutable pinned
  live content can survive unrelated append, but eviction/reset must not silently
  rebind a source row.

The goal is serializable application-state semantics for the declared/tracked
reads and writes, not merely checking each write target independently. GUI edits,
background publication, history/undo if present, and restored state must all pass
through revision-owning paths. Source files and ongoing device streams remain
external state, verified through their snapshot identities rather than locked.

An in-progress editor deserves special treatment: text typed but not submitted,
IME composition, and unsaved highlighter-dialog drafts must not be overwritten
by background publication. Represent the relevant edit session as a dependency
or temporary busy condition; reject/defer the CLI commit without blocking input.
A completed replacement in a fresh transaction is still allowed by D4.

### Atomic publication, not paint suppression

Prepare allocations, regex validation, source loading, result sets, and all
fallible model work before commit. At the linearization point, revalidate the
global enabled flag and dependency revisions, then install a bounded, prevalidated
change set without yielding to the event loop. Internal signals must not expose
partial state to synchronous observers; publish the combined change only after
all owners are consistent. Suppressing widget repaint alone is insufficient.

Prefer ownership/state swaps where the affected models support them. Where they
do not, extract a prepared-state/adoption seam under characterization tests.
Do not implement atomicity by changing visible widgets and trying to undo them
if a later command fails. Stale worker completions carry generations and cannot
publish over the committed state.

One commit means one logical presentation update and one coherent observer event,
not a promise of exactly one OS paint event or a freeze of live rendering. The
existing 30 FPS live refresh policy continues during preparation. Measure commit
cost locally and cap transaction size so installation does not monopolize the
GUI thread. Search can take time; the commit itself must remain short.

### Open, close, focus, and results

- Prepare new file/folder sources and searches without inserting provisional tabs
  into the visible registry. Abort/conflict releases unpublished resources.
  History and session-open lists update at commit, not at preparation.
- Stage a file/folder tab close without deleting its widget or detaching its model
  until commit. Retire resources safely after publication; cancellation completion
  and cleanup must not block the commit. Cancel only work owned by this transaction
  when aborting, not a person's concurrent search.
- Tab close does not delete the source file. Closing active live capture remains a
  deferred lifecycle capability. The initial file/folder close path is nonmodal.
- Never call raise/activateWindow or change OS focus. Opening is background by
  default. Changing the currently reviewed tab is an explicit in-application
  selection command, not an implicit side effect of every request.
- Navigation/filter/label changes publish together at commit. Closing the selected
  tab necessarily selects a successor; make that deterministic and part of the
  validated change set. Leave an empty workspace if closing the last tab rather
  than implicitly quitting the GUI or transferring focus to another app.
- If an operation genuinely requires interaction, return `INTERACTION_REQUIRED`;
  do not create a hidden dialog or add a caller-authorization popup.

For transactional filter changes, prepare the finite result snapshot before
publication so the new query is not shown beside old results. Live searches use
a finite source snapshot and resume normal incremental updates after commit.
Transaction reads can report a prepared query as pending until its result exists;
they must not label old result rows as matches of the new query.

### ACID boundaries and failure reporting

| Property | Initial contract |
| --- | --- |
| Atomicity | All supported review-state changes commit together or none become visible. |
| Consistency | Shared application invariants, source identity, scope, and lifecycle rules are validated. |
| Isolation | Optimistic dependency validation; readers see committed state or their own transaction view, never another transaction's partial state. |
| Durability | Uses existing session/settings persistence semantics. An in-memory commit is not a crash-durable database transaction; response distinguishes committed revision from persistence status. |

Full crash-durable, multi-object ACID would require a journal/recovery protocol
covering the currently separate persistence owners. Do not claim it by calling
QSettings sync after publishing, and do not add a database merely to implement
non-interrupting UI transactions. Durability beyond normal GUI actions is a
separate potential requirement, not assumed by this proposal.

Bound transaction lifetime, pinned snapshots, provisional sources, command count,
result bytes, and outstanding work. Abort/expiry releases candidates and publishes
nothing. Lost responses do not mean rollback: return/query transaction and
operation IDs, support idempotency keys with documented lifetime, and report an
expired/unknown replay status rather than promising exactly-once across restart.

Disabling automation prevents new requests/commits, terminates subscriptions, and
aborts uncommitted transactions. Already committed changes remain. The switch
and commit share the GUI-thread ordering point; disabling cannot half-apply a
transaction. Re-enabling republishes discovery without restarting the GUI.

## 5. Resources, addresses, and operations

Use opaque `instance_id`, `window_id`, `tab_id`, `pane_id`, `source_id`, and
`transaction_id` values. Paths, tab ordinals, display titles, and PIDs are not
stable tab identities. A closed/reopened tab gets a new runtime identity.
Use source generations and existing row identities for stale-reference detection.
There is no `agent_id` authorization scope or `layer_id` annotation owner.

Public source line numbers are one-based decimal strings. Large counts and
revisions also use strings to preserve 64-bit precision across Qt 5, JavaScript,
and JSON consumers. Bounded protocol versions/page limits may use JSON numbers.
Ranges have documented inclusive endpoints. Filtered rows, folder headings,
collapsed groups, and wrapped visual fragments are not source line addresses.

| Concern | Single file | Folder | Live |
| --- | --- | --- | --- |
| Location | Indexed source row | Member identity/path plus source row | Capture/segment identity plus retained row |
| Search boundary | Indexed snapshot and optional range | Enumerated member/result snapshot | Finite snapshot while append continues |
| Invalidation | Replacement, truncation, decoding change | Member changes plus enumeration/result generation | Eviction, reset, replacement; append handled by identity |
| Navigation | Reveal indexed source | Asynchronous member preparation before reveal | Reveal retained identity or report expired |

A path/size/mtime tuple is a change hint, not proof of content. Support optional
expected text/context/digest validation against the same source snapshot used by
the change. Report mismatch rather than silently relocating marks by fuzzy search.
Record encoding/line-splitting semantics. Defer substring coordinate mutation until
UTF-16/code-point/byte offsets have their own tested contract.

### Command groups

| Group | Proposed operations | Priority |
| --- | --- | --- |
| Discovery | `version`, `doctor`, `capabilities`, `schema`, `instance list` | Essential |
| Workspace | `window list`, `tab list/get/open/close/select` | Essential; file/folder close included |
| Observation | `state get`, `source describe/read`, `view get` | Essential; bounded explicit content reads |
| Navigation | `view goto/set`, follow state | Essential; never OS activation |
| Filters | `filter get/set/clear`, `result list` | Essential; explicit pane, mode, options |
| Marks | `mark list/add/remove/replace/clear` | Essential; same marks as GUI, explicit source/model scope |
| Highlighters | `highlight list/get/upsert/remove`, set activation | Essential; expose global-definition and effective-tab scopes |
| Color labels | `label list/set/remove/clear`, options | Essential; same per-tab controller, not mark colors |
| Transactions | `transaction begin/get/commit/abort/apply` | Essential; standalone writes autocommit |
| Async work | `operation get/wait/cancel` | Essential |
| Change events | `events watch` with bounded queues and resnapshot after gaps | High priority; polling snapshots suffice initially |
| Recipe replay | Export/import versioned command documents | Useful convenience over transactions, not a separate evidence subsystem |
| Capture lifecycle | Device connect/start/stop/reconnect/save | Deferred |

Color labels currently derive quick-highlighter entries from selected text, one
entry per selected line, through `ColorLabelsController`. Expose explicit text and
options or a snapshot-backed source selection that resolves to those entries.
Do not silently turn labels into a new per-row annotation type. Reuse the same
highlight composition and case/word options as the GUI.

Observation reports tab/pane identities, active tab, focused pane, visible source
ranges, selection shape, desired/submitted/applied filters, result readiness,
marks, effective highlights, labels, follow, auto-refresh, and capture status.
Do not infer human intent. Use revision-bound pagination for large collections;
expired cursors fail rather than combine incompatible snapshots.

Generate CLI help, schema, validation metadata, applicability, side-effect scope,
and common errors from one registry where practical. Do not write a second schema
by hand for an eventual MCP adapter. `--help`, `version`, and static `schema` work
without GUI/display initialization. Capability discovery includes source-specific
and transaction-specific limits.

## 6. Structured input, IPC, and replay

Use JSON-RPC 2.0 with named parameters and string request IDs over bounded,
length-prefixed UTF-8 messages. Negotiate an application protocol version
independently of the release version. Validate malformed/oversized messages,
partial/coalesced reads, excessive nesting, client backpressure, and version skew.
JSON-RPC batches are not ordered transactions; only the transaction API has the
commit semantics above.

CLI stdout is JSON; stderr is diagnostics. Offer explicit text output for people
and NDJSON for events. Complex input comes from `--file` / `--json @file` or stdin,
not shell-escaped giant JSON strings. Resolve CLI-relative paths before transport;
the server still validates them. Never execute a command, URL, or script found in
log text or an imported command document.

Illustrative command document, assuming `tab-a` and `src-a` were discovered:

```json
{
  "schema_version": 1,
  "idempotency_key": "review-update-17",
  "preconditions": {
    "tab_id": "tab-a",
    "state_revision": "42",
    "source_id": "src-a",
    "source_generation": "4"
  },
  "commands": [
    {"method": "filter.set", "params": {"tab_id": "tab-a", "pane_id": "results-a", "mode": "literal", "text": "timeout"}},
    {"method": "mark.add", "params": {"tab_id": "tab-a", "pane_id": "results-a", "source_id": "src-a", "line": "128"}},
    {"method": "label.set", "params": {"tab_id": "tab-a", "text": "connection reset", "label": 2}}
  ]
}
```

```sh
kloggctl instance list
kloggctl --instance INSTANCE tab list
kloggctl schema transaction.apply
kloggctl --instance INSTANCE transaction apply --file changes.json --wait
kloggctl --instance INSTANCE transaction get --transaction TX
```

The document provides an initial observed-state precondition; the server also
tracks all dependencies discovered while staging. Newly opened tabs receive
provisional references usable by later commands in the same transaction; those
references become visible runtime handles only on commit.

Accepted async work returns `operation_id`; completed commit returns its
`transaction_id`, `state_revision`, affected handles, and persistence status.
Domain errors carry a stable `error.data.code`, such as `CONTROL_DISABLED`,
`AMBIGUOUS_INSTANCE`, `REVISION_CONFLICT`, `EDIT_IN_PROGRESS`, `TARGET_CLOSED`,
`SOURCE_CHANGED`, `EVIDENCE_MISMATCH`, `EVIDENCE_EXPIRED`, `RESOURCE_LIMIT`,
`TRANSACTION_EXPIRED`, or `UNSUPPORTED_CAPABILITY`. Human error text is not an API.

### Local transport and default-on trust boundary

Use `QLocalServer` / `QLocalSocket`: filesystem Unix-domain sockets on macOS/Linux,
named pipes on Windows. No TCP port, mDNS, HTTP/CORS surface, or remote listener.
Discover each enabled instance through an atomically published descriptor in a
private, non-synced per-user runtime directory. Include instance/profile/install
identity and protocol range; isolate multiple/portable installations.

Default-on does not mean world-readable endpoints. Apply private directory/file
permissions or Windows ACLs and verify the local peer/instance handshake. A
per-instance secret may protect discovery/handshake but is not caller approval:
any program running as the same OS user is allowed. No client allowlist, pairing,
per-caller policy, or popup. Qt documents that macOS does not honor Unix socket
permission flags; rely on a protected parent and tested platform checks, not
`UserAccessOption` alone. Keep credentials out of arguments, outputs, and logs.

Handle stale endpoints and PID reuse without deleting live sockets. Enforce size,
work, and connection limits. Exclude arbitrary shell execution, source deletion,
credential access, remote URL fetching, and unrelated preference resets. File/
folder tab close and highlighter edits are allowed product operations, not
additional permission tiers. Observe existing live tabs without exposing device
lifecycle side effects. A disabled server rejects established connections too.

The global setting must be available locally in the GUI even when control is
disabled; do not expose a remote command that bypasses it. Ordinary configuration
ownership defines its scope (including portable profiles); it is global across
callers, not a machine administrator policy.

## 7. Settings and shell integration

Add an "Automation and command line" section to the existing settings panel:

```text
[x] Enable local command-line control
    Local programs running as your OS user can inspect and change this workspace.

Command: /installed/path/to/kloggctl
Shell integration: Not installed / Installed / Needs repair
[Install command-line tool...] [Repair / Remove] [Copy command] [Check]
```

The enable switch controls the running endpoint, not whether the binary exists.
Installation is independent: help/schema/doctor still work while control is off.
Persist the setting through the existing Configuration/OptionsDialog path, with a
true default for missing settings and correct preservation of an explicit false.
Applying it changes service availability without restart or authorization dialogs.

The install button is explicit authorization to modify the selected shell
integration, not permission to rewrite arbitrary startup files. Prefer a real
executable launcher in a user-owned bin directory on PATH, rather than an alias
or function: noninteractive harness shells may not load interactive rc files.

Platform behavior:

- Linux DEB: ship `kloggctl` in the normal package bin directory; show "available"
  when already discoverable rather than add redundant PATH entries.
- macOS: bundle the console binary under `Contents/MacOS`, sharing Qt frameworks;
  create a stable user-bin launcher pointing to the installed app.
- AppImage: bundle a CLI dispatch path that does not initialize the GUI; a user
  launcher targets the persistent AppImage, never its transient mount directory.
- Windows: bundle console-subsystem `kloggctl.exe`; install a stable user command
  path / user PATH entry using the platform service, not a Bash rc or shell alias.
- Portable packages: direct invocation always works; shell integration is optional
  and reports stale targets after the package is moved.

If the user-bin directory is absent from PATH, show which configuration will
change. Use a dedicated, delimited block appropriate to the selected shell or a
Windows user-environment change. Respect zsh/bash/fish conventions and custom
configuration paths; do not blindly assume `.bashrc` or `.zshrc`. Use backup and
atomic write, refuse unmanaged command-name collisions, and make repeated install,
repair, and removal idempotent. Remove only entries still owned by this installer,
preserving unrelated edits and shared PATH directories. Do not execute/source
startup files to discover configuration.

Keep installation in a platform-specific shell-integration service; the settings
widget only requests an operation and renders its result. Existing Qt-version and
OS differences belong in platform abstractions, not duplicated business/tests.

Explain that existing terminals/harness processes may need restart to inherit
PATH. Do not put PATH exports in every noninteractive shell's startup path just
to make it appear automatic. Offer absolute-path copy as the reliable fallback.
Container/WSL/SSH sandbox access to the host GUI is a separate boundary; installing
a command does not bypass it.

Test the final installed/extracted artifact: console output, no-display help,
no-GUI/disabled error, framework/DLL closure, spaced/Unicode paths, relocation,
install/reinstall/uninstall, and a real file/folder transaction. Keep current
macOS unsigned-package caveats; this feature does not add signing/notarization.

## 8. TDD delivery pipeline

Each implementation slice follows RED -> GREEN -> REFACTOR -> acceptance. This
execution order complements the runtime pipeline; it does not replace clear stage
ownership. Independent contract/test work can overlap, but shared owners and
schemas change in dependency order.

| Phase | Behavioral evidence before implementation | Exit gate |
| --- | --- | --- |
| P0: characterize current owners | Existing GUI filter, marks, highlighters, labels, open/close, kept panes, and parser behavior | Scope and lifetimes documented; GUI equivalence protected. |
| P1: shared commands and observation | Typed reads/writes match GUI state; every mutation invalidates relevant revisions | Narrow shared operation path, source adapters, no separate AI model. |
| P2: transaction core | Read-your-writes, isolation, conflict, all-or-none, collection phantom, abort/expiry, closed-target tests | Unpublished preparation, bounded commit, one coherent publication, no focus changes. |
| P3: asynchronous scenario parity | Open/filter/close multiple tabs; staged folder reveal; human edit during preparation; stale worker completion; live retention | Same pipeline for file/folder/live, accurate snapshot/result readiness, no leaked provisional tabs. |
| P4: CLI and local service | Offline help, parser/schema consistency, default-on/off, malformed frames, version skew, ambiguous instances, lost commit response | Real subprocess uses the same application operations and transaction engine. |
| P5: settings, packaging, docs | Shell install/repair/remove with collisions and custom paths; final-package control smoke | All supported packages include a usable CLI; settings and agent recipe are documented. |

Specific adversarial tests:

1. Agent stages filter + marks + labels; person changes the same filter; commit
   conflicts and none of the agent's writes appear.
2. Person edits an unrelated tab; the bounded transaction still commits.
3. Invalid regex or failed file load after earlier staged operations publishes
   nothing; there is no visible open-then-close flicker during rollback.
4. Programmatic marks appear in the existing GUI mark list; GUI removal is visible
   through the same API, with no provenance-based protection.
5. Global highlighter changes update affected tabs coherently; no observer sees
   the new registry with old effective rules under the committed revision.
6. A close staged against the selected tab detects concurrent presentation changes;
   last-tab close leaves an empty GUI and never deletes a source file.
7. Disable during preparation aborts the transaction; disable after commit does
   not revert committed changes. Explicit false survives save/reload/reset rules.
8. A worker finishes synchronously or late; completion is observed exactly once
   and cannot overwrite newer state or dereference a retired view.
9. Human text/IME/dialog edit sessions are not overwritten at publication.
10. A disconnected client resolves committed/unknown status without blind replay.

Use existing Catch/Qt/CTest fixtures for model and widget tests, plus subprocess
protocol tests and final-package smoke. Do not replace white-box tests wholesale
with CLI tests. The external CLI becomes an additional public-contract acceptance
surface; it does not expose test-only hooks in production.

Arm observers before starting work. Use deterministic preparation barriers to
interleave human actions, not sleeps. Timeouts are failure bounds, not timing
assertions. Preserve 30 FPS live behavior, use local-only performance budgets,
and respect per-test configuration/coordination-root isolation. macOS changes need
matching local sanitizer and packaged Cocoa/CLI smoke before push; other platform
results must be verified on their own runners, not inferred from macOS tests.

## 9. Documentation and validation status

The README, documentation hub, and technical architecture link here and explicitly
mark the control CLI as unimplemented. On delivery, update `DOCUMENTATION.md`
(embedded Help), generate the CLI/schema reference, and ship a provider-neutral
recipe: discover -> read -> stage -> commit -> observe. Keep one canonical
transaction/security contract rather than copies per harness.

This revision changes documentation only. Validate repository hygiene, relative
links, JSON examples, and consistency with accepted D1-D6 decisions. No behavioral
RED/GREEN or platform runtime verification is claimed for a proposal.

### References

- [Existing match-selection contract](MATCH_SELECTION.md)
- [Pipeline stage ownership](MATCH_SELECTION_PIPELINE_PLAN.md#3-pipeline-and-ownership)
- [Qt QLocalServer](https://doc.qt.io/qt-6/qlocalserver.html)
- [JSON-RPC 2.0 specification](https://www.jsonrpc.org/specification)
