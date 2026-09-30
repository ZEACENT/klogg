---
title: "klogg"
type: docs
---

## Find the signal. Keep the context.

**A desktop workspace for understanding logs.** Explore large files, search
across folders, and follow live device logs without losing the bigger picture.

[Download](https://github.com/ZEACENT/klogg/releases/latest) ·
[Quick start](https://github.com/ZEACENT/klogg#start-exploring) ·
[User guide](https://github.com/ZEACENT/klogg/blob/master/DOCUMENTATION.md) ·
[Documentation](https://github.com/ZEACENT/klogg/blob/master/docs/README.md)

A timeout is easy to find. Understanding what led to it takes context. klogg
keeps the original log and filtered results together, so you can move from a
useful match to the surrounding events in the same workspace.

![The original log and filtered results, with matching text highlighted in both views](/screenshots/mainwindow.png)

*Interface overview from an earlier upstream release. The user guide describes
current controls, folder search, and device capture.*

## Three ways to investigate

### Explore a large file

Search with plain text, regular expressions, or Boolean combinations. Limit a
single-file search to a useful range, add context around results, and follow
appended lines with Auto-refresh. klogg reads file content on demand and uses
parallel search and Vectorscan acceleration where supported, with Qt regex
fallback/verification for patterns that need it.

### Search a whole folder

Use **Open Folder** to search recursively across readable text files. Results
are grouped by file; selecting a match opens its source in the upper view.
Reuse filters, keep results, and mark lines worth revisiting. Folder search is
a snapshot, not a continuously watched directory: rerun it after files change.

### Follow live device logs

Use the bundled ADB helper for Android logcat. On macOS, use the bundled native
iOS stack without a Python setup. Search incoming logs, control reconnect
behavior, and explicitly save with or without ANSI sequences. Saved sessions
restore stopped until you choose to reconnect.

## Keep your investigation readable

Multiple highlighter sets, quick color labels, line marks, disjoint selections,
search history, and filter favorites help separate useful evidence from noise.
Dark mode, text wrapping, configurable shortcuts, and the scratchpad let you
adapt the workspace to your way of reading.

## Download this fork

This repository documents the **ZEACENT fork of klogg**, built on
[variar/klogg](https://github.com/variar/klogg) and
[glogg](https://github.com/nickbnf/glogg). Third-party package-manager listings
may follow the upstream project instead.

- **[Stable releases](https://github.com/ZEACENT/klogg/releases/latest):** Windows
  x64 installer/portable ZIP, Linux DEB/AppImage, and separate Intel/Apple Silicon DMGs.
- **[Continuous release](https://github.com/ZEACENT/klogg/releases/tag/continuous):**
  the latest rolling build, published through one shared release channel.
- **[Installation notes](https://github.com/ZEACENT/klogg#download):** architecture,
  CPU, operating-system, and package requirements. Check the selected release's notes.

**macOS DMGs are unsigned CI validation artifacts, not signed or notarized
releases.** They may require explicit local Gatekeeper approval. Stable
promotion reuses verified Continuous package payloads; it does not add signing.

## Go deeper

- [User guide](https://github.com/ZEACENT/klogg/blob/master/DOCUMENTATION.md):
  workflows, device prerequisites, keyboard commands, and saving.
- [Performance methodology and evidence](https://github.com/ZEACENT/klogg/blob/master/docs/REGEX_BENCHMARKS.md):
  recorded measurements with their conditions, not universal speed claims.
- [Build from source](https://github.com/ZEACENT/klogg/blob/master/docs/BUILD.md)
  and [contribute](https://github.com/ZEACENT/klogg/blob/master/CONTRIBUTING.md).

klogg is free and open source under GPLv3 or later. See the
[license](https://github.com/ZEACENT/klogg/blob/master/COPYING) and
[third-party notices](https://github.com/ZEACENT/klogg/blob/master/NOTICE).
