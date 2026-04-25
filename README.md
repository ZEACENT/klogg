![media_small](https://user-images.githubusercontent.com/1620716/119145300-2d98b800-ba52-11eb-8d87-abe72cf65dd1.png)

[![GitHub license](https://img.shields.io/github/license/ZEACENT/klogg.svg?style=flat)](https://github.com/ZEACENT/klogg/blob/master/COPYING)
[![C++](https://img.shields.io/github/languages/top/ZEACENT/klogg?style=flat)]()
[![GitHub contributors](https://img.shields.io/github/contributors/ZEACENT/klogg.svg?style=flat)](https://github.com/ZEACENT/klogg/graphs/contributors/)
[![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg?style=flat)](http://makeapullrequest.com)
[![Codacy Badge](https://api.codacy.com/project/badge/Grade/f6db6ef0be3a4a5abff94111a5291c45)](https://www.codacy.com/manual/ZEACENT/klogg?utm_source=github.com&amp;utm_medium=referral&amp;utm_content=ZEACENT/klogg&amp;utm_campaign=Badge_Grade)


[![Github all releases](https://img.shields.io/github/downloads/ZEACENT/klogg/total?style=flat)](https://github.com/ZEACENT/klogg/releases/)
[ ![Github](https://img.shields.io/github/v/release/ZEACENT/klogg?style=flat&label=Stable%20release&)](https://github.com/ZEACENT/klogg/releases/latest)

[![Packaging status](https://repology.org/badge/vertical-allrepos/klogg.svg)](https://repology.org/project/klogg/versions)

Check [GitHub releases](https://github.com/ZEACENT/klogg/releases/latest) for Windows installers and Linux/Mac packages.

Development status

[![Next milestone](https://img.shields.io/github/milestones/progress-percent/ZEACENT/klogg/4?style=flat&)](https://github.com/ZEACENT/klogg/milestone/4)
[![Ready for testing](https://img.shields.io/github/issues-raw/ZEACENT/klogg/status:%20ready%20for%20testing?color=green&label=issues%20ready%20for%20testing&style=flat)](https://github.com/ZEACENT/klogg/issues?q=is%3Aopen+is%3Aissue+label%3A%22status%3A+ready+for+testing%22)
[![Need documentation](https://img.shields.io/github/issues-search/ZEACENT/klogg?color=yellow&label=features%20need%20documentation&query=is%3Aissue%20label%3A%22status%3A%20need%20documentation%22&style=flat)](https://github.com/ZEACENT/klogg/issues?q=is%3Aissue+label%3A%22status%3A+need+documentation%22)
[![GitHub commits](https://img.shields.io/github/commits-since/ZEACENT/klogg/v22.06.svg?style=flat)](https://github.com/ZEACENT/klogg/commits/)
[![CI Build and Release](https://github.com/ZEACENT/klogg/actions/workflows/ci-build.yml/badge.svg)](https://github.com/ZEACENT/klogg/actions/workflows/ci-build.yml)

[![Chat on Discord](https://img.shields.io/discord/838452586944266260?label=Discord&style=flat)](https://discord.gg/DruNyQftzB) [![Join the chat at https://gitter.im/klogg_log_viewer/community](https://badges.gitter.im/klogg_log_viewer/community.svg)](https://gitter.im/klogg_log_viewer/community?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)

## Overview

Klogg is a multi-platform GUI application that helps browse and search
through long and complex log files. It is designed with programmers and
system administrators in mind and can be seen as a graphical, interactive
combination of grep, less, and tail.

![Klogg main window](website/static/screenshots/mainwindow.png)

Please refer to the
[technical documentation](docs/TECHNICAL_DOCUMENTATION.md)
page for how to use Klogg.

### Latest testing builds

| Windows | Linux | Mac |
| ------------- |------------- | ------------- |
| [continuous-win](https://github.com/ZEACENT/klogg/releases/tag/continuous-win) | [continuous-linux](https://github.com/ZEACENT/klogg/releases/tag/continuous-linux) | [continuous-osx](https://github.com/ZEACENT/klogg/releases/tag/continuous-osx) |

I try to keep a [changelog](CHANGELOG.md) with monthly changes. 

## Table of Contents

1. [About the Project](#about-the-project)
1. [Installation](#installation)
1. [Building](#building)
1. [Third-Party Dependencies](#third-party-dependencies)
1. [How to Get Help](#how-to-get-help)
1. [Contributing](#contributing)
1. [License](#license)
1. [Authors](#authors)

## About the Project

Klogg started as a fork of [glogg](https://github.com/nickbnf/glogg) - the fast, smart log explorer in 2016.

Since then it has evolved from fixing small annoying bugs to rewriting core components to
make it faster and smarter that predecessor.

Development of klogg is driven by features my colleagues and I need
to stay productive as well as feature requests from users on Github and in glogg mailing list.

Latest news about klogg development can be found at https://klogg.filimonov.dev.

### Comparing with glogg

Klogg has all best features of glogg:

* Runs on Unix-like systems, Windows and Mac thanks to Qt5
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
* Allows to configure several highlighters sets and switch between them
* Has a list of configurable predefined regular expression patterns
* Includes a dark mode
* Has configurable shortcuts
* Has a scratchpad window for taking notes and doing basic data transformations
* Provides lots of small features that make life easier (closing tabs, copying file paths, favorite files menu, etc.)

List of glogg issues that have been fixed/implemented in klogg can be found [here](https://github.com/ZEACENT/klogg/discussions/302).

List of all changes can be found [here](https://github.com/ZEACENT/klogg/milestone/8?closed=1).

**[Back to top](#table-of-contents)**

## Installation

This project uses [Calendar Versioning](https://calver.org/). For a list of available versions, see the [repository tag list](https://github.com/ZEACENT/klogg/tags).

### Current stable release builds

Binaries for all platforms can be downloaded from GitHub releases.

[ ![Release](https://img.shields.io/github/v/release/ZEACENT/klogg?style=flat)](https://github.com/ZEACENT/klogg/releases/latest)

#### Windows
Windows installer is also available from:

* [ ![Chocolatey](https://img.shields.io/chocolatey/v/klogg?style=flat)](https://chocolatey.org/packages/klogg)
* [ ![Scoop Extras bucket](https://img.shields.io/scoop/v/klogg?bucket=extras)](https://scoopsearch.github.io/#/apps?q=klogg)
* [Winget package](https://winget.run/pkg/ZEACENT/klogg) 

#### Mac OS
Package for Mac can be installed from Homebrew

[ ![homebrew cask](https://img.shields.io/homebrew/cask/v/klogg?style=flat)](https://formulae.brew.sh/cask/klogg)

#### Linux
It is recommended to use klogg package from distribution-specific [repositories](https://repology.org/project/klogg/versions).

Generic packages are available from the klogg DEB repository hosted at GitHub Pages.
They are built to run on Ubuntu 22.04/24.04 (x86-64 only).

For DEB packages first download the gpg key:
```
curl -sS https://klogg.filimonov.dev/klogg.gpg.key | gpg --dearmor | sudo tee /etc/apt/keyrings/klogg.gpg
```

You might need to manually create `/etc/apt/keyrings` directory.

Then download the repository list file for your distribution (replace `<ubuntu_release>` with `jammy` or `noble`):
```
curl -sS https://klogg.filimonov.dev/deb/klogg.<ubuntu_release>.list | sudo tee /etc/apt/sources.list.d/klogg.list
```

Finally, install using apt
```
sudo apt-get update
sudo apt install klogg
```

If there is already an entry for JFrogg hosted klogg repository in `/etc/apt/sources.list`, then remove this line from it:
```
deb [trusted=yes] https://favpackage.jfrog.io/artifactory/klogg_deb/ <ubuntu_release> utils
```

There is also an AppImage package that can be used without installation. To run klogg from AppImage, download the package and make in executable with either a file manager or terminal command `chmod +x <path_to_klogg_AppImage>` and then run the AppImage file.

AppImage uses FUSE2 and Ubuntu 22.04 has moved away from FUSE2 into FUSE3 and therefore you need to install the necessary package to enable compatibility with FUSE2 `sudo apt install libfuse2`.

As indicated by this link from the official appimage documentation: https://docs.appimage.org/user-guide/troubleshooting/fuse.html#setting-up-fuse-2-x-alongside-of-fuse-3-x-on-recent-ubuntu-22-04-debian-and-their-derivatives

### Testing builds

![CI Build and Release](https://github.com/ZEACENT/klogg/workflows/CI%20Build%20and%20Release/badge.svg)

| Windows | Linux | Mac |
| ------------- |------------- | ------------- |
| [continuous-win](https://github.com/ZEACENT/klogg/releases/tag/continuous-win) | [continuous-linux](https://github.com/ZEACENT/klogg/releases/tag/continuous-linux) | [continuous-osx](https://github.com/ZEACENT/klogg/releases/tag/continuous-osx) |

**[Back to top](#table-of-contents)**

## Building

Please review
[docs/BUILD.md](docs/BUILD.md)
for how to setup Klogg on your local machine for development and testing purposes.

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

First, please refer to the
[technical documentation](docs/TECHNICAL_DOCUMENTATION.md)
page.

You can open issues using [klogg issues page](https://github.com/ZEACENT/klogg/issues)
or post questions to glogg development [mailing list](http://groups.google.co.uk/group/glogg-devel).

## Contributing

We encourage public contributions! Please review [CONTRIBUTING.md](CONTRIBUTING.md) for details on our code of conduct and development process.

## License

This project is licensed under the GPLv3 or later - see [COPYING](COPYING) file for details.

## Authors

* **[Anton Filimonov](https://github.com/variar)**
* *Initial work* - **[Nicolas Bonnefon](https://github.com/nickbnf)**

See also the list of [contributors](https://klogg.filimonov.dev/docs/getting_involved/#contributors) who participated in this project.

## Backlog

| ID | Task | Priority | Status | Related |
|----|------|----------|--------|---------|
| TASK-001 | [Search generation ID refactoring](#task-001-search-generation-id-refactoring) | Low | Done | [PR #11](https://github.com/ZEACENT/klogg/pull/11) |
| TASK-002 | [Chart Panel — regex match → time series](#task-002-chart-panel) | Medium | Planned | [SPEC](docs/SPEC_CHART_AND_FILTERS_PANEL.md) |
| TASK-003 | [Filters Panel — persistent dock with groups & pinning](#task-003-filters-panel) | Medium | Planned | [SPEC](docs/SPEC_CHART_AND_FILTERS_PANEL.md) |

### TASK-001: Search generation ID refactoring

**Scenario:**
`CrawlerWidget::replaceCurrentSearch()` needs to discard stale `searchProgressed`
signals from an interrupted search before starting a new one. The current approach
temporarily disconnects and reconnects the
`LogFilteredData::searchProgressed` / `CrawlerWidget::updateFilteredView` slot.

**Problem:**
With `Qt::QueuedConnection`, `disconnect()` does not remove already-posted
`QMetaCallEvent`s from the receiver's event queue — they will still be delivered
after reconnect. Currently the window between disconnect and reconnect is very
small (a few synchronous calls in `replaceCurrentSearch()`), so the practical
impact is negligible. However, the pattern is fragile and could become a real bug
if the window widens in future refactors.

**Code context:**
- Signal: `LogFilteredData::searchProgressed(LinesCount, int, LineNumber)` — `src/logdata/include/logfiltereddata.h:149`
- Emit sites (6+): `src/logdata/src/logfiltereddata.cpp` (lines 164, 634, 646, 666), `src/logdata/src/logfiltereddataworker.cpp` (lines 319, 456, 485, 623, 708)
- Disconnect/reconnect: `src/ui/src/crawlerwidget.cpp` (lines 1830–1831, 1904–1905)
- Slot: `CrawlerWidget::updateFilteredView()` — `src/ui/src/crawlerwidget.cpp:630`

**Proposed fix:**
1. Add a monotonic `uint64_t` generation counter to `LogFilteredData`, incremented by `runSearch()` / `updateSearch()`
2. Extend `searchProgressed` signal to carry the generation ID
3. In `CrawlerWidget::updateFilteredView()`, ignore signals where `generation != activeSearchGeneration_`
4. Remove the disconnect/reconnect calls in `replaceCurrentSearch()`

**Trade-offs:**
Cross-cutting change touching signal signature, all emit sites, and all connected slots.
Should be done in a dedicated PR with thorough regression testing.

**Resolution:**
Implemented in branch `docs/backlog-generation-id`.  The wire type for the
generation argument is plain `quint64` rather than the
`LogFilteredDataWorker::OperationGeneration` typedef, because moc treats
typedefs of non-builtin types as unregistered metatypes and `QSignalSpy`
decodes the `QVariant` back to 0; the typedef alias is kept for
code-readability but does not appear in any `Q_SIGNAL` signature.  Two
new SCENARIOs in `tests/ui/logfiltereddata_test.cpp` cover generation
increment and signal payload.  Receiver-side staleness gate is factored
into `klogg::isStaleSearchGeneration` (`src/logdata/include/searchgeneration.h`)
with its own unit test.  Cache-hit path bumps the generation via
`LogFilteredDataWorker::bumpGeneration()` so prior-search progress
signals are correctly dropped.

### TASK-002: Chart Panel

**Scenario:**
Render a time-series chart of regex-matched events inside klogg so that
post-mortem analysis goes beyond "search & scroll" into "see when things
happen and how often".

**Spec:** [`docs/SPEC_CHART_AND_FILTERS_PANEL.md`](docs/SPEC_CHART_AND_FILTERS_PANEL.md)
covers goals / non-goals, UX, data model, three phased iterations
(Phase 1: filter-frequency on a line-number axis; Phase 2: capture-group
numeric aggregation; Phase 3: optional timestamp axis), files affected,
testing strategy, and effort estimate (~5 weeks for Phase 1 + 2).

**Recommended order:** ship after TASK-003.

### TASK-003: Filters Panel

**Scenario:**
Surface the existing Predefined Filters feature as a persistent
left-side dock with grouping and pinning, so users can toggle filter
sets in one click instead of opening a multi-step dialog.

**Spec:** [`docs/SPEC_CHART_AND_FILTERS_PANEL.md`](docs/SPEC_CHART_AND_FILTERS_PANEL.md)
covers goals / non-goals, UX (tree view + drag-drop + per-group pin),
data-model migration (legacy flat list → grouped collection), three
phased iterations, testing strategy, and effort estimate (~1.5 weeks
to feature-complete).

**Recommended order:** ship before TASK-002.

**[Back to top](#table-of-contents)**
