# CI performance evidence (T0)

## Status and scope

Instrumentation and a reproducible **baseline** are available. Performance
acceptance is **incomplete**: no post-change cold plus three comparable warm
platform workflow runs have been executed. No speedup, p50 target, cache-hit
threshold, or native-core reuse target is claimed achieved.

The collector is read-only. It does not dispatch workflows, change cache state,
publish artifacts, or alter quality gates. The implementation intentionally
consists of a small observation wrapper and one collector/report script, not a
benchmark scheduling framework.

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

## New observation/report contract

Native action invocation preserves the original build command and child exit
status. It calls `ci_build_observation.py run-build` with the existing roots,
output and object-cache mode, plus `--cache-tool ccache|sccache`. Windows activates
sccache only after its helper confirms activation; absence remains disabled.

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
immutable environment/runner identity, build type/options and cache backend.
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

## Remaining acceptance

- Execute authorized cold + three warm runs per comparable target/configuration;
  preserve raw receipts and report p50/max before claiming improvements.
- Record restored key/hit, cache occupancy before/after cleanup/save, and delta
  miss/cleanup reasons where supported. Revisit provisional budgets from data.
- Requalify Linux TSan with a real cache-capable compiler environment if that
  unchanged-build optimization is required.
- Native-core consumer migration remains blocked on the real host dependency
  chain plus the seven-target producer/publisher and lock receipts. Workflow
  bookkeeping cannot substitute for those missing artifacts.
- Keep strict quality gates unchanged. The collector reports GitHub conclusions;
  it does not claim to audit warning annotations or prove sanitizer correctness.
