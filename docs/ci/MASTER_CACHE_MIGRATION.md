# Master cache migration and the 20-30 minute target

## Evidence and conclusion

[CI Build 39091756819](https://github.com/ZEACENT/klogg/actions/runs/39091756819),
at `f0ea6a09935c8c1a3407811446536482427fc931`, ran from
2026-10-18 02:54:51Z to 03:58:10Z: **63m19s**. The Intel macOS TSan
check explicitly reported **"The job has exceeded the maximum execution time
of 1h0m0s."** This is a job timeout, not evidence of manual cancellation or a
hung test. Its build finished and 37/40 CTest entries passed, including the main
unit and integration binaries. Tests were still progressing at cancellation.
The Intel packaging job continued until 03:58:04Z; the strict final gate then
rejected the cancelled TSan lane. No publication was dispatched.

Raw API payloads, 13 master metrics, 14 comparison-PR metrics, six Windows cache
reports, annotations, cache inventory, selected log lines and archive hashes
are retained in [master-39091756819](evidence/master-39091756819).
The Intel package metrics upload timed out; its missing artifact is explicitly
recorded, not replaced with lifetime cache counters. Joined evidence files are
derived from the retained original payloads, without relabeling cache state.

The green PR run [39053583829](https://github.com/ZEACENT/klogg/actions/runs/39053583829)
used merge `ef79c643c55ae4128563a6ddfe83171713c9cf6f`. That merge, PR head
`7f776ea7461604cff2641b77a79db092ce49373a`, and master all have tree
`29eb067217f0257a1e6e6e15498e8ab9e00a85ca`. All 13 available paired metrics
have matching CMake option dictionaries and compiler/host-image identities.
The first trusted-master cache transition, not a new source regression, was the
important difference from the green PR.

## What changed on the critical path

The longest completion chain was Intel ADB producer -> Intel app/package -> gate.
These are actual elapsed intervals; parallel job durations must not be summed.

| Observation | Green PR | First merged master |
| --- | ---: | ---: |
| Workflow envelope | 47m24s | 63m19s, cancelled |
| Run start to Intel app job start | 23m17s | 14m31s |
| Intel app job duration | 24m00s | 48m42s |
| Intel TSan configure/build step | 2m58s | 45m58s |
| Intel TSan measured build wall | 110.494s | 2653.252s |
| Intel TSan current cache hits / misses | 716 / 6 | 1 / 721 |
| Intel TSan tests | 412s, complete | 422s, interrupted |
| Intel ADB source-build step | 1190s | 715s |

Native prerequisites became ready 8m46s earlier, but the downstream Intel job
became 24m42s longer. Native compiler caching and shorter dependency serialization
therefore remain useful even after application caches are warm.

The prior trusted seed run, 39031248659 at `9a8e8e42`, completed a cold Intel TSan
job in 49m32s (35m40s configure/build, 8m23s tests). Its cold Intel release build
step took 36m54s. The new cold TSan run was also slower in setup and extraction;
there is no proof that a new compiler flag caused all of the slowdown.
`-g1` and LTO-off were already enabled. The original macOS TSan scope is
first-party code plus mimalloc and TBB, not every source-built dependency.
Prebuilt Qt is not instrumented in this lane; the Linux source-built Qt TSan
lane remains the authority for Qt-internal races.

## Cache reuse was partial, not absent

- Linux Jammy/Noble/Resolute remained warm: 913/953, 943/951 and 945/953 current
  cacheable calls hit. Linux sanitizer caches also remained effective.
- Focal/AppImage still missed 232/953 calls. Linux TSan remains explicitly
  cache-disabled pending a genuinely qualified cache-capable environment.
- PR macOS jobs could restore their own `ccache-pr-v1` entries. GitHub's cache
  service does not expose PR-scoped entries to master, so those entries were not
  available to the historical master run. The separate cross-ref artifact
  transport described below does not change that service-level scope.
- Master restored old `ccache-v2` entries. PR #80 changed native macOS compiler
  checking from the previous default to `content`; old shared seeds and new
  PR-local seeds did not provide equivalent warmth. Exact per-object miss
  diagnostics were not retained, so this is not an assertion about every miss.
- ARM master recorded 5 hits / 715 misses. Intel ASan recorded 1 / 721.
  Both successfully saved their new trusted caches.
- All three Windows lanes activated sccache, produced validated error-free
  reports, and saved their first master seeds. All had zero hits. Stored sizes
  were approximately 482/270/325 MB for release/x86/ASan; warm performance is
  still unproven.
- Intel TSan never reached its success-gated save. Intel release's cache-save
  step appeared successful, but its log said reservation failed; no finalized
  daily key existed in the subsequent inventory. A blind rerun was therefore
  not guaranteed warm.

The pinned cache action rewrites reservation exceptions to a generic "another
job may be creating this cache" warning and exits successfully. This is not
proof of actual contention. The optional metrics upload also emitted an
`ETIMEDOUT` failure annotation on an otherwise successful packaging check.

The inventory contained 39 entries totaling 10,491,431,817 bytes (~9.77 GiB).
This is capacity-pressure evidence, not proof of a particular eviction. Deleting
failed workflow records does not delete compiler caches or free their quota.

## Implementation and boundaries

### Ref-scoped saves and shared compiler artifacts

`save-verified-compiler-cache` requires a version-compatible lookup plus an
exact-ref/key, positive-size REST entry. Its bounded visibility retry refreshes
both observations. Disagreement is `unconfirmed`, not permission to save;
malformed or unavailable metadata fails closed. An existing finalized daily
primary or recovery is reused. Confirmed absence permits a primary save and at
most one recovery save. A cache action's successful exit alone is not persistence
evidence. macOS writes identify the `content` compiler-check policy through
`ccache-content-v1` / `ccache-content-pr-v1`; legacy prefixes remain read-only.
Linux's qualified image policy and existing cache namespaces remain unchanged.

A separate Actions-artifact bridge now shares ordinary compiler objects between
canonical `ZEACENT/klogg` same-repository PRs and master pushes. This is an explicit
**shared trusted-code policy**, not hostile-PR isolation and not a relaxation of
GitHub's ref-scoped cache service. Fork PRs and workflow dispatches do not enter
this cross-ref bridge. Existing scoped-cache paths and dispatch read-only
behavior remain separate.

`ci_compiler_object_descriptor.py` describes actual compiler/toolchain,
architecture, configuration, dependency inputs, existing byte budgets and
producer policy. `ci_compiler_object_transport.py` and the restore/export
composites transport only bounded compiler-cache data, not build trees, tools,
executables, installed native stacks or configuration from an archive. Artifact
names do not contain the application commit SHA: each ccache/sccache compilation
still checks its own source, headers and flags. Application source revisions may
differ while the declared compatibility and policy inputs match.

Admission verifies immutable artifact digest and provenance, the successful
producing job and its required build/test/package gates, and the actual compiled
checkout's policy bytes. PR head and synthetic merge checkout identities are
kept distinct; the original attempt's authenticated local reusable-workflow
references and Git commit/tree metadata establish the merge checkout. An
unrelated failed or cancelled aggregate workflow does not invalidate a
successful producing lane. A newer failed execution of that producer cannot be
hidden behind an older successful snapshot. Missing checkout proof is a miss,
not permission to trust a manifest's self-reported revision.

Snapshots have two-day retention and use the existing per-label object cap;
candidate discovery, downloads and extraction are bounded. **Actions-artifact
storage is distinct from the Actions-cache quota.** Per-snapshot caps and short
retention do not prove a repository-wide storage budget or cost reduction,
especially with concurrent lanes and retained runs.

At the existing validated save boundary, the optional descriptor lets the save
action stamp a backend acquisition hint bound to repository and compatibility
context. Application hints also record the compiled source tree; native-only
hints rely on the dependency/compiler identity without requiring the application
tree to match. The hint is **priority guidance, not an attestation or proof of
warmth**, and is excluded from shared object archives alongside configuration.

A matching backend is retained instead of replaced by a shared snapshot. Without
that match, bounded discovery prefers an authenticated current-tree application
snapshot, then the newest compatible snapshot; a different application tree
remains eligible for compiler-object reuse. Export is suppressed only by a
compatible snapshot younger than one day that covers the current application
tree, or by a young matching native dependency snapshot. An older/other-source
snapshot therefore cannot repeatedly erase a warmer backend or suppress the
changed source's first shared export. Unmarked or mismatched backends remain
usable on a bridge miss, but do not receive this priority guarantee.

Reports distinguish discovered artifacts from imported ones: backend retention
records available artifact metadata without an imported `artifact_id`, so build
metrics retain the backend key/source. Hosted acceptance must still exercise
own-PR preservation, cross-ref transfer and refresh under storage pressure;
the reviewed acquisition logic is not an end-to-end performance result.

### Native compiler objects and macOS build overlap

Ordinary macOS ADB and iOS jobs use compiler objects outside their erased build
and install directories. ADB uses the CMake compiler-launcher environment; the
iOS builder accepts an optional executable-only launcher and passes it through
Autotools/OpenSSL `CC`/`CXX` with the selected compilers and SDK. The native cache
helper carries trusted cache configuration and observes current-build counters.
The allowance remains 128 MB per native target pending working-set measurements.
Source preparation, fresh linking, original verification/smoke, legal assembly
and current receipts remain mandatory. Dependency qualification/publisher modes
do not opt into these ordinary cache paths. No production native-core reuse or
publisher/lock activation is implied.

Package-enabled macOS jobs no longer wait for all native producers before
starting application compilation:

1. **Prepare:** configure with ADB/iOS artifact gates disabled and perform exactly
   one identical precompile stabilization configure before building reachable
   objects and the existing test-build aggregate. This settles the pinned
   dependency's generated-header state without changing dependency source,
   pre-seeding feature results or rolling back timestamps. Do not publish a
   package or run tests in this phase.
2. **Rendezvous:** authenticate successful same-run native producers and their
   artifact IDs, including the latest relevant execution on a partial rerun;
   download and verify the ADB, legal-support and iOS artifacts.
3. **Finalize:** reconfigure the same Ninja tree with both native gates enabled,
   require unchanged prepared compilation commands, and run the final `ci_build`
   before the original tests and package qualification. The build tree and Ninja
   command history are not erased between phases.

Package-free macOS sanitizer jobs retain a single full build without the native
producer rendezvous. **Only the Intel macOS TSan row now explicitly opts into the
accepted pure-`-O1` profile**, using `KLOGG_CI_MACOS_TSAN_O1=ON` and
`CMAKE_PROJECT_INCLUDE=cmake/CiMacosTsanProfile.cmake`. The module remains
default-off for ordinary/local and other builds. The shared build action checks
the generated compilation commands between configure and build only for the
full/macOS/thread path; configuration intent alone is not accepted as proof.

The profile changes optimization only. It retains the original first-party plus
mimalloc/TBB sanitizer scope, existing per-command debug flags (including
mimalloc's `-g`), LTO-off, CPU flags, job name, timeout and test/build concurrency.
The accepted local comparison is recorded below; it is not a claim of broader
sanitizer instrumentation or guaranteed hosted latency.

### Observation and unchanged gates

Build observations record the effective cache transport, artifact ID or backend
key and compiler-check policy, and mirror current reports in the build log before
optional artifact upload. Overlap builds retain separate prepare/finalize evidence
and publish a whole-build report only after successful finalization. Its build
wall time is the sum of those two build invocations, excluding configure,
rendezvous, tests and packaging; it is not workflow latency. Ninja logs from
separate invocations are not treated as one continuous clock.

No test, sanitizer scope, package verification, timeout or final-gate rule is
relaxed. Compiler-object publication remains after the producing lane's required
validation, not before it to escape the cold-build -> timeout -> no-seed loop.
Production compiler-environment policy inputs and native-core refusal conditions
remain unchanged. The old-CMake Windows preload probe skips only when required
policies are unavailable; other Windows cache tests still run.

## Local native-cache measurement

### Intel ADB

The original Intel ADB source build, fresh legal overlay, package smoke and
artifact verification passed with cold objects and three warm samples on the
pinned Xcode 26.6 / CMake 3.31.6 / Ninja 1.12.1 tools. Source-build time was
340.686s cold and 50.960s, 44.884s, 43.102s warm: warm median 44.884s, worst
50.960s. Each warm sample hit 707/752 cacheable calls (94.02%). Actual payload
remained below 44 MB, versus the 128 MB per-target allowance, with no cleanup.

[Original local counters and verification receipts](evidence/native-adb-local-20261018)
are retained separately. These measurements used local inspection mode, not a
claimed hosted image or release qualification. ARM-native validation and full
workflow latency remain unmeasured. Fresh linking still occurs and produces
UUID/timestamp differences; complete binary reproducibility is not claimed.

### Intel iOS native stack

[Local iOS native-cache evidence](evidence/native-ios-local-20261018) retains four
accepted fresh source-build/verification samples using pinned Xcode 26.6,
CMake 3.31.6 and Ninja 1.12.1. Full sample time was **420.933s cold**, then
**236.620s, 243.085s and 270.704s warm**. The accepted warm median was **243.085s**,
**42.25% less wall time** than cold (1.73x speedup); the worst accepted warm
sample was 270.704s. This is a local native-stack result, not ADB's source-build
measurement or a hosted/application/workflow timing claim.

Each accepted warm sample recorded 3,095 direct hits and zero misses: **100% of
cacheable calls**, not every tool invocation or all work. Maximum accepted cache
occupancy was 28,448,270 bytes (28.45 MB), below the 128 MB allowance. All four
samples passed fresh source/Mach-O verification, fresh-process loading and export
checks for the eight-dylib closure, the os_trace contract, and source/legal-overlay
verification. Source archive hashes and recorded dylib hashes matched across
those four samples; this is not a general binary-reproducibility guarantee.

**One original third-warm attempt is excluded and disclosed**, not folded into
the accepted distribution. It stalled at shell startup in `_dyld_start` for an
estimated **4,219.992s**, was sampled and terminated, and was replaced by a fresh
full build. The [exclusion record](evidence/native-ios-local-20261018/host-stall-exclusion.json)
retains the observed state, sampled-frame summary and log/state/sample hashes;
complete log dumps and binaries are not checked in. The accepted median above
therefore describes the stated four-sample cohort, not every attempted run.

This is local native Intel acceptance only. Hosted and native ARM behavior,
repository-wide cache/artifact storage, and end-to-end CI latency remain
unmeasured. It does not qualify the separately blocked host-tool/native-core
closure or activate a production dependency lock.

## Local Intel macOS TSan pure-O1 acceptance

[Retained TSan profile evidence](evidence/macos-tsan-o1-local-20261018) records
one sequential local baseline/pure-`-O1` comparison, with fresh separate build
directories and compiler-object caching disabled. OS filesystem/page caches were
not flushed. Compiler parallelism was four and CTest parallelism eight in both
runs; no competing heavy validation was active.

- Whole `ci_build` wall: **1,061.966s -> 617.019s**, **41.90% less**.
- Full 40-entry CTest wall: **324.472s -> 290.867s**; all 40 passed with zero
  skipped/disabled tests.
- Build plus tests: **1,386.438s -> 907.887s**, **34.52% less**, saving **478.552s**
  in this local pair.

`ci_build` includes compilation, generated code/resources, archiving and linking;
it is not isolated compiler time. The combined total excludes configure,
acquisition and environment setup. These are one-pair local observations, not a
hosted percentage guarantee, repeated cold/warm distribution or proof of the
20-30 minute whole-workflow target.

The 835-command comparison preserves effective debug flags exactly: **819 `-g1`
and 16 `-g`** in both builds. Optimization changes from 534 `-O2`, 285 `-O3` and
16 `-O1` commands to 835 `-O1` commands. Apart from the documented generated Qt
documentation-resource path normalization, all other command inputs remain
matched. The unchanged sanitizer-scope gate confirms **134/134 first-party**
and **54/54 required mimalloc/TBB** entries instrumented, with **0/525 ordinary
dependency** entries instrumented. This preserves the original scope; it does
not imply all source-built dependencies are sanitized.

The retained intermediate mixed optimization/light-debug experiment is explicitly
**not** the activation evidence. The accepted pure optimization profile preserves
debugging and sanitizer inputs rather than combining a flag reduction with the
optimization change. Its successful full TSan CTest run does not stand in for
full-suite acceptance of the separate package-enabled overlap configuration.

## Local application-overlap acceptance

[Local overlap receipts](evidence/native-app-overlap-local-20261018) retain
original and stabilized Intel application runs plus a small real-CMake regression.
The pinned vectorscan configure order initially generated `config.h` before
caching `HAVE_MMAP`; the final configure then changed that header and replayed
prepared work. Exactly one identical native-OFF configure before compilation
settles this state. The normal native-ON final configure remains mandatory; no
protected policy/source file change, feature-result pre-seeding or timestamp
rollback was used.

The deterministic finalization comparison is:

- vectorscan compile edges: **207 -> 0**;
- `libhs` rearchives: **1 -> 0**;
- already-prepared test executable relinks: **8 -> 0**;
- two generated Qt resource compile edges remain, both served from cache.

The additional stabilization configure cost **12.256s** in this local sample.
Stabilized build-only times were **413.835s prepare / 173.059s finalize**; action
times were **482.99s / 186.66s**, including configuration and other overhead.
**Do not infer a causal total-duration speedup:** the original prepare had 717
cache misses versus only 3 in the stabilized run. Cache state and configure
strategy differ; the retained source/tool/flag/strategy records do not turn them
into a matched cold/warm performance comparison. The result establishes avoided
build work, not a hosted workflow latency improvement.

Both `original/` and `stabilized/` use authenticated historical same-run native
artifacts from master run 39091756819. That identity authenticates the downloaded
fixtures, not a new GitHub execution. Four selected CTest entries passed
(`klogg_smoke`, `adb_helper_verifier_contract`, `ios_native_release_contract`,
`klogg_ios_native_contract_tests`); **the full CTest suite was not run**. These
local runs retain package/native verification and Cocoa smoke receipts.

`current-recipe-package/` is a separate local package proof using the newly
accepted iOS recipe. Its source/legal overlay was assembled and rebound locally,
then Cocoa-smoked with the **original application binary**, not a newly compiled
or stabilized application. It has no claimed GitHub artifact IDs, does not
establish full current-recipe application recompilation, and does not test
physical iOS device capture. The historical fixture and current local recipe
proofs must not be presented as one continuous hosted acceptance run.

## Final local script-quality gate

After the shared-cache, native-overlap, configuration-stabilization and pure-O1
integrations, `python3 scripts/run_ci_quality.py --json` passed all six lints and
ran 2,468 script tests: 2,460 passed, eight skipped, zero failures or errors.
The final suite took 1,125.039 seconds. The aggregate unittest output does not
list the individual skip reasons. Earlier integration failures were repaired
before this complete rerun; focused tests are not substituted for this result.

The [compact quality receipt](evidence/local-quality-20261018.json) records the
commands, per-check results and complete-output hash. This was a local dirty
worktree check against base `f0ea6a09935c8c1a3407811446536482427fc931`, not a new
hosted run. Historical Actions identity variables supplied test-contract context
only. This script suite is separate from the native CTest/package evidence above
and does not qualify hosted latency, cross-platform runtime or production cores.

## Remaining acceptance work

**The 20-30 minute workflow target is not achieved or proven by these changes.**

1. Complete a trusted cold run, including the Intel TSan lane, then at least
   three comparable warm runs with exact compiled source, compiler, SDK,
   architecture and configuration identity. Report median and worst workflow
   latency, not only compiler time. Historical collected states remain `unknown`.
   No post-change hosted cohort is supplied by the implementation itself.
2. Confirm Windows warm cacheable-call hits >=90% and configure/build <=15m on
   all three labels. Exercise same-repository PR-to-master artifact reuse,
   unchanged own-PR warm reruns, partial reruns, snapshot refresh and cache-service
   recovery. Confirm the existing budgets retain working sets without churn.
3. Measure ADB and iOS object-cache behavior on both native CI architectures.
   The local Intel ADB and iOS cohorts above do not establish hosted or ARM
   performance. Retain full source-build/verification evidence, effective
   occupancy and all excluded attempts on cold and three warm samples; pending
   hosted/ARM work must not be reported as completed.
4. Complete hosted and full-suite macOS prepare/rendezvous/finalize acceptance
   with current-run artifacts. Local receipts establish the retained-work result
   and four selected tests, not full CTest or a matched hosted speedup. Keep the
   historical-native-fixture runs separate from the current-recipe local package
   proof. Record native wait, each configure/build phase and the end-to-end
   workflow envelope separately, with a current DAG.
5. Investigate Focal misses and cold Intel variability with current-run data.
   A timeout increase is containment, not a latency improvement. Reducing the
   established sanitizer scope or trusting failed-build objects is not a remedy.
   The explicitly enabled Intel macOS pure-`-O1` row has local scope-preservation
   and full-test evidence, but still needs hosted cold/three-warm validation.
   Preserve exact debug/sanitizer inputs and the generated-command guard; do not
   extrapolate the one local pair to a guaranteed whole-workflow improvement.
6. Complete real iOS host input/execution closure, seven-target qualification,
   signed companion assets, controlled publication and a reviewed production
   lock before claiming native cores are reused without source builds. Ordinary
   compiler-object artifacts do not supply that evidence. Publication and lock
   activation still require separate authorization.

The observer/reporter can retain and compare evidence, but there is not yet a
controlled benchmark scheduler that creates cold/three-warm workflow cohorts.
A green functional PR remains necessary, not sufficient, for this acceptance.
