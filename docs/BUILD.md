# How to Build Klogg

## Overview

These instructions cover local source builds. See the [documentation hub](README.md)
for related guides and the [dependency inventory](DEPENDENCIES.md) for dependency
versions, provenance, and packaged live-source helpers.
Local builds can be faster because code can be optimized for current CPU instead of generic x86-64. Support for SSE4/AVX code paths
will be enabled if available on build machine.

## Getting the Source

This project is [hosted on GitHub](https://github.com/ZEACENT/klogg). You can clone this project directly using this command:

```
git clone https://github.com/ZEACENT/klogg
```

## Dependencies

To build Klogg:

- CMake 3.14 or later to generate build files
- A C++17 compiler compatible with the selected Qt and dependency versions;
  prefer the toolchains used by the current CI matrix over historical minimums.
- Git and Python 3 for source/dependency and helper verification tooling.
- Qt 5 or Qt 6 development libraries. Regular downloaded-Qt CI jobs use Qt
  6.9.3; Windows x86 compatibility uses Qt 5.15.2, and Linux's instrumented-Qt
  ThreadSanitizer job uses Qt 5.15.19. Linux distribution builds use their
  container's Qt packages; see [ci-build.yml](../.github/workflows/ci-build.yml).
  Required modules include:
  - QtCore
  - QtGui
  - QtWidgets
  - QtConcurrent
  - QtNetwork
  - QtXml
  - QtSvg
  - QtTools / LinguistTools

To build the Vectorscan regular expressions backend (default):

- A supported CPU target: x86 builds require at least
  [SSSE3](https://en.wikipedia.org/wiki/SSSE3); ARM64 uses its own backend.
  AVX-enabled x86 builds have additional CPU requirements.
- Boost (1.58 or later, header-only part)
- Ragel (6.8 or later; precompiled binary is provided for Windows; has to be installed from package managers on Linux or Homebrew on Mac)

To build installer for Windows:

- nsis to build installer for Windows
- Precompiled OpenSSL 1.1.x library (only needed for Qt5 builds; Qt6 uses Schannel TLS backend built into Windows)

Building tests:

- QtTest

Most other C++ dependencies are provided by
[CPM](https://github.com/cpm-cmake/CPM.cmake) during configuration. Some support
local-package discovery, but patched or verified dependencies such as Vectorscan,
efsw, and mimalloc must use the pinned source trees. See
[3rdparty/CMakeLists.txt](../3rdparty/CMakeLists.txt), rather than assuming a
system package can replace every dependency. Device capture also needs the
verified helper/native-library artifacts described below and in the
[dependency reference](DEPENDENCIES.md).

## Building

### Configuration options

By default Klogg is built without support for reporting crash dumps. This can be enabled via cmake option `-DKLOGG_USE_SENTRY=ON`.

Klogg uses the Vectorscan regular expressions library, with the architecture
requirements above, Ragel, and Boost headers. It can be built with only the
Qt regular expressions backend by
passing `-DKLOGG_USE_VECTORSCAN=OFF` to cmake.

The dependency build provides pinned static mimalloc through
`klogg_mimalloc_wrapper`, with `MI_OVERRIDE=OFF`; this is not a process-wide
malloc replacement. There is no public allocator-selection CMake option.
See [DEPENDENCIES.md](DEPENDENCIES.md) and `3rdparty/CMakeLists.txt`.

### Building on Linux

The release matrix builds Ubuntu 22.04, 24.04, and 26.04 packages; the
AppImage build uses Ubuntu 20.04 as its compatibility baseline. The following
Qt 6 source-build recipe is suitable for current Ubuntu development hosts
(for example, Ubuntu 24.04):

```bash
sudo apt-get update
sudo apt-get install build-essential git python3 cmake ninja-build \
  qt6-base-dev qt6-svg-dev qt6-5compat-dev qt6-tools-dev qt6-tools-dev-tools \
  libboost-all-dev ragel
```

Configure and build from the repository root:

```bash
cmake -S . -B build_root -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build_root --parallel
```

For a Qt 5 build, install `qtbase5-dev`, `libqt5svg5-dev`, and `qttools5-dev` instead of the Qt 6
packages and select that Qt installation when configuring CMake. These are
source-build instructions, not a substitute for the release packaging scripts
and their helper artifacts.

Binaries are placed into `build_root/output`.

See `.github/workflows/ci-build.yml` for more information on build process.

### Building on Windows

Install Microsoft Visual Studio 2022 with C++ support (Community edition is fine).
Note: Qt 6.10+ binaries are built with MSVC 2022.

Install a supported Qt version using [online installer](https://www.qt.io/download-qt-installer).
Make sure to select version matching Visual Studio installation. 64-bit libraries are recommended.

Install CMake from [Kitware](https://cmake.org/download/).
Use version 3.14 or later.

Download the Boost source code from http://www.boost.org/users/download/.
Extract to some folder. Directory structure should be something like `C:\Boost\boost_1_63_0`.
Then add `BOOST_ROOT` environment variable pointing to main directory of Boost sources so CMake is able to find it.

Prepare build environment for CMake. Open command prompt window and run:

```
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\vsdevcmd" -arch=x64
```

Next setup Qt paths:

```
<path_to_qt_installation>\bin\qtenv2.bat
```

Then add CMake to PATH:

```
set "PATH=<path_to_cmake_bin>;%PATH%"
```

Configure klogg solution (use CMake generator matching Visual Studio version):

```
cd <path_to_project_root>
cmake -S . -B build_root -G "Visual Studio 17 2022" -A x64
cmake --build build_root --config RelWithDebInfo --parallel
```

CMake should generate `klogg.sln` file in `<path_to_project_root>\build_root` directory. Open solution and build it.

Binaries are placed into `build_root/output`.

For https network urls support with Qt6, no additional libraries are required — Qt6 uses the Schannel TLS backend built into Windows. For Qt5 builds, download a precompiled OpenSSL 1.1.x library from https://www.firedaemon.com/download-firedaemon-openssl-1.1.1-zip and place `libcrypto-1_1*.dll` and `libssl-1_1*.dll` for the desired architecture next to the klogg binaries.

### Building on Mac OS

Current packaged builds target macOS 14.0 on Apple Silicon and macOS 15.0 on
Intel. A local source build must target a macOS version supported by its Qt
and other linked libraries; the former 10.13 baseline does not describe the
current packages.

#### Step 1: Verify Xcode Command Line Tools

First, verify that Xcode Command Line Tools are installed:

```bash
xcode-select --version
```

If not installed, run:

```bash
xcode-select --install
```

#### Step 2: Install Homebrew (if not already installed)

Homebrew is the package manager for macOS. If not installed, run:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

After installation, add Homebrew to PATH (if using Apple Silicon Mac):

```bash
echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zprofile
eval "$(/opt/homebrew/bin/brew shellenv)"
```

#### Step 3: Install Build Dependencies

Install all required dependencies using Homebrew:

```bash
brew install cmake ninja qt@6 boost ragel
```

**Notes:**
- `cmake`: Build system generator (requires version 3.14 or later)
- `ninja`: Fast build tool
- `qt@6`: Qt 6 libraries (Homebrew may provide a newer version than CI's Qt 6.9.3)
- `boost`: Boost C++ libraries (header-only part, for Vectorscan)
- `ragel`: Ragel state machine compiler (version 6.8 or later, for Vectorscan)

**Note:** If you already have Qt 5 installed, you can use Qt 5 instead. The project supports both Qt 5 and Qt 6.

#### Step 4: Find Qt Installation Path

After installation, you need to find the Qt installation path. Typical paths are:

- **Intel Mac (Homebrew default):** `/usr/local/Cellar/qt@6/<version>/lib/cmake/Qt6`
- **Apple Silicon Mac:** `/opt/homebrew/Cellar/qt@6/<version>/lib/cmake/Qt6`

You can find it using:

```bash
brew --prefix qt@6
```

Or search directly:

```bash
find /opt/homebrew /usr/local -name "Qt6Config.cmake" 2>/dev/null | head -1
```

#### Step 5: Select the Source Directory

Run subsequent commands from the repository root; CMake creates `build_root`:

```bash
cd <path_to_klogg_repository_clone>
# Keep this shell at the repository root for the commands below.
```

#### Step 6: Configure CMake

Choose the appropriate configuration command based on your Qt version:

**Using Qt 6 (recommended, matches CI):**

```bash
cmake -S . -B build_root -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKLOGG_OSX_DEPLOYMENT_TARGET=14.0 \
  -DKLOGG_GENERIC_CPU=ON \
  -DKLOGG_USE_SENTRY=OFF \
  -DQt6_DIR="$(brew --prefix qt@6)/lib/cmake/Qt6"
```

**Using Qt 5:**

Install `qt@5` separately if using this variant, and use a fresh build directory
when switching Qt major versions. `QT_DIR` selects the major when both are
installed; `Qt5_DIR` locates its component configuration.

```bash
cmake -S . -B build_root -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKLOGG_OSX_DEPLOYMENT_TARGET=14.0 \
  -DKLOGG_GENERIC_CPU=ON \
  -DKLOGG_USE_SENTRY=OFF \
  -DQT_DIR="$(brew --prefix qt@5)/lib/cmake/Qt5" \
  -DQt5_DIR="$(brew --prefix qt@5)/lib/cmake/Qt5"
```

**Parameter explanations:**
- `-G Ninja`: Use Ninja as the build system (faster)
- `-DCMAKE_BUILD_TYPE=RelWithDebInfo`: Release build with debug information
- `-DKLOGG_OSX_DEPLOYMENT_TARGET=14.0`: Apple Silicon package target; use `15.0` for the current Intel package target, or a newer value required by your installed dependencies
- `-DKLOGG_GENERIC_CPU=ON`: Build for generic CPU (matches CI)
- `-DKLOGG_USE_SENTRY=OFF`: Disable Sentry crash reporting (recommended on macOS)
- `-DQt6_DIR` or `-DQt5_DIR`: Path to Qt CMake configuration files

**If CMake cannot find Qt, manually specify the path:**

```bash
# First find Qt path
QT_PATH=$(brew --prefix qt@6)
echo "Qt path: $QT_PATH"

# Then use full path
cmake -S . -B build_root -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKLOGG_OSX_DEPLOYMENT_TARGET=14.0 \
  -DKLOGG_GENERIC_CPU=ON \
  -DKLOGG_USE_SENTRY=OFF \
  -DQt6_DIR="$QT_PATH/lib/cmake/Qt6"
```

#### Step 7: Build the Project

After successful configuration, start building:

```bash
cmake --build build_root -j$(sysctl -n hw.ncpu)
```

The `-j$(sysctl -n hw.ncpu)` flag uses all CPU cores on your Mac for parallel compilation, speeding up the process.

Alternatively, use Ninja directly:

```bash
ninja -C build_root
```

Binaries are placed into `build_root/output`.

#### Step 8: Run the Application

After compilation, the executable is located at:

```bash
build_root/output/klogg
```

You can run it directly:

```bash
./build_root/output/klogg
```

#### Step 9: (Optional) Package as DMG

Release packaging also requires the source-built ADB helper and native iOS
library artifacts, with their dependency closure; see [DEPENDENCIES.md](DEPENDENCIES.md)
and `.github/actions/agent-package-mac/action.yml`. After preparing the required
artifacts, create the DMG with:

```bash
cd build_root
cpack
```

The DMG file will be generated in the `build_root/packages/` directory.

#### Troubleshooting

**CMake cannot find Qt:**
- Ensure Qt is correctly installed: `brew list qt@6`
- Manually specify Qt path: `-DQt6_DIR=$(brew --prefix qt@6)/lib/cmake/Qt6`
- Check Qt version: `brew info qt@6`

**Cannot find ragel:**
```bash
brew install ragel
# Ensure ragel is in PATH
which ragel
```

**Cannot find boost:**
```bash
brew install boost
# Set environment variable if needed
export BOOST_ROOT=$(brew --prefix boost)
```

**Build errors:**
- Ensure all dependencies are installed: `brew list cmake ninja qt@6 boost ragel`
- Clean build directory and reconfigure: `rm -rf build_root && mkdir build_root`

Without an override, CMake chooses the macOS deployment target. Set
`-DKLOGG_OSX_DEPLOYMENT_TARGET=<target>` explicitly when matching a package build.
The target must be at least the minimum required by all linked dependencies,
including Qt.

## Running tests

Tests are built by default. To turn them off pass `-DKLOGG_BUILD_TESTS=OFF` to CMake.
Tests use Catch2 (provided by the dependency build) and the Qt Test module for
the selected Qt major version. Run CTest from the repository root:

```sh
cmake -E chdir build_root ctest --build-config RelWithDebInfo --verbose
```

`cmake -E chdir` works with the supported CMake 3.14 baseline and does not
require platform-specific shell directory commands.

Tests can run in parallel; every registered test gets its own
capture-coordination root (`KLOGG_CAPTURE_COORDINATION_ROOT`) and portable
config directory (`KLOGG_PORTABLE_CONFIG_DIR`). For example, run up to four
independent entries at once (adjust the count for your machine):

```sh
cmake -E chdir build_root ctest --build-config RelWithDebInfo --parallel 4
```

CI also runs independent test entries concurrently.

### Performance budgets (local-only gates)

Wall-clock budget assertions never gate CI: they are compiled into the tests
but only fire when `KLOGG_PERF_GATES=1` is set (see `KLOGG_CHECK_PERF_BUDGET`
in `tests/helpers/test_utils.h`). To check performance budgets locally, run
against an optimized (RelWithDebInfo, non-sanitizer) build:

```bash
python3 scripts/run_perf_gates.py            # full suite with budgets enabled
python3 scripts/run_perf_gates.py -- -R klogg_tests   # one binary only
```

Sanitizer and Debug builds distort timings; expect spurious budget failures
there. `scripts/lint_test_determinism.py` (part of the CI lint gate) rejects
new raw wall-clock assertions and unbounded timing patterns in tests.

Because the macro's expression is skipped unless `KLOGG_PERF_GATES=1`, and CI
never sets it, a `KLOGG_CHECK_PERF_BUDGET` call site is a statement that **CI
does not check that property**. Route only genuine speed budgets through it,
and mark every call site with `// lint-allow: perf-budget -- <reason>` in a real
comment within the assertion span. The lint requires a nonempty reason on that
same line; strings and bare markers do not qualify. A *correctness* or *liveness* property — "the call
only dispatches and never runs the work on the caller's thread", "the
contended lock was waited on for the configured timeout" — must be asserted
deterministically so it runs on every CI leg, by observing the mechanism
rather than the elapsed time:

- `FileWatcher::efswOperationThreadForTest()` reports which thread executed the
  last efsw operation, so a test can assert `addFile` / `updateConfiguration` /
  `checkWatches` dispatched their work instead of running it on the caller.
- `CaptureStoreTestAccess::capturePathGateWaits()` records the timeouts that
  reached the capture-path gate, so a test can assert the configured timeout
  was used instead of a hardcoded default.

### macOS first-party ThreadSanitizer: live-save guard

Changes to asynchronous live-save ownership need a ThreadSanitizer run before
push; passing normal tests or ASan/UBSan does not cover thread synchronization.
Use a separate build directory, with the same Qt version and architecture as the
CI leg when reproducing a CI report:

```bash
cmake -S . -B build_root_macos_tsan_followup -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_SANITIZER_THREAD=ON -DKLOGG_USE_LTO=OFF \
  -DKLOGG_USE_SENTRY=OFF \
  -DQt6_DIR="$(brew --prefix qt@6)/lib/cmake/Qt6"
cmake --build build_root_macos_tsan_followup \
  --target klogg_tests klogg_itests -j 8

TSAN_OPTIONS=halt_on_error=1 \
  build_root_macos_tsan_followup/output/klogg_tests \
  -platform offscreen --warn NoTests '[live-save-async]'
TSAN_OPTIONS=halt_on_error=1 \
  build_root_macos_tsan_followup/output/klogg_itests \
  -platform offscreen --warn NoTests '[live-save-filename],[live-save-cutover]'
```

Rebuild first and retain `--warn NoTests`: a stale executable with no matching
test otherwise exits successfully without exercising the regression. These
focused tests are an early guard, not a substitute for the complete sanitizer
CTest run after building all targets.

macOS first-party TSan uses uninstrumented Qt. Queued callable publication and
blocking-call ordering may not be visible to TSan. Live-save owner-thread calls
therefore use a C++ synchronized request mailbox and per-call completion, with a
named Qt slot carrying only the wakeup. Do not suppress the whole export service or treat
a sanitizer abort as a timing flake. Linux's instrumented-Qt TSan configuration
is separate and requires its pinned Qt runtime.
