# Compiler-object recovery after partial reruns

## Incident and evidence

Master run [40790961382](https://github.com/ZEACENT/klogg/actions/runs/40790961382)
completed successfully in 60m20s, compared with 24m30s for the preceding master
run 39696850322. The Intel package lane took 54m08s and Intel TSan took 44m32s.
Current-build counters, not lifetime cache statistics, showed 729 misses out of
734 calls and 731 misses out of 732 calls respectively.

Both ref-scoped Intel caches were absent. Their precise eviction/deletion cause
is not established. The shared object bridge also reported a miss even though
PR run [40765051993](https://github.com/ZEACENT/klogg/actions/runs/40765051993)
retained unexpired Intel artifacts with identical compatibility keys. Its merge
checkout and master had the same Git tree, `86a3de0fdc9cc19d618531ef09fc1a22a146e96e`.

The PR finished at aggregate attempt 5. Intel's successful executions and object
exports belonged to attempt 1. The old admission rule rejected any artifact
more than three aggregate attempts behind, before checking the effective lane
or authenticated checkout. Replaying the authenticated metadata reproduced that
rejection for both Intel lanes.

GitHub's attempt-5 jobs response included inherited Intel successes with new job
IDs and `run_attempt=5`, but the original attempt-1 execution timestamps and
steps. An aggregate rerun count is therefore not an execution failure signal.

## Admission contract

- Authenticate the artifact's original producing attempt, successful lane,
  required validation/export steps, execution time window and checkout proof.
- Inspect the newest effective producer lane. Inherited successful records do
  not invalidate an earlier artifact, regardless of the aggregate attempt gap.
- If a newer attempt omits the lane, inspect older attempts until the effective
  lane is found. A newer failed or incompletely validated lane still rejects
  the older snapshot; an unrelated cancelled rerun cannot hide that failure.
- Keep repository/event, toolchain, policy bytes, digest, archive and source
  checks unchanged. A successful latest attempt cannot authorize a failed
  original producer or supply its missing checkout proof.
- Bound discovery by 128 metadata requests, three artifact pages, twelve
  candidates and the existing 300-second deadline. Request-budget exhaustion
  retains an already fully authenticated fallback, if one exists, without
  admitting the interrupted candidate. Otherwise it leaves backend objects
  intact, imports nothing and requests a fresh export. The metadata deadline
  is optional unavailability too: a verified fallback cannot start a download
  after the transport time budget is spent. Download overruns and integrity
  failures remain fatal; they are not optional cache misses.

Regression coverage models the observed inherited-job response at attempts 5,
10 and 100, and verifies that a present effective lane needs only the original
and latest attempt/job responses, not a walk across the entire gap.

## Recovery reasons and lifecycle

The acquisition report records candidate decisions, exclusions, metadata request
count, pages searched and whether discovery completed or hit a budget. A global
artifact-page limit is not proof that no compatible snapshot exists.

Selected snapshots report their age, remaining retention eligibility and time
until refresh. The existing two-day retention and one-day refresh threshold are
unchanged: the incident involved valid, hours-old artifacts, not insufficient
retention. Increasing retention or backend capacity would not fix that failure.
A changed source tree or an old snapshot still requests a verified new export;
a young snapshot covering the same tree can suppress a redundant export.

The composite action publishes the report in the job summary and propagates its
path to build observation. Metrics bind it to the consumer checkout, run and
attempt before embedding it. Acquisition is deliberately distinct from measured
cache hits: a bridge miss can coexist with useful legacy backend objects, and
imported objects still undergo compiler input checks.

## Remaining link work

The incident's phased Ninja evidence shows:

| Measurement | Windows x64 | Intel macOS |
| --- | ---: | ---: |
| Prepare build | 930.249 s | 2209.553 s |
| Last prepare object completed | 597.940 s | 1996.562 s |
| Link tail after last object | 332.309 s | 212.991 s |
| Finalize build | 253.596 s | 184.030 s |
| Finalize compiled objects | 2 | 2 |

Finalization logs are cumulative; these counts exclude preparation records.
There was no wholesale recompilation at finalization. Six heavy test links ran
concurrently in preparation. Existing measurements do not prove memory pressure
or establish a better global worker count.

Regex and folder benchmark inputs were ready long before preparation completed,
but their links were deferred to finalization. The authoritative CI benchmark
aggregate moves those existing links into preparation under the same graph
check that excludes package applications. It does not add the process-integrated
benchmark to CI, change LTO/debug/optimization flags, remove tests, or weaken
native/package validation.

Those two benchmark links accounted for 409.431 s of cumulative edge time on
Windows and 357.722 s on Intel. These are overlapping task durations, **not** a
promised wall-time saving. Preparation can grow when links move earlier; compare
prepare plus finalize and complete job time, not finalization alone.

## Hosted acceptance procedure

1. Run the full PR matrix with unchanged tests, sanitizers and package checks.
   The policy change intentionally creates a new shared-artifact identity;
   its first run may seed rather than consume that identity.
2. Exercise repeated partial reruns while retaining successful original producer
   artifacts. Inspect authenticated original/latest attempt and job metadata.
3. Trigger a subsequent PR run with unchanged build policy and ordinary source
   inputs, and verify that the earlier successful artifacts remain admissible.
4. Collect at least two warm samples. Record run/attempt/SHA, actual acquisition
   source, current hit/miss deltas, prepare/finalize timing, test/package timing
   and whole-workflow critical path. Do not treat a lucky backend hit as proof
   that rerun admission was exercised.
5. Check review comments and successful-check annotations as well as failures.
   Record final measured results in the linked PR acceptance report before
   declaring the latency goal achieved. Updating documentation after every run
   would itself trigger another run and change the consumer revision.

The incident's unaffected Windows lane completed at workflow +31m25s. Repairing
both cold Intel lanes alone cannot promise a sub-30-minute workflow with the
other timings held fixed. Hosted measurements, rather than a lower timeout or
reduced validation scope, determine acceptance.

## Hosted evidence and acceptance record

The complete, current acceptance result is maintained in
[PR #86](https://github.com/ZEACENT/klogg/pull/86). This documentation-only update
also supplies a new ordinary PR consumer run without changing the compiler
policy or application source inputs, so cross-run admission can be tested after
partial reruns of the producer.

Initial run [40901901224](https://github.com/ZEACENT/klogg/actions/runs/40901901224),
on `8937a393`, finished in 35m06s but **failed** and is not an accepted green
sample. Its current-build counters and measured build-invocation times were:

| Lane | Compiler hits / calls | Build time | Job time |
| --- | ---: | ---: | ---: |
| Intel macOS ordinary | 729 / 734 | 735.000 s | 1578 s |
| Windows ordinary | 972 / 977 | 807.376 s | 1465 s |
| Intel macOS TSan | 727 / 732 | 82.648 s | 904 s (failed test) |

These gains used backend objects; shared acquisition correctly reported no
candidate for the changed policy identity. They do not by themselves prove the
partial-rerun recovery path.

The initial failure set was fully inspected:

- ARM64 iOS and ADB producers executed no steps and were cancelled by GitHub:
  "The job was not acquired by Runner of type hosted even after multiple
  attempts." Capacity annotations explicitly identified macOS ARM64 runners.
- The ARM64 application prepared successfully, then correctly refused the
  cancelled same-run iOS producer at the native rendezvous.
- Intel TSan failed one existing CaptureStore child-readiness assertion at
  `tests/unit/capturestore_test.cpp:3429`. The child had reported process start
  but no main-entry phase at the readiness deadline, then exited normally with
  code zero during cleanup without being killed. No TSan race report was
  emitted. This is recorded as a failed sample, not waived as a passing flake.
- Windows, Linux, Intel ASan/UBSan, CodeQL, static analysis, coverage and lint
  passed. Inspection found no additional actionable successful-check warning.

Review follow-up `2128922a` addresses optional metadata-deadline handling and adds
successful restore/metrics integration coverage. Its clean-tree local quality
receipt passed all lint gates and 2,594 script tests (12 platform-dependent
skips). Run [40910224083](https://github.com/ZEACENT/klogg/actions/runs/40910224083)
starts the cache-admission producer sequence; final green, rerun-admission and
warm sample receipts belong in the PR acceptance report rather than being
inferred from the failed initial sample.

## Native rendezvous after many partial reruns

The delayed ARM64 recovery exposed a second history-dependent failure in consumer
run 40919941215, attempt 6. The ADB producer finally succeeded in attempt 5, and
the application prepared successfully in attempt 6, but its rendezvous spent
30 minutes retrying the historical jobs endpoint before timing out.

The exact `jobs?filter=all&per_page=100` paginated request reproduced HTTP 502
locally after 19.36 seconds. That history contained 216 job records. In contrast,
`attempts/6/jobs?per_page=100` returned the complete 36-job effective snapshot,
including inherited successful producers with their original execution intervals.

Rendezvous now reads that attempt-scoped snapshot. Pagination completeness,
repository/run/head identity, newest producer success, artifact ancestry/expiry
and upload-window checks remain unchanged. The run identity is checked again
after the artifact snapshot, so a concurrent rerun cannot authorize a stale
receipt. API retry diagnostics expose only a parsed HTTP status, not raw stderr.
The timeout is not increased.

A live replay of the fixed rendezvous against the same failed attempt resolved
all three required ARM64 artifacts in 7.83 seconds: iOS artifact 12921058724,
ADB artifact 12926829196 and support artifact 12920033497. This validates the
actual inherited-job shape rather than assuming that an aggregate rerun means
a producer rebuild. The shared path covers both macOS and Windows.

The fast deterministic guard is:

```sh
python3 -I tests/scripts/test_ci_native_overlap.py RendezvousTests
```

It rejects historical enumeration in the six-attempt scenario, retains failure
and artifact-window rejection tests, and rejects a changed run identity before
returning a receipt. The complete native-overlap module additionally exercises
real CMake/Ninja preparation and populated-artifact macOS ASan finalization.
