# How to Build Klogg

## Overview

These instructions will get you a copy of the project up and running on your local machine for development and testing purposes.
Local builds can be faster because code can be optimized for current CPU instead of generic x86-64. Support for SSE4/AVX code paths
will be enabled if available on build machine.

## Getting the Source

This project is [hosted on GitHub](https://github.com/ZEACENT/klogg). You can clone this project directly using this command:

```
git clone https://github.com/ZEACENT/klogg
```

## Dependencies

To build Klogg:

- cmake 3.12 or later to generate build files
- C++ compiler with decent C++17 support (at least gcc 7.5, clang 7, msvc 19.14)
- Qt libraries 5.9 or later (CI builds use Qt 5.9.5/5.12.5/5.15.2):
  - QtCore
  - QtGui
  - QtWidgets
  - QtConcurrent
  - QtNetwork
  - QtXml
  - QtTools

To build the Vectorscan regular expressions backend (default):

- CPU with support for [SSSE3](https://en.wikipedia.org/wiki/SSSE3) instructions (for the Vectorscan backend)
- Boost (1.58 or later, header-only part)
- Ragel (6.8 or later; precompiled binary is provided for Windows; has to be installed from package managers on Linux or Homebrew on Mac)

To build installer for Windows:

- nsis to build installer for Windows
- Precompiled OpenSSL 1.1.x library (only needed for Qt5 builds; Qt6 uses Schannel TLS backend built into Windows)

Building tests:

- QtTest

All other dependencies are provided by [CPM](https://github.com/cpm-cmake/CPM.cmake) during cmake configuration stage (see 3rdparty directory).

CPM will try to find Vectorscan, TBB, uchardet and xxhash installed on build host.
If a library can't be found, the one provided by CPM will be used.

## Building

### Configuration options

By default Klogg is built without support for reporting crash dumps. This can be enabled via cmake option `-DKLOGG_USE_SENTRY=ON`.

Klogg uses the Vectorscan regular expressions library which requires CPU with SSSE3 support,
ragel and boost headers. Klogg can be built with only the Qt regular expressions backend by
passing `-DKLOGG_USE_VECTORSCAN=OFF` to cmake.

Klogg can use custom memory allocator. By default it uses TBB memory allocator for Windows, mimalloc on Linux and default system allocator on MacOS.
Memory allocator override can be turned off by passing `-DKLOGG_OVERRIDE_MALLOC`. If you want to use TBB allocator on Linux then pass
`-DKLOGG_USE_MIMALLOC=OFF`.

### Building on Linux

Here is how to build klogg on Ubuntu 18.04.

Install dependencies:

```
sudo apt-get install build-essential cmake qtbase5-dev libboost-all-dev ragel
```

Configure and build klogg:

```
cd <path_to_klogg_repository_clone>
mkdir build_root
cd build_root
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
cmake --build .
```

**_If cmake gives error about missing "Qt5LinguistTools" configuration files, try running:_**

```bash
sudo apt-get install qttools5-dev
```

Binaries are placed into `build_root/output`.

See `.github/workflows/ci-build.yml` for more information on build process.

### Building on Windows

Install Microsoft Visual Studio 2022 with C++ support (Community edition is fine).
Note: Qt 6.10+ binaries are built with MSVC 2022.

Intall latest Qt version using [online installer](https://www.qt.io/download-qt-installer).
Make sure to select version matching Visual Studio installation. 64-bit libraries are recommended.

Install CMake from [Kitware](https://cmake.org/download/).
Use version 3.14 or later.

Download the Boost source code from http://www.boost.org/users/download/.
Extract to some folder. Directory structure should be something like `C:\Boost\boost_1_63_0`.
Then add `BOOST_ROOT` environment variable pointing to main directory of Boost sources so CMake is able to fine it.

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
set PATH=<path_to_cmake_bin>:$PATH
```

Configure klogg solution (use CMake generator matching Visual Studio version):

```
cd <path_to_project_root>
md build_root
cd build_root
cmake -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
```

CMake should generate `klogg.sln` file in `<path_to_project_root>\build_root` directory. Open solution and build it.

Binaries are placed into `build_root/output`.

For https network urls support with Qt6, no additional libraries are required — Qt6 uses the Schannel TLS backend built into Windows. For Qt5 builds, download a precompiled OpenSSL 1.1.x library from https://www.firedaemon.com/download-firedaemon-openssl-1.1.1-zip and place `libcrypto-1_1*.dll` and `libssl-1_1*.dll` for the desired architecture next to the klogg binaries.

### Building on Mac OS

Klogg requires macOS High Sierra (10.13) or higher.

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
- `cmake`: Build system generator (requires version 3.12 or later)
- `ninja`: Fast build tool
- `qt@6`: Qt 6 libraries (project supports Qt 5.9+ or Qt 6, CI uses Qt 6.9.3)
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

#### Step 5: Create Build Directory

Create a build directory in the project root:

```bash
cd <path_to_klogg_repository_clone>
mkdir -p build_root
cd build_root
```

#### Step 6: Configure CMake

Choose the appropriate configuration command based on your Qt version:

**Using Qt 6 (recommended, matches CI):**

```bash
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKLOGG_OSX_DEPLOYMENT_TARGET=14.0 \
  -DKLOGG_GENERIC_CPU=ON \
  -DKLOGG_USE_SENTRY=OFF \
  -DQt6_DIR=$(brew --prefix qt@6)/lib/cmake/Qt6 \
  ..
```

**Using Qt 5:**

```bash
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKLOGG_OSX_DEPLOYMENT_TARGET=14.0 \
  -DKLOGG_GENERIC_CPU=ON \
  -DKLOGG_USE_SENTRY=OFF \
  -DQt5_DIR=$(brew --prefix qt5)/lib/cmake/Qt5 \
  ..
```

**Parameter explanations:**
- `-G Ninja`: Use Ninja as the build system (faster)
- `-DCMAKE_BUILD_TYPE=RelWithDebInfo`: Release build with debug information
- `-DKLOGG_OSX_DEPLOYMENT_TARGET=14.0`: Target macOS version (adjust as needed, e.g., 13.0, 14.0, 15.0)
- `-DKLOGG_GENERIC_CPU=ON`: Build for generic CPU (matches CI)
- `-DKLOGG_USE_SENTRY=OFF`: Disable Sentry crash reporting (recommended on macOS)
- `-DQt6_DIR` or `-DQt5_DIR`: Path to Qt CMake configuration files

**If CMake cannot find Qt, manually specify the path:**

```bash
# First find Qt path
QT_PATH=$(brew --prefix qt@6)
echo "Qt path: $QT_PATH"

# Then use full path
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKLOGG_OSX_DEPLOYMENT_TARGET=14.0 \
  -DKLOGG_GENERIC_CPU=ON \
  -DKLOGG_USE_SENTRY=OFF \
  -DQt6_DIR=$QT_PATH/lib/cmake/Qt6 \
  ..
```

#### Step 7: Build the Project

After successful configuration, start building:

```bash
cmake --build . -j$(sysctl -n hw.ncpu)
```

The `-j$(sysctl -n hw.ncpu)` flag uses all CPU cores on your Mac for parallel compilation, speeding up the process.

Alternatively, use Ninja directly:

```bash
ninja
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

If you need to create a macOS installer package (DMG), run:

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

By default, klogg will rely on cmake to figure out target MacOS version. Usually it uses build host version.
To override default cmake value pass an option `-DKLOGG_OSX_DEPLOYMENT_TARGET=<target>` to cmake during configuration step,
`<target>` is one of `10.14`, `10.15`, `11`, `12`, `13`, `14`, `15`. Klogg's target must be greater or equal to target used by Qt libraries.

## Running tests

Tests are built by default. To turn them off pass `-DBUILD_TESTS:BOOL=OFF` to cmake.
Tests use catch2 (bundled with klogg sources) and require Qt5Test module. Tests can be run using ctest tool provider by CMake:

```
cd <path_to_klogg_repository_clone>
cd build_root
ctest --build-config RelWithDebInfo --verbose
```
