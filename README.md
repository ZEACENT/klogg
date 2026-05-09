[![GitHub license](https://img.shields.io/github/license/ZEACENT/klogg.svg?style=flat)](https://github.com/ZEACENT/klogg/blob/master/COPYING)
[![C++](https://img.shields.io/github/languages/top/ZEACENT/klogg?style=flat)]()
[![GitHub contributors](https://img.shields.io/github/contributors/ZEACENT/klogg.svg?style=flat)](https://github.com/ZEACENT/klogg/graphs/contributors/)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg?style=flat)](http://makeapullrequest.com)
[![Github all releases](https://img.shields.io/github/downloads/ZEACENT/klogg/total?style=flat)](https://github.com/ZEACENT/klogg/releases/)
[![Github](https://img.shields.io/github/v/release/ZEACENT/klogg?style=flat&label=Stable%20release)](https://github.com/ZEACENT/klogg/releases/latest)
[![CI Build](https://github.com/ZEACENT/klogg/actions/workflows/ci-build.yml/badge.svg)](https://github.com/ZEACENT/klogg/actions/workflows/ci-build.yml)

## Overview

Klogg is a multi-platform GUI application that helps browse and search
through long and complex log files. It is designed with programmers and
system administrators in mind and can be seen as a graphical, interactive
combination of grep, less, and tail.

Please refer to the
[technical documentation](docs/TECHNICAL_DOCUMENTATION.md)
page for how to use Klogg.

A [changelog](CHANGELOG.md) tracks monthly changes.

## Table of Contents

1. [About the Project](#about-the-project)
1. [Installation](#installation)
1. [Building](#building)
1. [Third-Party Dependencies](#third-party-dependencies)
1. [How to Get Help](#how-to-get-help)
1. [Contributing](#contributing)
1. [License](#license)
1. [Authors](#authors)
1. [Backlog](#backlog)

## About the Project

Klogg started as a fork of [glogg](https://github.com/nickbnf/glogg) - the fast, smart log explorer in 2016.

Since then it has evolved from fixing small annoying bugs to rewriting core components to
make it faster and smarter than its predecessor.

### Comparing with glogg

Klogg has all the best features of glogg:

* Runs on Unix-like systems, Windows and Mac thanks to Qt5/Qt6
* Is fast and reads the file directly from disk, without loading it into memory
* Can operate on huge text files (10+ Gb is not a problem)
* Search results are displayed separately from original file
* Supports Perl-compatible regular expressions
* Colorizes the log and search results
* Displays a context view of where in the log the lines of interest are
* Watches for file changes on disk and reloads it (kind of like tail)
* Is open source, released under the GPL

And on top of that klogg:

* Is heavily optimized using multi-threading and SIMD
* Supports files with more than 2147483647 lines
* Includes optimized regular expressions search; benchmark details live in [docs/REGEX_BENCHMARKS.md](docs/REGEX_BENCHMARKS.md)
* Allows combining regular expressions with boolean operators (AND, OR, NOT)
* Supports many common text encodings
* Detects file encoding automatically using [uchardet](https://www.freedesktop.org/wiki/Software/uchardet/) library (supports utf8, utf16, cp1251 and more)
* Can limit search operations to some part of huge file
* Allows configuring several highlighter sets and switching between them
* Has a list of configurable predefined regular expression patterns
* Includes a dark mode
* Has configurable shortcuts
* Has a scratchpad window for taking notes and doing basic data transformations
* Provides lots of small features that make life easier (closing tabs, copying file paths, favorite files menu, etc.)

List of glogg issues that have been fixed/implemented in klogg can be found [here](https://github.com/ZEACENT/klogg/discussions/302).

List of all changes can be found [here](https://github.com/ZEACENT/klogg/milestone/8?closed=1).

### Comparing with variar/klogg

This fork builds on [variar/klogg](https://github.com/variar/klogg) and adds:

* **iOS live log streaming** — device discovery and log capture via pymobiledevice3
* **Android logcat live streaming** — ADB-based logcat with persistent-file save mode
* **ANSI color rendering** — configurable output modes (strip / render / plain) for live streams
* **Live-source search throttling** — smooth UI during streaming without blocking the event loop
* **Search generation IDs** — monotonic counter prevents stale search results after interrupting a search (see [PORTABILITY.md](docs/PORTABILITY.md))
* **Batched flush** — triple-threshold output flushing (64 KB / 100 lines / 1 s) for live log captures
* **Highlighter coexistence** — selection highlighting preserves keyword highlighter colors
* **Status bar pending indicator** — shows pending search lines as `Ln:X/Y (+N pending)`
* **Platform-aware tool resolution** — robust external-tool discovery across macOS launchd, Windows, and Linux (see [PORTABILITY.md](docs/PORTABILITY.md))

## Installation

This project uses [Calendar Versioning](https://calver.org/). For a list of available versions, see the [repository tag list](https://github.com/ZEACENT/klogg/tags).

Binaries for all platforms are available from [GitHub Releases](https://github.com/ZEACENT/klogg/releases/latest).

### Continuous builds

| Windows | Linux | Mac |
| ------------- |------------- | ------------- |
| [continuous-win](https://github.com/ZEACENT/klogg/releases/tag/continuous-win) | [continuous-linux](https://github.com/ZEACENT/klogg/releases/tag/continuous-linux) | [continuous-osx](https://github.com/ZEACENT/klogg/releases/tag/continuous-osx) |

## Building

Please review
[docs/BUILD.md](docs/BUILD.md)
for how to set up Klogg on your local machine for development and testing purposes.

### Regex Benchmark Snapshot

Regular-expression benchmark methodology now lives in [docs/REGEX_BENCHMARKS.md](docs/REGEX_BENCHMARKS.md). The current snapshot was refreshed on March 1, 2026 across `simple`, `normal`, and `complex` regex profiles with `50MB`, `500MB`, and `5GB` tmpfs-backed corpora, comparing `Qt`, `Vectorscan generic`, and `Vectorscan AVX` builds.

500MB median search time snapshot:

| Profile | Qt (ms) | Vectorscan generic (ms) | Vectorscan AVX (ms) |
| --- | ---: | ---: | ---: |
| `simple` | 523.41 | 169.98 | 169.92 |
| `normal` | 690.74 | 186.47 | 180.23 |
| `complex` | 1071.93 | 219.07 | 204.00 |

The full matrix, throughput tables, regex contents, and fairness counters (`searched lines`, `matches`, `hit rate`) live in [docs/benchmarks/regex-benchmark-results.md](docs/benchmarks/regex-benchmark-results.md) and [docs/benchmarks/regex-benchmark-results.json](docs/benchmarks/regex-benchmark-results.json).

## Third-Party Dependencies

All C++ dependencies are managed via [CPM](https://github.com/cpm-cmake/CPM.cmake) unless noted otherwise.

| Dependency | Version / Commit | Source | Purpose |
|---|---|---|---|
| [Qt](https://www.qt.io/) | 5 or 6 | System | GUI framework (Core, Widgets, Concurrent, Network, Xml, Svg) |
| [Vectorscan](https://github.com/VectorCamp/vectorscan) | `d29730e` | `VectorCamp/vectorscan` | Regex acceleration (default engine) |
| [Boost](https://www.boost.org/) | - | System | Required by Vectorscan |
| [simdutf](https://github.com/simdutf/simdutf) | 5.6.2 | `simdutf/simdutf` | SIMD UTF-8 processing |
| [CRoaring](https://github.com/RoaringBitmap/CRoaring) | 4.2.1 | `RoaringBitmap/CRoaring` | Compressed bitmaps |
| [streamvbyte](https://github.com/lemire/streamvbyte) | 1.0.0 | `lemire/streamvbyte` | Variable-byte integer encoding |
| [robin_hood](https://github.com/martinus/robin-hood-hashing) | 3.11.2 | `martinus/robin-hood-hashing` | Fast hash maps/sets |
| [xxHash](https://github.com/Cyan4973/xxHash) | 0.8.1 | `Cyan4973/xxHash` | Fast hashing |
| [type_safe](https://github.com/foonathan/type_safe) | 0.2.4 | `foonathan/type_safe` | Type-safe utilities |
| [oneTBB](https://github.com/variar/oneTBB) | `c9be1ac` | `variar/oneTBB` | Threading / parallelism |
| [mimalloc](https://github.com/microsoft/mimalloc) | 2.1.7 | `microsoft/mimalloc` | Memory allocator |
| [efsw](https://github.com/SpartanJ/efsw) | 1.4.1 | `SpartanJ/efsw` | File system watcher |
| [Uchardet](https://gitlab.freedesktop.org/uchardet/uchardet) | 0.0.8 | `uchardet/uchardet` (GitLab) | Character encoding detection |
| [maddy](https://github.com/variar/maddy) | `602e266` | `variar/maddy` | Markdown to HTML conversion |
| [exprtk](https://github.com/variar/klogg_exprtk) | `1f9f4cd` | `variar/klogg_exprtk` | Expression parsing |
| [KF5Archive](https://github.com/variar/klogg_karchive) | `f546bf6` | `variar/klogg_karchive` | Archive format support (tar, zip, bzip2, lzma) |
| [KDSingleApplication](https://github.com/variar/KDSingleApplication) | `5b30db3` | `variar/KDSingleApplication` | Single-instance enforcement |
| [KDToolBox](https://github.com/KDAB/KDToolBox) | `6468867` | `KDAB/KDToolBox` | Signal throttler |
| [whereami](https://github.com/gpakosz/whereami) | `dcb52a0` | `gpakosz/whereami` | Executable path detection |
| [Sentry Native SDK](https://github.com/getsentry/sentry-native) | `a3d5862` | `getsentry/sentry-native` | Crash reporting (optional) |
| [macdeployqtfix](https://github.com/arl/macdeployqtfix) | `df88850` | `arl/macdeployqtfix` | macOS Qt deployment (macOS only) |
| [Catch2](https://github.com/catchorg/Catch2) | 2.13.8 | `catchorg/Catch2` | Unit testing framework |
| [backward-cpp](https://github.com/bombela/backward-cpp) | 1.6 | `bombela/backward-cpp` | Stack trace capture (testing) |

## How to Get Help

Please refer to the
[technical documentation](docs/TECHNICAL_DOCUMENTATION.md)
page.

You can open issues on the [klogg issues page](https://github.com/ZEACENT/klogg/issues).

## Contributing

Contributions are welcome! Please review [CONTRIBUTING.md](CONTRIBUTING.md) for details on the code of conduct and development process.

## License

This project is licensed under the GPLv3 or later — see [COPYING](COPYING) file for details.

## Authors

* **[Anton Filimonov](https://github.com/variar)**
* *Initial work* — **[Nicolas Bonnefon](https://github.com/nickbnf)**

See also the list of [contributors](https://github.com/ZEACENT/klogg/graphs/contributors) who participated in this project.

## Backlog

See [docs/BACKLOG.md](docs/BACKLOG.md) for the task backlog and planned features.
