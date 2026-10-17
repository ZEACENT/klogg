# Contributing to klogg

Thank you for helping make logs easier to understand. Useful contributions
include bug reports, documentation, accessibility and usability improvements,
translations, reproducible performance investigations, and code.

[Documentation hub](docs/README.md) · [Build guide](docs/BUILD.md) ·
[Code of Conduct](CODE_OF_CONDUCT.md)

## Before you start

This is the [ZEACENT fork](https://github.com/ZEACENT/klogg), built on
[variar/klogg](https://github.com/variar/klogg). **GitHub is this project's only
communication channel.** Check that your report or change belongs to the version
you are using.

- Keep each issue or pull request focused on one problem or coherent change.
- Discuss significant new features or architectural changes in an issue before
  investing in a large implementation.
- Consider Windows, Linux, and macOS, as well as the relevant file, folder-search,
  and live-device scenarios. Their shared UI does not make every behavior identical.
- Be constructive and welcoming. Follow the [Code of Conduct](CODE_OF_CONDUCT.md).

## Report a bug

Use [GitHub Issues](https://github.com/ZEACENT/klogg/issues) for ordinary bug
reports. Include:

1. The klogg version and package source, from the About dialog or release page.
2. Operating system, processor architecture, and relevant display or device details.
3. The source type: single file, folder search, Android capture, or macOS iOS capture.
4. Steps to reproduce, expected behavior, and actual behavior.
5. A small synthetic or sanitized example, and screenshots or diagnostics when useful.

Do not upload private log content, credentials, personal device identifiers, or
customer data. Large-file problems are easier to investigate with a reproducible
data generator and exact search pattern than with an unexplained timing number.

**Security concerns are different:** read [SECURITY.md](SECURITY.md). Do not post
sensitive vulnerability details in a public issue or assume an upstream contact
maintains this fork.

## Suggest an improvement

Describe the workflow and the outcome you need, not only a proposed control.
Explain which source types it affects and what you do today as a workaround.
The [backlog](docs/BACKLOG.md) and
[panel specification](docs/SPEC_CHART_AND_FILTERS_PANEL.md) are planning records,
not commitments or lists of already shipped features.

## Improve the documentation

The [documentation hub](docs/README.md) describes each document's role:

- `README.md` introduces the product and points readers to downloads and guides.
- `DOCUMENTATION.md` is the canonical user guide and is also embedded in Help.
  Keep essential instructions self-contained and compatible with the maddy HTML renderer.
- `docs/BUILD.md` owns build and test instructions.
- `docs/DEPENDENCIES.md` summarizes pins; CMake and lockfiles remain authoritative.
- Architecture documents explain current behavior; proposals and historical
  investigations must be clearly labeled as such.

Use English for repository prose. Application translations belong in
`src/app/i18n/`. Verify menu labels, shortcuts, platform limitations, links, and
heading anchors. Use real screenshots with non-sensitive content, and identify
older images instead of presenting them as current builds. Preserve benchmark
data and its provenance; do not turn one controlled result into a universal claim.

## Contribute code

1. Fork the repository and create a branch for your change.
2. Follow the [build guide](docs/BUILD.md) and existing `.clang-format` style.
3. Add or update tests that demonstrate the intended behavior. See
   [portability guidance](docs/PORTABILITY.md) for cross-platform and asynchronous
   contracts. Prefer observable completion over timing-dependent sleeps.
4. Run the relevant tests and the fast quality gate:

   ```sh
   python3 scripts/run_ci_quality.py
   git diff --check
   ```

5. Follow the build guide for sanitizer checks relevant to the change. Performance
   budgets are opt-in local checks, not a replacement for CI correctness tests.
6. Update affected user or developer documentation and open a pull request.

State the problem, how to reproduce it, the approach, and what you verified.
Name checks that were not run and any remaining platform limitations. Keep
unrelated formatting, generated output, and dependency upgrades out of the change.

## Commit messages

Use a short, descriptive English subject. A prefix such as `fix:`, `feat:`,
`docs:`, `test:`, `build:`, `ci:`, `perf:`, or `refactor:` is welcome when it helps
explain the change. For nontrivial changes, explain the motivation and relevant
verification in the body rather than repeating the diff.

## Recognition and license

Contributors build on the work of Nicolas Bonnefon, Anton Filimonov, and the
wider glogg/klogg community. See the
[contributors](https://github.com/ZEACENT/klogg/graphs/contributors),
[GPL license](COPYING), and [third-party notices](NOTICE).
