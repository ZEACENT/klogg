# Windows CI object cache

Windows application jobs use an optional sccache compiler launcher. Only object
cache data is restored; `build_root`, executables, compiler tools and packaged
PDB bundles are never restored. Every run still configures a fresh Ninja build,
links its binaries, runs the existing tests and performs enabled package checks.

## Pinned executable

`ci/tools/sccache-windows.json` pins sccache **0.18.0**, the upstream x64 Windows
MSVC executable (the host is x64 for both x64 and x86 target compilers):

- Archive: <https://github.com/mozilla/sccache/releases/download/v0.18.0/sccache-v0.18.0-x86_64-pc-windows-msvc.zip>
- SHA-256: `8965c74d5e8a225244f741e18ad2f3f504f48228dc1bac948fc22761a348363d`
- License: Apache-2.0; this build-only tool is not included in application packages.

The digest was checked against the upstream release asset metadata and the
actual downloaded archive. Acquisition bounds download/member sizes and verifies
the complete archive before copying its exact executable member into a separate
fresh directory. The executable version is checked before use. It is not taken
from an object cache, mutable action tag or a PATH-preinstalled binary. The
helper runs with isolated Python (`-I`) from the checkout, not a download folder.

## Identity, isolation and saves

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
  An existing exact generation is not overwritten.

Save occurs after successful tests and enabled package/SSL qualification, and
before metrics/cache evidence/package/symbol artifact upload on the success path.
Failure-only diagnostic uploads remain on the failure path and cannot authorize
a cache save. The reporter requires current-run state, a successful statistics
reset, valid counters without cache I/O errors, configured C/C++ launchers, actual
cacheable compile flags and a bounded nonempty object directory. Links/reparse
points are rejected. The server is stopped before bounding/saving the directory.

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
symbol collection and packaging remain enabled. Release/x86 LTO remains enabled,
and the ASan leg still disables LTO. Upstream sccache's MSVC parser supports the
existing `/GL` and ASan flags; this is not a substitute for Windows runtime
qualification. ASM/RC commands do not use the C/C++ launchers and are excluded
from the C/C++ debug-flag check. No refresh rate, test gate or backend is weakened.

## Initial budgets and fallback

The lock declares initial per-label uncompressed object-cache limits:

| Label | Initial limit |
| --- | ---: |
| `x64-qt6` | 1 GiB |
| `x86-qt5` | 512 MiB |
| `x64-qt6-asan` | 2 GiB |

These are **unmeasured initial working-set allowances**, not evidence that the
hot object set or aggregate repository cache quota fits. Retained generations
and concurrent PR namespaces multiply storage. Measure eviction/hit behavior and
adjust budgets and retention together. sccache enforces its disk-cache limit;
after shutdown the helper also removes oldest entries if needed to enforce the
save byte limit. A cache emptied by that process is not saved.

Download/hash/version/start/reset failures leave the original native compiler
configuration intact and report the reason. Cache action failures are optional;
a cold compile still runs. `SCCACHE_IGNORE_SERVER_IO_ERROR=1` permits native
compilation if server communication later fails. Bad/missing statistics or cache
validation disable saving, not the required tests or package checks. Compilation,
test and packaging failures retain their existing job-failure semantics.

## Evidence and required Windows acceptance

`windows-object-cache-<label>` contains `windows-object-cache.json`, bound to the
run ID, attempt and SHA. It records restore matches/outcomes, activation/fallback
reason, raw statistics since the pre-configure reset, hits/misses, non-cacheable
reasons, errors and retained/pruned bytes. The job summary includes this evidence.
Build metrics separately measure before/after-build counter deltas so configure
probes do not become compilation hits. Reports do not infer a hit rate from zero
requests or claim an unmeasured performance target.

Before accepting the Windows performance target, on native Windows 2022:

1. For **x64 Qt6, x86 Qt5 and x64 Qt6 ASan**, record one cold and at least three
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
5. Check own-PR reuse, compatible master fallback and dispatch read-only behavior;
   prove failed tests/package qualification do not publish object data.

Local offline tests and a CMake preload probe cover implementation contracts.
**No native MSVC/x86/ASan/installer or hosted cold/warm acceptance has been run
for this change. The requested >=90% hit-rate and 15-minute targets are not
measured or claimed achieved.**
