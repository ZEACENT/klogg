<p align="center">
  <img src="src/app/images/hicolor/scalable/klogg.svg" width="80" height="80" alt="klogg logo">
</p>

<h1 align="center">klogg</h1>

<p align="center"><strong>Find the signal. Keep the context.</strong></p>

<p align="center">
  A desktop workspace for understanding logs.<br>
  Explore large files, search across folders, and follow live device logs without losing the bigger picture.
</p>

<p align="center">
  <a href="https://github.com/ZEACENT/klogg/releases/latest"><strong>Download</strong></a> &nbsp;·&nbsp;
  <a href="#start-exploring">Quick start</a> &nbsp;·&nbsp;
  <a href="docs/README.md">Documentation</a> &nbsp;·&nbsp;
  <a href="docs/BUILD.md">Build from source</a>
</p>

<p align="center">
  <a href="https://github.com/ZEACENT/klogg/releases/latest"><img src="https://img.shields.io/github/v/release/ZEACENT/klogg?style=flat&label=release&color=008765" alt="Latest release"></a>
  <a href="https://github.com/ZEACENT/klogg/actions/workflows/ci-build.yml"><img src="https://github.com/ZEACENT/klogg/actions/workflows/ci-build.yml/badge.svg" alt="CI build status"></a>
  <a href="COPYING"><img src="https://img.shields.io/badge/license-GPL--3.0--or--later-555555?style=flat" alt="GPL version 3 or later"></a>
</p>

---

## From the first match to the full story

A timeout is easy to find. Understanding what led to it takes context.
klogg keeps your original log and filtered results together, so you can move
between a useful match and the surrounding events instead of juggling editor
windows and terminal output.

![klogg showing the original file above its filtered results, with matching text highlighted in both views](website/static/screenshots/mainwindow.png)

*Interface overview from an earlier upstream release. Current controls and the
folder/device workflows are described in the [user guide](DOCUMENTATION.md).*

### One large file. A focused investigation.

Open a log too large for a comfortable editor session. Narrow it down with plain
text, regular expressions, or Boolean combinations, then jump from each result
back into the original file. Add lines before or after matches when an isolated
message is not enough.

- Read file content on demand rather than loading the whole text into an editor buffer.
- Limit a single-file search to a relevant range of lines.
- Follow a growing file and enable **Auto-refresh** to keep search results up to date.
- Work with common text encodings, automatic detection, and manual overrides.

### A whole folder. One place to look.

When the trail crosses several services or rotated logs, use **Open Folder**.
klogg searches recursively and groups results by file. Select a match to inspect
its source in the upper view; collapse groups to concentrate on the files that
matter.

Use the same search modes, reusable filters, highlights, and line marks across
the investigation. **Keep Results** preserves a result pane while you try the
next query. Folder searches are snapshots: run the search again to include
changed files; they do not continuously watch the directory.

### Live devices. The same familiar workspace.

Bring Android logcat directly into klogg with the bundled ADB helper. On macOS,
connect to an iOS device through the bundled native capture stack, without a
Python setup. Search and highlight incoming logs as you would a file.

Control reconnect behavior, choose rolling-output limits, and explicitly save
the capture with or without ANSI sequences. Restored saved sessions start
stopped, so reconnecting a device remains your choice.

[Learn about live capture and saving](DOCUMENTATION.md#live-device-logs)

## Make the important lines stand out

| When you need to... | klogg helps you... |
| --- | --- |
| Follow a request through noisy output | Combine patterns with AND, OR, and NOT; keep matching lines beside their context. |
| Recognize recurring signals | Apply multiple highlighter sets and quick color labels without losing selection visibility. |
| Collect evidence | Mark lines, extend a selection with Shift-click, or select separate lines with Ctrl-click (Command-click on macOS); copy with line numbers or save the selection. |
| Compare investigations | Reuse filter favorites and search history, or keep results in another tab. |
| Read comfortably | Choose dark mode, wrap long lines, adjust fonts, and customize shortcuts. |
| Take a working note | Send text to the scratchpad for notes and basic transformations. |

## Download

Choose a package from **[GitHub Releases](https://github.com/ZEACENT/klogg/releases/latest)**.
Use the **[Continuous release](https://github.com/ZEACENT/klogg/releases/tag/continuous)**
for the latest rolling build. Features described here follow the current source;
check your release notes if a control is missing in an older package. The table
below describes the current Continuous packages; an older Stable release may
offer a different set of files.

| Platform | Current Continuous packages | Before you install |
| --- | --- | --- |
| Windows | x64 installer or portable ZIP | Current packages use Qt 6 and Vectorscan AVX2; choose hardware that supports AVX2. |
| Linux | Ubuntu 22.04, 24.04, and 26.04 DEB; AppImage | Choose the DEB for your Ubuntu version, or the AppImage for a compatible distribution. |
| macOS | Separate Apple Silicon and Intel DMGs | Choose your processor architecture and check the release's minimum macOS version. |

**macOS packages are unsigned CI validation artifacts, not signed or notarized
releases.** Gatekeeper may require explicit local approval. Review the source
and release before approving an application; do not disable Gatekeeper globally.

For Windows, run the installer or extract the portable ZIP before launching.
On Ubuntu, install the matching downloaded package with `sudo apt install ./<package>.deb`.
For an AppImage, make the downloaded file executable with `chmod +x <package>.AppImage`
and run it. On macOS, open the matching DMG and copy the application to Applications.

Stable releases promote a manifest-verified Continuous release and reuse its
package payloads; promotion does not add macOS signing or notarization. Release
pages include checksums and source/support assets. klogg uses
[calendar versioning](https://calver.org/).

Third-party package-manager listings may track upstream klogg rather than this
fork. Use this repository's releases for the capabilities documented here.

## Start exploring

1. **Choose a source.** Open a file, choose **File > Open Folder...**, or connect
   through **File > Open ADB Logcat...** / **Open iOS Log Stream...** (macOS).
2. **Ask a focused question.** Enter `timeout` as plain text, or enable regex and
   try `ERROR|WARN`. Search results appear below the source view.
3. **Follow the evidence.** Select a result to see it in context. Highlight useful
   patterns, mark lines worth revisiting, and keep or export the results you need.

The **[user guide](DOCUMENTATION.md)** covers search syntax, device prerequisites,
keyboard commands, and saving. It is also available from the application's Help
menu. The **[documentation hub](docs/README.md)** separates everyday workflows
from architecture, build instructions, benchmarks, and future plans.

## Built for demanding logs

The interface is backed by C++17 and Qt, 64-bit line addressing, parallel search,
SIMD-assisted text processing, and compressed match storage. Vectorscan
accelerates compatible regular expressions; Qt's regular-expression engine
provides the fallback/verification path for patterns that need it. Indexes,
caches, results, and live capture still consume memory: usage depends on the
workload, not just the source file's byte size.

### Performance, with the conditions attached

The recorded **May 17, 2026** benchmark compares search backends on generated
corpora using macOS x86_64 and Qt 6.10.1, with five measured iterations after one
warmup. Below are **500 MiB full-search median times** (the benchmark's `500MB`
case), not end-to-end application startup times or a promise for every machine:

| Regex profile | Qt | Vectorscan generic | Vectorscan AVX |
| --- | ---: | ---: | ---: |
| Simple | 573.93 ms | 169.90 ms | 165.90 ms |
| Normal | 606.89 ms | 208.23 ms | 215.07 ms |
| Complex | 1046.55 ms | 235.67 ms | 244.48 ms |

Hardware, storage, encoding, expressions, and the selected backend affect results.
See the [methodology](docs/REGEX_BENCHMARKS.md),
[full results](docs/benchmarks/regex-benchmark-results.md), and
[machine-readable evidence](docs/benchmarks/regex-benchmark-results.json), including
incremental and ANSI-streaming measurements. These are recorded snapshots, not
new measurements of every release.

For implementation details, read the [architecture overview](docs/TECHNICAL_DOCUMENTATION.md),
[portability guidance](docs/PORTABILITY.md), and [dependency reference](docs/DEPENDENCIES.md).

## Build, contribute, and get help

**GitHub is this project's only communication channel.**

- **Build locally:** follow the [build guide](docs/BUILD.md) for platform setup,
  configuration, tests, and local performance gates.
- **Report a problem or request a feature:** use [GitHub Issues](https://github.com/ZEACENT/klogg/issues),
  including your version, platform, and a minimal example without private log data.
- **Contribute:** start with [CONTRIBUTING.md](CONTRIBUTING.md) and the
  [Code of Conduct](CODE_OF_CONDUCT.md). Documentation and usability improvements
  are welcome alongside code.
- **Security:** read the [security policy](SECURITY.md) before sharing sensitive details.
- **Follow development:** see [releases](https://github.com/ZEACENT/klogg/releases),
  the [historical changelog](CHANGELOG.md), and the [backlog](docs/BACKLOG.md).
  Backlog entries are plans, not shipped-feature promises.

## Open source, with a long history

This is the **ZEACENT fork of [variar/klogg](https://github.com/variar/klogg)**,
continuing a project that began as a fork of
[Nicolas Bonnefon's glogg](https://github.com/nickbnf/glogg) in 2016.
It builds on the work of [Anton Filimonov](https://github.com/variar),
[Nicolas Bonnefon](https://github.com/nickbnf), and the
[contributors](https://github.com/ZEACENT/klogg/graphs/contributors).

klogg is free and open source under **GPLv3 or later**.
See [COPYING](COPYING) for the license and [NOTICE](NOTICE) for third-party notices.
