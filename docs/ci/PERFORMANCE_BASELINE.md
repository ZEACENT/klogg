# CI performance evidence (T0)

## Status and scope

Instrumentation and a reproducible **baseline** are available. Performance
acceptance is **incomplete**: no post-change cold plus three comparable warm
platform workflow runs have been executed. No hosted whole-workflow speedup, p50
target, cache-hit threshold, or native-core reuse target is claimed achieved.
The separately retained local Intel ADB and iOS cold/three-warm measurements in
[Master cache migration](MASTER_CACHE_MIGRATION.md#local-native-cache-measurement)
are not a hosted matrix or end-to-end workflow acceptance cohort. The accepted
iOS full native-stack samples show 420.933s cold versus 243.085s warm median
(42.25% less wall time), with a separately disclosed 4,219.992s host-startup stall
excluded and replaced. That result is distinct from ADB's timing and hit-rate
measurements and does not establish hosted or ARM performance.

The first merged-master run and its 60-minute TSan timeout are analyzed in
[Master cache migration](MASTER_CACHE_MIGRATION.md), with retained PR/master
comparison evidence. PR-local warmth did not establish trusted-master warmth;
cache-action success did not prove that the backend retained a cache.

The collector is read-only. It does not dispatch workflows, change cache state,
publish artifacts, or alter quality gates. The observation and collector/report
scripts are not a benchmark scheduling framework. Compiler-artifact transport,
native caching and phased build orchestration are separate workflow execution
features; their presence does not create controlled performance samples.

Two production policy inputs must remain byte-for-byte unchanged without a real
qualified environment rebuild: `scripts/ci_build_metrics.py` and
`.github/actions/docker-build/action.yml`. `scripts/ci_build_observation.py`
reuses the original metrics implementation rather than duplicating its Ninja
analysis. Native builds use the wrapper; Linux runs the locked action unchanged
and binds host context to its optional report afterward.

## Reproduce the baseline

Evidence directory: [`evidence/pr79-39027449860`](evidence/pr79-39027449860).
It contains original API response payloads, the 14 original metrics JSON payloads,
artifact archive SHA-256 values, and selected original cache log lines with source
URLs, original line numbers, and whole-log hashes. No binary archives are stored.
The normalized `evidence.json` joins these inputs; `report.json` is derived.

```sh
# Choose a NEW directory; collection refuses to overwrite an existing receipt.
python3 -I scripts/ci_perf_report.py collect \
  --repo ZEACENT/klogg --run-id 39027449860 --attempt 1 \
  --expected-sha f4c3d005ca2204b23489855988175ecd6f994e48 \
  --output /tmp/klogg-pr79-fresh-evidence

# Fully offline, using the durable receipt and explicit dependency map.
python3 -I scripts/ci_perf_report.py report \
  docs/ci/evidence/pr79-39027449860/evidence.json \
  --dag docs/ci/evidence/pr79-39027449860/dag.json

# Multiple receipts are grouped only by comparable compiled-build identity.
python3 -I scripts/ci_perf_report.py report /path/to/cold/evidence.json \
  /path/to/warm1/evidence.json /path/to/warm2/evidence.json \
  /path/to/warm3/evidence.json --dag /path/to/explicit-run-dags.json
```

The collector paginates jobs/artifacts, checks run/attempt/head-SHA ancestry,
reports missing/expired/unreadable metrics, and downloads only `build-metrics-*`.
Each download uses a fresh temporary directory. Python runs with `-I`; archives
are size-bounded and read as JSON, never extracted or executed. Logs used for this
baseline were separately fetched read-only into fresh temporary directories and
retained as explicitly selected excerpts, not complete logs. They are not fetched
by the default metrics collector.

`dag.json` maps run ID to actual job ID to dependency job IDs. The baseline map
expands the explicit `needs` declarations at the API run-head workflow source;
`dag-source.json` records that SHA, the workflow hash, and the job mapping. Skipped
branches are omitted. Matrix siblings are separate jobs. Do not reuse old job IDs
for another run, or infer a DAG merely from timestamp adjacency.

## Verified PR #79 baseline

Run: [39027449860, attempt 1](https://github.com/ZEACENT/klogg/actions/runs/39027449860).
API run-head SHA: `f4c3d005ca2204b23489855988175ecd6f994e48`.
This is **not asserted to be the compiled checkout SHA**. Pull-request checkout
normally compiles a synthetic merge commit. The legacy artifact lacks compiled
SHA/toolchain/configuration provenance, so that identity remains unknown here;
it cannot qualify as a comparable sample by retroactively copying the API SHA.

| Measurement | Verified value | Source / interpretation |
| --- | ---: | --- |
| API created-to-updated envelope | 50m37s | Includes final workflow bookkeeping |
| Created-to-last active job completion | 50m36s | Observed wall envelope, not sum of jobs |
| Sum of active job execution | 5h06m54s | 18,414s of parallel job intervals; not elapsed latency |
| Longest dependency execution chain | 50m14s | 3,014s, excludes queue/scheduling gaps |
| Intel ADB helper job | 13m12s | Actual job interval |
| Intel macOS release job | 35m55s | Actual job interval |
| Intel macOS dependency wait | 14m34s | Run-start to last required predecessor completion |
| Intel macOS scheduling after readiness | 3s | Readiness-to-start; not dependency waiting |
| Intel macOS configure + compile | 26m44s | Composite step; API cannot split its inner steps |
| Intel macOS compile wrapper wall | 26m02.776s | Artifact, excludes configure |
| Intel macOS tests / packaging | 4m16s / 1m32s | Separate API steps |
| Windows x64 release job | 35m08s | Actual job interval |
| Windows x64 configure + compile | 26m49s | The reported 26m49s is this step, not the whole job |
| Windows x64 compile wrapper wall | 26m14s | Artifact; object cache explicitly disabled |
| Ubuntu 22.04 release current-build hits | 99.37% | 947 hits / 953 cacheable requests, artifact deltas |

The longest chain is native dependency preflight -> ADB source prefetch -> ADB
legal assets -> Intel ADB helper -> Intel macOS release -> ci-gate. It is not the
sum of all platform jobs. The initial API queue interval is zero for this run;
that does not imply each job acquired a runner immediately. Intel ASan, for
example, had a substantial post-readiness scheduling interval.

API composite actions expose only their combined duration. Reports preserve
`configure_compile`; separate `configure` and `compile` API phase fields are null
when unavailable. The compile-wrapper measurement is separately available in
`build_metrics`. Subtracting it from the composite duration is not labeled pure
configure time because it includes wrapper/step overhead. Unknown steps remain
`other`; absent phases remain null, never synthetic zero. The user-supplied
"Linux preparation 40s" has not been verified as a consistently defined phase
across this matrix, so it is not used as a measured acceptance baseline.

## Cache evidence and bounded budgets

Artifact `cache_deltas` are before/after counters for this build only. By contrast,
`ccache -s` log hit totals can include restored history: the macOS ASan log says
49.65% while its current-build artifact proves 716/722 = 99.17%. Never use those
accumulated log totals as current-run hit rates.

| Lane | Current-build hits | Restore evidence | Post-build stored footprint | Budget selected for verification |
| --- | ---: | --- | --- | ---: |
| macOS Intel release | 3/722 = 0.42% | PR and shared keys both absent | ~0.303 GB (5.64% of 5 GiB) | 500M |
| macOS ARM release | 610/720 = 84.72% | PR absent, shared exact daily key hit | 102.7% of 250 MB; 78 accumulated cleanups | 500M |
| macOS ASan/UBSan | 716/722 = 99.17% | PR absent, shared exact daily key hit | ~0.133 GB | 250M |
| macOS first-party TSan | 1/722 = 0.14% | PR and shared keys both absent | ~0.100 GB | 250M |
| Linux Ubuntu 22.04 release | 947/953 = 99.37% | Shared exact daily key hit | ~0.421 GB | 500M |
| Linux Ubuntu 24.04 release | 945/951 = 99.37% | Shared exact daily key hit | ~0.393 GB | 500M |
| Linux Ubuntu 26.04 release | 947/953 = 99.37% | Shared exact daily key hit | ~0.400 GB | 500M |
| Linux Ubuntu 20.04 AppImage | 947/953 = 99.37% | Shared exact daily key hit | Unmeasured: no statistics output | 500M, provisional |
| Linux ASan/LSan | 712/718 = 99.16% | Shared exact daily key hit | ~0.138 GB | 250M |
| Linux UBSan | 712/718 = 99.16% | Shared exact daily key hit | ~0.188 GB | 250M |

Footprints above are rounded on-disk occupancy observations, not measured minimum
working sets or unique reusable-object sizes. GiB and decimal GB/MB are not
interchangeable. The selected allowances preserve headroom above observed
occupancy and still require cold/warm verification. In particular a universal
Linux 250M cap would fall below the observed ~400 MB release caches and could
regress already high hit rates. AppImage's allowance is a provisional peer-lane
choice, not a measured footprint.

Intel's 0.42% result was a **cold restore absence**, not evidence that a compiler
cache implementation was broken or that eviction caused those misses. ARM shows
capacity pressure, but its 78 cleanups are accumulated, not a current-run cleanup
delta. These logs do not provide per-entry miss reasons. Windows baseline object
caching was disabled; a new sccache budget has no comparable baseline occupancy
or warm-run proof yet.

Linux TSan's qualified image has no ccache. That lane remains explicitly disabled;
unchanged-third-party recompilation exclusion is pending a real rebuilt and
requalified environment, not silently asserted fixed.

## Current acceleration and observation scope

The current workflow combines three distinct mechanisms:

- **Ref-scoped backend caches:** validated daily ccache/sccache generations use
  fresh compatible-version lookup plus exact-ref REST persistence checks and a
  bounded recovery path. GitHub's PR/master cache visibility rules still apply.
- **Cross-ref compiler-object artifacts:** canonical same-repository PRs and
  master pushes explicitly share trusted-code compiler data through authenticated
  Actions artifacts. Compatibility binds actual tools, configuration, dependencies,
  producer policy and existing budgets, not the application SHA. Admission still
  requires a successful producing job and its required gates, even when unrelated
  jobs made the aggregate fail or cancel. Forks/dispatches do not enter the bridge.
  This transport is neither a compiler-environment replacement nor qualified
  ADB/iOS binary-core reuse.
- **Native overlap and object caching:** ordinary macOS ADB and iOS jobs retain
  compiler objects but still build/link/verify fresh native outputs. Package-enabled
  macOS application jobs prepare objects and test binaries while those producers
  run, rendezvous with authenticated same-run artifacts, then finalize the same
  Ninja tree with native gates enabled before tests/package qualification.
  Package-free sanitizer jobs retain the single full-build path.

Artifact IDs, backend restore keys, acquisition decisions and current-build counter
deltas are separate evidence. An imported archive is not automatically warm; a
recent snapshot can contain a different application's source working set. Verify
that acquisition preserves a warmer own-PR backend, and measure refresh, transfer
cost and occupancy rather than counting an artifact download as a compiler hit.
A repository/context-bound backend hint is stamped only at the validated save
boundary. Matching application-tree hints retain the backend instead of replacing
it; native hints depend on the native/compiler identity, not application source.
Without a matching hint, discovery prefers an authenticated current-tree
application snapshot before the newest compatible snapshot. Current-tree
matching guides acquisition/freshness, not compiler-artifact compatibility.
A young snapshot suppresses export only if it covers that application tree, or
matches the native dependency identity. The refresh threshold is one day.
Hints are guidance, not attestations or warm-state evidence, and are never
transported as shared object data. A retained backend has no imported artifact ID;
metrics continue to identify the backend key even if discovery found a snapshot.

Shared artifacts use existing per-label object caps, bounded candidate/download
budgets and two-day retention. Their storage is distinct from the Actions-cache
quota. Neither those bounds nor fewer duplicate uploads prove repository-wide
storage or cost reduction; retained runs and concurrent lanes must be measured.

The macOS TSan baseline covers first-party code plus mimalloc and TBB, not all
source-built dependencies or prebuilt Qt. That scope, exact per-command debug
flags and LTO-off are preserved. The Intel macOS TSan workflow row now explicitly
enables the accepted pure-`-O1` profile through `KLOGG_CI_MACOS_TSAN_O1=ON`.
A macOS/thread-only runner step checks the profile beneath `$KLOGG_WORKSPACE` and
appends the absolute checkout path to `cmake/CiMacosTsanProfile.cmake` as
`CMAKE_PROJECT_INCLUDE` before cache identity is described. The matrix does not
supply a relative include, which would fail for nested dependency `project()`
calls. Ordinary/module defaults remain off. A full/macOS/thread-only guard checks
generated commands between configure and build. This optimization change is separate from compiler-object
reuse and does not activate the earlier mixed optimization/debug experiment.

## New observation/report contract

Single-build native action invocations preserve the existing `ci_build` command
and child exit status. They call `ci_build_observation.py run-build` with the
existing roots, output and object-cache mode, plus `--cache-tool ccache|sccache`.
Windows activates sccache only after its helper confirms activation; absence
remains disabled.

Package-enabled macOS overlap instead records `measurement_scope: phase` for
prepare and finalize. Prepare performs exactly one additional identical
native-OFF configure before compilation to stabilize pinned dependency-generated
headers; configuration passes, command hashes and their wall times are recorded
separately from build wall time. The native-ON final configure still runs. Only
successful finalization can publish
`measurement_scope: phased-build`; its `build_wall_ms` sums the two build
invocations and excludes configure, native-producer waiting, tests and packaging.
Phase provenance must agree on source/run/toolchain identity and all recorded
CMake options except the two intentional native gate changes. Cache deltas are
summed only when both phases supply valid compatible counters. Raw Ninja history
is retained: append-only suffixes describe phase-local work, while log compaction
makes that phase's compilation analysis unknown rather than inventing work.

The aggregate retains both phase reports, not a fabricated union of Ninja clock
origins. Standalone partial phases cannot establish a whole-build performance
sample. Single-build and phased-build measurements belong to separate comparison
groups. Use a fresh explicit DAG for each overlap run and retain the rendezvous
step interval; the historical native-producer-to-application dependency maps do
not describe the new within-job wait. Never sum concurrent native/application
intervals to claim workflow latency.

For Linux, the workflow runs this on the **host**, immediately before upload:

```sh
python3 scripts/ci_build_observation.py bind-environment \
  --metrics "$KLOGG_BUILD_ROOT/ci-build-metrics-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT.json" \
  --identity "$KLOGG_CI_ENVIRONMENT_IDENTITY"
```

The identity must be the verified immutable image reference from environment
resolution, not a mutable tag. The host supplies `GITHUB_SHA`, `GITHUB_RUN_ID`,
`GITHUB_RUN_ATTEMPT`, `KLOGG_CONFIG_ARCH`, `KLOGG_LABEL`, and
`KLOGG_CONFIG_PACKAGE_TAG`. Native environment identity defaults to runner
`ImageOS/ImageVersion`. The SHA denotes the workflow's compiled checkout under
this workflow's default checkout behavior; custom checkout refs must supply the
actual checkout SHA instead. Compiler ID/version/target, build type, boolean
configuration switches and compiler flags come from generated CMake files, not a
caller label alone. Files are parsed as text, never executed. Binding rejects
existing mismatched identity and stale run filenames; a missing optional metrics
file is a notice, not a build failure. No compiler inputs or qualified action
bytes change to carry telemetry.

`KLOGG_CI_CACHE_STATE` is `cold`, `warm`, or `unknown` (default). Set it only with
recorded restore/seed evidence; a restore hit alone does not establish a complete
warm working set. The observer never clears caches or reruns a build to create a
sample. A documented cold run starts with no compiler objects, while warm runs
reuse the corresponding saved compiler-object state, with a fresh build tree.

Reports group by compiled SHA, compiler identity, architecture, configuration,
immutable environment/runner identity, build type/options, cache backend and
measurement scope. Benchmark comparability still requires the same compiled
source, even though compiler-artifact compatibility intentionally does not.
Each group needs at least **one cold and three distinct warm run/attempts**.
Missing identity, duplicate samples, unknown cache state, and insufficient samples
are explicitly incomplete. Cold and warm compile-wall distributions are separate,
with sample count, p50 and maximum. Phase/job/wall values are reported per run;
no workflow-level p50 is fabricated from incomplete or mixed matrix samples.

CMake options must be a nonempty recorded string-valued mapping, as emitted by
`ci_build_observation.py`; absent, empty or malformed options cannot qualify a
group. Build durations must be finite nonnegative numbers, not booleans. Measured
cache samples additionally require all three nonnegative integer delta counters
and at least one cacheable request. Missing/malformed counters or zero requests
leave the hit rate null and the comparison incomplete, while preserving valid
compile-wall samples. Explicitly disabled caches do not require hit-rate evidence.
Malformed non-object provenance is an artifact error; missing/null legacy
provenance remains readable but cannot establish comparable identity. These
checks do not relabel the baseline: the offline command above still reproduces
its stored derived report unchanged.

Sccache JSON `counts` are summed; `adv_counts` are overlapping detail and are not
added again. Before/after deltas use an exclusive per-job server. Counter decreases
invalidate measurement. Sccache does not expose a reset epoch in these counters:
no server reset/restart or concurrent unrelated client is allowed inside the
measurement interval. The Windows helper's zeroed raw report is supplementary,
not a replacement for per-build deltas.

## PR #80 first run: incomplete observation

[`evidence/pr80-39045173443`](evidence/pr80-39045173443) retains the first PR run's
API records and 14 build-metrics payloads. The additional `windows-cache-*.json`
files retain artifacts 12105768589 (x86), 12106281413 (ASan), and 12106468997
(x64 release); the artifact API records identify their names and originating run.
The API head is `9ca328069d0d510a9a9e064ec64d7123f1c52dcc`, while the compiled
merge SHA recorded in all reports is `9b9a66f4203dd55800565f2899b8456964e0d912`.
The overall run failed, and every cache-state label remains `unknown`. It is
not an accepted cold or warm performance sample.

All three Windows jobs passed using uncached fallback: their cache reports say
`enabled: false`, `validated: false`, and `missing Windows compiler/toolchain
identity`. Native Windows uppercases environment names; copying `os.environ`
into a case-sensitive dictionary broke the mixed-case VS/SDK lookup. The fix
normalizes lookup keys, not identity values, and tests actual workflow inputs in
native-uppercase form. A later hosted run must prove activation and reuse.

Linux/macOS restored exact shared cache entries but their build deltas were
mostly misses. Linux release hits ranged from 0/953 to 16/953; macOS Intel had
3/722 and ARM 4/720. A real seeded-object regression reproduces a concrete cause:
switching `compiler_check` from the established `mtime` policy to `content`
alone invalidates the seed, without changing compiler or source. Linux PRs are
restore-only and cannot replace shared seeds, so they retain explicit `mtime`
inside the unchanged, digest-verified images. Native macOS retains `content`
for runner metadata variation and can save its own guarded PR namespace. The
probe does not attribute every CI miss; later reports must demonstrate reuse.

The observed workflow envelope was 59m55s, Windows x64 configure/build 27m23s,
and macOS Intel configure/build 29m23s. These are not controlled comparisons
against PR #79: identities/cache states differ and baseline compiled identity is
unknown. Native ADB and iOS jobs still compiled sources, and the native
qualification gate was skipped. Green individual jobs or restored archive keys
therefore cannot establish either the speed targets or acquisition-free core
reuse.

## Local Intel macOS TSan pure-O1 comparison

[`evidence/macos-tsan-o1-local-20261018`](evidence/macos-tsan-o1-local-20261018)
retains the accepted sequential baseline/pure-`-O1` pair. Whole `ci_build` wall
was **1,061.966s -> 617.019s** (41.90% lower); full 40-entry CTest wall was
**324.472s -> 290.867s**, with every test passing and zero skipped/disabled.
Combined build/test wall was **1,386.438s -> 907.887s** (34.52% lower, 478.552s
saved). This total excludes configure, acquisition and setup; `ci_build` includes
code/resource generation, archiving and linking, not only compilation.

Both used fresh separate build trees, no compiler-object cache, four compiler
jobs and eight CTest jobs. OS filesystem/page cache was not flushed. This is one
local sequential pair, not a repeated hosted distribution or a guaranteed
whole-workflow improvement. The historical baseline records above are unchanged.

All 835 compilation commands preserve effective debug flags: 819 `-g1` and
16 `-g`. Optimization changes from 534 `-O2`, 285 `-O3` and 16 `-O1` to 835 `-O1`;
other command inputs match after the documented generated-resource path
normalization. The original sanitizer gate confirms 134/134 first-party plus
54/54 mimalloc/TBB entries instrumented and 0/525 ordinary dependency entries
instrumented. Source/tool identities and normalization proof are retained.
The mixed `-O1`/light-debug exploratory result is separate and **not** activation
evidence. Full CTest for this TSan configuration does not establish full-suite
qualification of the separate package-enabled overlap build.

## Local application-overlap evidence: retained work, not a matched speedup

[`evidence/native-app-overlap-local-20261018`](evidence/native-app-overlap-local-20261018)
records a real Intel application comparison and a small real-CMake ordering
regression. The additional identical precompile configure eliminates the observed
vectorscan header-state replay: final vectorscan compile edges **207 -> 0**,
`libhs` rearchives **1 -> 0**, and prepared test relinks **8 -> 0**. Two generated
Qt resource compile edges remain and hit cache. The extra configure cost 12.256s.
Stabilized prepare/finalize build-only durations were 413.835s/173.059s, versus
482.99s/186.66s for their action intervals including configuration and overhead.

These durations are not a causal before/after speedup: the original prepare had
717 cache misses and the stabilized prepare only 3. The comparison explicitly
records unmatched cache state/strategy and source/tool/flag identities. The
measured claim is avoided finalization work, not a cold-to-warm workflow gain.

`original/` and `stabilized/` consumed authenticated historical same-run native
artifacts. Four selected CTest entries passed; **full CTest was not run**. The
separate `current-recipe-package/` receipt uses locally assembled/rebound accepted
current iOS outputs and Cocoa-smokes the **original application binary**. It is
not current-recipe application recompilation, has no GitHub artifact-ID claim,
and is not physical-device capture acceptance. These evidence scopes remain
separate from hosted, current-run native rendezvous and whole-workflow acceptance.

## Remaining acceptance

- Execute authorized cold + three warm runs per comparable target/configuration;
  preserve raw receipts and report p50/max before claiming improvements.
- Record backend keys, authenticated artifact IDs/acquisition decisions, transfer
  time, cache occupancy before/after cleanup/save, and current-build miss/cleanup
  reasons where supported. Exercise PR-to-master transfer, warmer own-PR reuse,
  partial reruns and daily refresh. Revisit provisional budgets from data.
- Retain hosted macOS prepare/finalize and rendezvous intervals, full test/package
  results and a current DAG. Local retained-work evidence and four selected tests
  do not establish full-suite/current-run overlap acceptance. Hosted overlap
  benefit and hosted/native ARM ADB/iOS cold/three-warm performance remain pending;
  the local Intel native-cache cohorts cannot fill those evidence slots.
- Validate the explicitly enabled Intel macOS TSan pure-`-O1` row on hosted
  cold/three-warm cohorts, retaining the generated-command guard, exact debug
  flags, original sanitizer scope and all tests. The accepted local pair supports
  this narrow opt-in, not a guaranteed hosted percentage or broader activation.
- Requalify Linux TSan with a real cache-capable compiler environment if that
  unchanged-build optimization is required.
- Native-core consumer migration remains blocked on the real host dependency
  chain plus the seven-target producer/publisher and lock receipts. Ordinary
  compiler-object reuse and workflow bookkeeping cannot substitute for those
  missing artifacts or authorize publication.
- Keep strict quality gates unchanged. The collector reports GitHub conclusions;
  it does not claim to audit warning annotations or prove sanitizer correctness.
