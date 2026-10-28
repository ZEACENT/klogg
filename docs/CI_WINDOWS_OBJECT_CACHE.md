# Windows CI object cache

Windows application jobs use an optional sccache compiler launcher. Only object
cache data is restored; `build_root`, executables, compiler tools and packaged
PDB bundles are never restored. Every run still configures a fresh Ninja build,
links its binaries, runs the existing tests and performs enabled package checks.

## Pinned executable

`ci/tools/sccache-windows.json` pins sccache **0.18.0**, the upstream x64 Windows
MSVC executable (both the host and supported target are x64):

- Archive: <https://github.com/mozilla/sccache/releases/download/v0.18.0/sccache-v0.18.0-x86_64-pc-windows-msvc.zip>
- SHA-256: `8965c74d5e8a225244f741e18ad2f3f504f48228dc1bac948fc22761a348363d`
- License: Apache-2.0; this build-only tool is not included in application packages.

The digest was checked against the upstream release asset metadata and the
actual downloaded archive. Acquisition bounds download/member sizes and verifies
the complete archive before copying its exact executable member into a separate
fresh directory. The executable version is checked before use. It is not taken
from an object cache, mutable action tag or a PATH-preinstalled binary. The
helper runs with isolated Python (`-I`) from the checkout, not a download folder.

## Identity, backend scope and saves

A fingerprint includes the actual `cl.exe` digest, MSVC tools and Windows SDK
versions, target architecture, Qt version/architecture, label, RelWithDebInfo
configuration, sanitizer, full CMake options (including LTO/backend settings),
sccache archive digest and preload digest. There is no cross-fingerprint restore.

- Master push: `windows-sccache-v1-<fingerprint>-master-<YYYYMMDD>`.
- PR: `windows-sccache-v1-<fingerprint>-pr-<number>-<YYYYMMDD>`.
- A PR restores its own namespace first, then compatible master data if absent.
  It never saves to master or another PR's namespace.
- Dispatch can read master data but cannot save. Unsupported events fail closed.
- Daily immutable generations restore from their exact identity/namespace prefix.
  An existing exact generation is not overwritten. Saves use the shared
  verified-save action: a version-compatible lookup and exact-ref/key API entry
  must confirm persistence. Bounded retries refresh both lookup and metadata;
  disagreement is unconfirmed, not absence. One `-recovery-<run>-<attempt>`
  generation is allowed after confirmed absence following the primary save;
  an existing healthy daily recovery is reused. An initial exact restore does
  not bypass the end-of-build persistence check.

Save occurs after successful tests and enabled package/SSL qualification, and
before shared compiler-object export and subsequent cache-evidence/package/symbol
uploads on the success path. Build-metrics upload may occur earlier and is not a
save gate. Failure-only diagnostic uploads remain on the failure path and cannot
authorize a cache save. The reporter requires current-run state, a successful statistics
reset, valid counters without cache I/O errors, configured C/C++ launchers, actual
cacheable compile flags and a bounded nonempty object directory. Links/reparse
points are rejected. The server is stopped before bounding/saving the directory.

## Cross-ref compiler-object artifacts

The backend namespace rules above do not describe the new, separate Actions
artifact bridge. Canonical `ZEACENT/klogg` same-repository PRs and master pushes
explicitly share ordinary compiler objects under a **shared trusted-code policy**.
This is not hostile-PR isolation, and it does not change GitHub's cache-service
ref visibility. Fork PRs and dispatches do not participate in this bridge.

`ci_compiler_object_descriptor.py` uses the current-run verified Windows cache
state, selected configuration/dependencies, producer policy and existing budget.
The compatibility key is independent of the application SHA: sccache still checks
each compilation's source, headers and flags. Explicit producer display names
cover x64 release and x64 ASan, including the ASan job's display name
which differs from its cache label.

`restore-compiler-object-artifact` validates the immutable artifact digest,
canonical repository/run/attempt provenance, successful producing job and its
required build/test/package/cache-validation steps. It checks policy at the
actual authenticated checkout, not the PR head when a merge commit was compiled.
A successful lane remains usable if unrelated jobs caused an aggregate failure
or cancellation; a newer failed execution of that producer does not authorize
an older success. No equality with the consumer's whole application SHA is
required. Only bounded object data is admitted before sccache activation, never
compiler tools, build trees, PDB bundles or replayed configuration.

Concurrent uploads can shift the repository's offset pages and repeat an artifact
ID. Cross-page repeats are accepted only when all admission metadata agrees;
same-page duplicates, malformed IDs and contradictions still fail closed, with
unchanged selected-artifact revalidation and three-page/twelve-candidate/
one-download/300-second limits. The deterministic regression proves this pagination
case exists; the older combined duplicate/invalid-listing error does not identify
which payload condition occurred on a historical Windows x86 run.

`export-compiler-object-artifact` runs after current-run cache validation and the
original tests/enabled package checks. The sccache server is stopped before
snapshot export. Snapshots use existing per-label object limits, bounded
candidate/download/extraction budgets and two-day retention. **Artifact storage
is separate from the Actions-cache quota.** These per-snapshot bounds do not
prove a repository-wide storage budget or cost reduction.

The save action's optional descriptor stamps a repository/context/source-tree
hint at the existing validated boundary. A matching hint retains the backend
instead of replacing it with a shared snapshot; the marker is acquisition
guidance, not an attestation or proof that every object is warm. Shared archives
exclude and reject this marker and cache configuration. Without a matching hint,
discovery prefers an authenticated current-tree application snapshot before the
newest compatible one. Other source trees remain compatible for object reuse.
Export is suppressed only when a snapshot covering the current tree is younger
than one day; an older/other-source artifact cannot suppress the changed source's
first shared export. Unmarked backends are retained on a bridge miss but do not
receive marked-backend priority.

Discovery does not masquerade as import: a retained backend's report has available
artifact metadata but no imported `artifact_id`, so build metrics keep the backend
restore key/source. A real small ccache regression exercises repeated A-to-B
acquisition without losing B's hit; it is not a native Windows sccache performance
cohort. Hosted acceptance must still confirm warm-backend preservation, refresh
and cross-ref transfer on both Windows labels.

These artifacts are an accelerator, not environment qualification or full native
binary-core reuse. Related macOS prepare/rendezvous/finalize and ADB/iOS object
caching are described in [master cache migration](ci/MASTER_CACHE_MIGRATION.md);
they do not replace any Windows runtime or installer gate.

## Debug information and unchanged build behavior

`cmake/WindowsObjectCache.cmake` is a Windows CI-only preload and compiler-rules
override. The project's legacy CMake policy baseline means setting CMP0141's
property alone is not enough. After Windows-MSVC initializes C/C++ Debug and
RelWithDebInfo defaults, the override changes only `/Zi` or `/ZI` to `/Z7`, before
CMake caches those flags. In particular it preserves `/MD` and `/MDd` under
legacy CMP0091, optimization flags and custom toolchain flags. Hardcoding an
entire config flag list before platform initialization would lose the CRT ABI
required by Qt/ASan. New-policy runtime-library behavior is unchanged, while the
Embedded debug-information property covers dependencies choosing CMP0141 NEW.
`/Z7` keeps debug information with each object instead of a shared compiler PDB,
which otherwise makes `/Zi` compilation unsuitable for reliable caching.

Linker `/DEBUG` settings and final executable PDB generation are unchanged;
symbol collection and packaging remain enabled. Release LTO remains enabled,
and the ASan leg still disables LTO. Upstream sccache's MSVC parser supports the
existing `/GL` and ASan flags; this is not a substitute for Windows runtime
qualification. ASM/RC commands do not use the C/C++ launchers and are excluded
from the C/C++ debug-flag check. No refresh rate, test gate or backend is weakened.

The separate accepted pure-`-O1` opt-in applies **only to Intel macOS TSan**.
Its module remains default-off elsewhere; no Windows optimization, `/Z7`, LTO,
ASan or package setting changes with that profile. Its local full-CTest/timing
evidence in [master cache migration](ci/MASTER_CACHE_MIGRATION.md) does not
establish a Windows hit-rate or build-time result.

## Initial budgets and fallback

The lock declares initial per-label uncompressed object-cache limits:

| Label | Initial limit |
| --- | ---: |
| `x64-qt6` | 1 GiB |
| `x64-qt6-asan` | 2 GiB |

These are **unmeasured initial working-set allowances**, not evidence that the
hot object set or aggregate repository cache quota fits. Retained generations
and concurrent PR namespaces multiply storage. Measure eviction/hit behavior and
adjust budgets and retention together. sccache enforces its disk-cache limit;
after shutdown the helper also removes oldest entries if needed to enforce the
save byte limit. A cache emptied by that process is not saved.

Download/hash/version/start/reset failures leave the original native compiler
configuration intact and report the reason. Legacy backend restore action failures
are optional; a cold compile still runs. The authenticated cross-ref artifact path
has its own fail-closed provenance/integrity checks: a malformed selected snapshot
is not silently accepted or treated as an ordinary miss. Once validated objects
are eligible for saving, unconfirmed persistence is an explicit failure after the
bounded checks/recovery path, not a silently successful save. This does not make
cache-tool installation mandatory. `SCCACHE_IGNORE_SERVER_IO_ERROR=1` permits
native compilation if server communication later fails. Bad/missing statistics
or cache validation disable saving, not the required tests or package checks.
Compilation, test and packaging failures retain their existing job-failure
semantics.

## Evidence and required Windows acceptance

`windows-object-cache-<label>` contains `windows-object-cache.json`, bound to the
run ID, attempt and SHA. It records restore matches/outcomes, activation/fallback
reason, raw statistics since the pre-configure reset, hits/misses, non-cacheable
reasons, errors and retained/pruned bytes. The job summary includes this evidence.
Build metrics separately measure before/after-build counter deltas so configure
probes do not become compilation hits. Shared acquisition reports retain artifact
identity, producer attempt and acquisition decisions separately from backend
restore keys; observers identify which transport supplied the effective cache.
Reports do not infer a hit rate from zero requests, equate an artifact download
with warmth, or claim an unmeasured performance target.

Before accepting the Windows performance target, on native Windows 2022:

1. For **x64 Qt6 and x64 Qt6 ASan**, record one cold and at least three
   warm runs of the same source/configuration. Preserve compatible object data,
   not build directories; confirm unchanged compiler/SDK identity for comparison.
2. Record actual compile wall time, object hit/miss deltas, non-cacheable reasons,
   cache errors, restored/saved generations, retained bytes and end-to-end job
   time. Compare multiple warm runs/p50/p95 against the cold baseline; inspect
   eviction and generated version/resource churn rather than assuming all
   translation units are cacheable.
3. Keep all existing unit/integration/backend and ASan checks required. Confirm
   `/Z7` in actual C/C++ commands, final linker PDBs and packaged symbols. Exercise
   cold-cache, restore-service failure and unavailable-tool fallback explicitly.
4. On the package-enabled x64 Qt6 leg, retain portable/installer runtime identity,
   silent-install/version, bundled ADB and SSL/TLS checks from
   `agent-package-win` and `verify_ssl.ps1`. Repeat with warm-cache-produced
   objects; a compiler hit alone does not qualify an installer.
5. Check own-PR backend reuse, compatible master fallback and dispatch read-only
   behavior, plus same-repository PR-to-master compiler-artifact transfer. Exercise
   a warm exact own-PR backend alongside an older shared artifact, partial reruns,
   refresh/retention and both producer display names. Prove failed producing
   tests/package qualification do not authorize object-data reuse; unrelated
   aggregate failures are not a substitute for checking the producing lane.
6. Measure artifact bytes/transfer time and retained storage separately from the
   Actions-cache quota. Two-day artifact retention and per-label caps alone do
   not establish aggregate storage or cost improvement.

Local offline tests and a CMake preload probe cover implementation contracts.
Historical first-master activation and stored sizes are retained in
[master cache migration](ci/MASTER_CACHE_MIGRATION.md); they are not a controlled
warm cohort for the expanded artifact bridge. **Native MSVC/x64/ASan/installer
acceptance and hosted cold/three-warm validation of this expanded change remain
pending. The requested >=90% hit-rate and 15-minute targets are not measured or
claimed achieved.**
