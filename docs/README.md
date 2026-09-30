# Documentation

[Back to klogg](../README.md) · [Download](https://github.com/ZEACENT/klogg/releases/latest)

Start with the user guide if you want to investigate logs. Build and architecture
references are for contributors; proposals and historical investigations are
listed separately so they are not mistaken for current product behavior.

## Use klogg

| Guide | What you will find |
| --- | --- |
| [Quick start](../README.md#start-exploring) | Choose a source, search, and follow results back into context. |
| [Download and installation](../README.md#download) | Release channels, platform packages, and the unsigned macOS package caveat. |
| [User guide](../DOCUMENTATION.md) | Files, folder search, filters, highlights, line marks, selections, encodings, settings, and shortcuts. Also bundled in the application's Help menu. |
| [Live device logs](../DOCUMENTATION.md#live-device-logs) | Android and macOS iOS prerequisites, capture controls, saving, rolling output, and restoring stopped sessions. |

The README and user guide describe the current source. An older installed
release may not expose every control. Refer to its
[release notes](https://github.com/ZEACENT/klogg/releases) when comparing behavior.

## Build and contribute

| Reference | Purpose |
| --- | --- |
| [Build guide](BUILD.md) | Platform setup, CMake configuration, tests, sanitizers, and opt-in local performance gates. |
| [Contributing](../CONTRIBUTING.md) | Bug reports, proposed changes, review expectations, and documentation maintenance. |
| [Portability and engineering guidance](PORTABILITY.md) | Cross-platform contracts, concurrency, Qt differences, and deterministic testing. |
| [Dependency reference](DEPENDENCIES.md) | Main dependency roles and source pins, with links to authoritative manifests and notices. |
| [Qualified CI environments](CI_ENVIRONMENTS.md) | Producer bootstrap status, immutable inputs, qualification, publication, verification, and rollback. |
| [Source-built ADB helper](../packaging/adb/README.md) | Packaging-specific source closure, toolchains, verification, and legal assets. |

## Understand the implementation

These references explain the implementation, not an additional set of product
promises. Source files and regression tests are authoritative when details change.

| Reference | Scope |
| --- | --- |
| [Architecture overview](TECHNICAL_DOCUMENTATION.md) | UI, data sources, search, storage, platform boundaries, and links to deeper references. |
| [Incremental search](INCREMENTAL_SEARCH_ARCHITECTURE.md) | Appended-data searches, scheduling, cancellation, and presentation. |
| [ADB and live-capture architecture](ADB_LOGCAT_ARCHITECTURE.md) | Managed device transport, temporary capture storage, live saving, and restore boundaries. |

## Performance: methodology and recorded evidence

A benchmark snapshot describes its recorded revision, machine, inputs, and
configuration. It is not a universal performance guarantee or an automatically
refreshed measurement of the latest release.

| Document | Status and use |
| --- | --- |
| [Regex benchmark methodology](REGEX_BENCHMARKS.md) | How to reproduce and interpret full, incremental, and streaming search measurements. |
| [Regex results](benchmarks/regex-benchmark-results.md) / [JSON](benchmarks/regex-benchmark-results.json) | Recorded measurements, backend comparisons, and fairness counters. |
| [Live-capture methodology](benchmarks/live-capture-methodology.md) | Scenario definitions, instrumentation, and interpretation rules. |
| [Live-capture results](benchmarks/live-capture-results.md) | Recorded evidence and its qualifications; read the methodology alongside it. |
| [Live-capture dependency inventory](benchmarks/live-capture-dependency-inventory.md) | Dependency evidence associated with the capture benchmark work, not the current application's complete SBOM. |

For local speed budgets, use the [build guide](BUILD.md#performance-budgets-local-only-gates).
Correctness and liveness assertions still run in CI; wall-clock performance
budgets are opt-in local checks.

## Plans and historical material

| Document | Status |
| --- | --- |
| [Backlog](BACKLOG.md) | Planning record; an entry is not a delivery commitment. |
| [Chart and filters panel specification](SPEC_CHART_AND_FILTERS_PANEL.md) | Proposal. The proposed dock panels are not shipped features. |
| [Text-wrapping analysis](WRAP_TEXT_ANALYSIS.md) | Historical implementation investigation; not a current testing recipe. |
| [Test-pollution audit](audit_pollution_report.md) | Point-in-time audit, not a statement about the current source tree. |
| [Changelog](../CHANGELOG.md) | Historical change record. Current release information lives on GitHub Releases. |
| [Website news](../website/content/docs/news/) and [release archive](../website/content/archive/) | Historical upstream articles and releases; their platform claims apply to those versions. |

## Project policies and attribution

- [Code of Conduct](../CODE_OF_CONDUCT.md)
- [Security policy](../SECURITY.md), including safe reporting through GitHub
- [GPL license](../COPYING) and [third-party notices](../NOTICE)
- [Website privacy notice](../website/content/docs/privacy_policy/_index.md)
- [Website legal notice](../website/content/docs/legal_notice/_index.md)

## Maintaining these documents

Keep one canonical home for each kind of information:

- **Product positioning and downloads:** root README; the website summarizes and links back.
- **User instructions:** root `DOCUMENTATION.md`. It is converted by maddy,
  embedded in Qt resources, and included in packages; keep essential workflows
  readable offline and preserve its path.
- **Build instructions:** `BUILD.md`; link to it instead of copying setup commands
  into architecture or community pages.
- **Dependency pins:** CMake and lockfiles first, with `DEPENDENCIES.md` as the
  readable overview. Legal source-closure material stays under `packaging/adb/`.
- **Performance evidence:** keep report and JSON paths stable. Benchmark scripts,
  CMake targets, and contract tests consume them.
- **Plans and history:** label their status. Do not silently turn an old proposal,
  benchmark result, or bug diary into a description of current behavior.

When updating a workflow, verify menu labels and platform restrictions against
the implementation, check relative links and heading anchors, and preview both
GitHub Markdown and embedded Help when the user guide changes. Repository prose
is English; translations belong in the application's translation catalogs.
