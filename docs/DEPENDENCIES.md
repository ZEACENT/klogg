# Dependency reference

[Documentation hub](README.md) · [Build guide](BUILD.md) · [Third-party notices](../NOTICE)

This is a reader's guide to the main dependencies, not a complete release SBOM
or a replacement for license notices. The authoritative source pins and build
conditions live in [3rdparty/CMakeLists.txt](../3rdparty/CMakeLists.txt), the root
[CMake configuration](../CMakeLists.txt), and the device-helper manifests below.
A release's source, license, and receipt assets describe the payload actually
shipped with that release.

Most C++ dependencies are fetched through [CPM](https://github.com/cpm-cmake/CPM.cmake).
Qt and Boost are provided by the build environment. Some dependencies can use
local packages; dependencies requiring verified patches or source-tree checks
must use their pinned sources. See the build guide before substituting system
libraries. Compiler-image acquisition and qualification have a separate
[CI environment contract](CI_ENVIRONMENTS.md); a reused environment does not
reuse an old application binary or test result.

## Application, search, and storage

Revisions below are abbreviated Git commits. Version labels are included where
the build identifies them; the commit, including any repository patches, is the
more precise reference.

| Dependency | Pinned source / version | Role |
| --- | --- | --- |
| [Qt](https://www.qt.io/) | Qt 5 or Qt 6; environment-provided | Native desktop UI, concurrency, networking, XML, SVG, and regular expressions; Qt Test for tests. See the build guide for current toolchains. |
| [Vectorscan](https://github.com/VectorCamp/vectorscan) | `d29730e` + patches | Accelerated regex matching when `KLOGG_USE_VECTORSCAN` is enabled; Qt handles fallback/verification. |
| [Boost](https://www.boost.org/) | Environment-provided | Build dependency for Vectorscan. |
| [simdutf](https://github.com/simdutf/simdutf) | 5.6.2 / `58c12ff` | SIMD Unicode processing. |
| [CRoaring](https://github.com/RoaringBitmap/CRoaring) | 4.2.1 / `3ef7b44` | Compressed bitmaps for line/result sets. |
| [streamvbyte](https://github.com/lemire/streamvbyte) | `c43294a` + patches | Compact integer encoding. |
| [robin_hood](https://github.com/martinus/robin-hood-hashing) | 3.11.2 / `f2cae2e` + patch | Hash maps and sets. |
| [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.1 / `35b0373` | Fast hashing. |
| [type_safe](https://github.com/foonathan/type_safe) | 0.2.4 / `1dbea79` | Type-safe utilities. |
| [oneTBB](https://github.com/variar/oneTBB) | `c9be1ac` | Task scheduling and parallel execution. |
| [mimalloc](https://github.com/microsoft/mimalloc) | 2.1.7 / `8c532c3` | Explicit allocation support; built with global allocation override disabled. |
| [efsw](https://github.com/SpartanJ/efsw) | 1.4.1 / `62f785c` + patches | File-change monitoring. |
| [uchardet](https://gitlab.freedesktop.org/uchardet/uchardet) | 0.0.8 / `ae6302a` | Text-encoding detection. |
| [maddy](https://github.com/variar/maddy) | `602e266` | Converts the canonical Markdown user guide into embedded HTML. |
| [exprtk](https://github.com/variar/klogg_exprtk) | `1f9f4cd` | Expression evaluation. |
| [KArchive](https://github.com/variar/klogg_karchive) | `f546bf6` + patch | Archive handling. |
| [KDSingleApplication](https://github.com/variar/KDSingleApplication) | `5b30db3` | Application-instance coordination. |
| [KDToolBox](https://github.com/KDAB/KDToolBox) | `6468867` | Signal throttling. |
| [whereami](https://github.com/gpakosz/whereami) | `dcb52a0` | Executable-location discovery. |

## Optional, testing, and deployment dependencies

| Dependency | Pinned source / version | When used |
| --- | --- | --- |
| [Sentry Native SDK](https://github.com/getsentry/sentry-native) | `a3d5862` + patch | Crash-reporting builds with `KLOGG_USE_SENTRY=ON`; not a universal runtime requirement. |
| [macdeployqtfix](https://github.com/arl/macdeployqtfix) | `df88850` | macOS Qt deployment tooling. |
| [Catch2](https://github.com/catchorg/Catch2) | 2.13.8 | Tests enabled through `KLOGG_BUILD_TESTS`. |

CMake, a C++17 compiler, Git, the selected build generator, and packaging tools
are development prerequisites rather than application runtime dependencies.
Vectorscan also needs Ragel. Follow the platform-specific [build instructions](BUILD.md)
for their installation and configuration.

## Android capture helper

Fresh Android sessions use the managed ADB smart-socket backend with a bundled,
source-built ADB client/server helper. The helper is a separate dependency
closure, not a system Android SDK installation or a runtime download.

- [Source lock and toolchain metadata](../packaging/adb/adb-helper.lock.json)
- [Source-build and legal-asset guide](../packaging/adb/README.md)
- [Helper superbuild](../packaging/adb/superbuild/CMakeLists.txt)
- [Capture architecture](ADB_LOGCAT_ARCHITECTURE.md)

Keep the closure's source offers, license texts, patches, and SBOM assets with
the corresponding release. This overview does not replace those obligations.
The contextual guide under `packaging/adb/` is included in source archives and
must remain at its existing path.

## Native iOS capture stack

The native stack is macOS-only. Source fetching and linking are controlled by
`KLOGG_FETCH_IOS_NATIVE_DEPENDENCIES` and `KLOGG_ENABLE_IOS_NATIVE_STACK`;
linking requires a verified `KLOGG_IOS_NATIVE_STACK_ROOT`. Fresh native sessions
do not use pymobiledevice3 or start a Python process.

| Component | Source pin |
| --- | --- |
| [OpenSSL](https://github.com/openssl/openssl) | `8cf17aa` |
| [curl](https://github.com/curl/curl) | `68720b4` |
| [libplist](https://github.com/libimobiledevice/libplist) | `cf5897a` |
| [libtatsu](https://github.com/libimobiledevice/libtatsu) | `42329cb` |
| [libimobiledevice-glue](https://github.com/libimobiledevice/libimobiledevice-glue) | `aef2bf0` |
| [libusbmuxd](https://github.com/libimobiledevice/libusbmuxd) | `adf9c22` |
| [libimobiledevice](https://github.com/libimobiledevice/libimobiledevice) | `149f762` + repository patches |

See the [native-stack superbuild](../packaging/ios-native/superbuild/CMakeLists.txt),
[dependency configuration](../3rdparty/CMakeLists.txt), and
[build guide](BUILD.md) for the source closure and staged dynamic libraries.

## Updating this reference

When changing a pin or replacing a dependency, update the actual CMake/lockfile
source first, preserve required verification and legal assets, and then update
this summary. Do not infer an exact shipped version from a package-manager
listing or from an old benchmark's environment.
