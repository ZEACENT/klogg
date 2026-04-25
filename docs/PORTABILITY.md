# klogg portability guide

## 1. Why this document exists

External-process invocation is one of klogg's biggest cross-platform footguns. The same `QProcess::start("toolname", ...)` call that works on a developer's terminal-launched build can silently fail in a Finder-launched `klogg.app`, or in a Linux build started from a desktop launcher. New contributors should read this document before adding any new dependency on an external binary (adb, git, gh, make, future tools).

## 2. The macOS GUI-launchd PATH trap

When a macOS application is launched from Finder, Dock, Spotlight, or `open(1)`, it does not inherit the user's interactive shell environment. It inherits the environment of the launchd user-domain session. The practical consequences:

- `launchctl getenv PATH` is typically empty on a stock system. With no `PATH` in the environment, the kernel's `posix_spawn`/`execvp` falls back to `_PATH_DEFPATH`, defined in `<paths.h>` as `/usr/bin:/bin:/usr/sbin:/sbin`.
- `/etc/paths` and `/etc/paths.d/*` are consumed by `path_helper(1)`. `path_helper` is invoked from the system-wide shell rc files (`/etc/zprofile`, `/etc/profile`). It runs only for shell sessions. GUI-spawned processes never see its output.
- Therefore `/usr/local/bin` (Homebrew on Intel), `/opt/homebrew/bin` (Homebrew on Apple Silicon), `~/Library/Android/sdk/platform-tools`, `~/.cargo/bin`, and every other typical developer install location are invisible to a `QProcess` spawned by GUI-launched klogg.

A curious developer can verify this empirically:

```sh
# Empty on most systems, even though the shell PATH is rich.
launchctl getenv PATH

# Build a one-off probe app:
#   MyProbe.app/Contents/MacOS/MyProbe -> shell script that writes
#   "$PATH" and `which adb` to /tmp/probe.log. Launch via `open MyProbe.app`
#   (NOT from the terminal, which would inherit the shell env) and inspect
#   /tmp/probe.log to see exactly what a GUI-spawned process gets.
```

This is not a bug. It is the documented behavior of launchd's per-user domain. Apps that need to find developer tools must resolve absolute paths themselves.

## 3. Why Windows and Linux are usually fine

| Platform | GUI-launch PATH source                                                                          | Typical outcome                                                                                                                    |
| -------- | ----------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| Windows  | `HKLM\SYSTEM\...\Environment` + `HKCU\Environment`, merged by the session                       | GUI and console apps inherit the same `PATH`. The Android SDK installer adds `%ANDROID_SDK_ROOT%\platform-tools` to System `PATH`. |
| Linux    | DE session env, usually populated from `~/.profile`, `/etc/profile`, `/etc/environment`, PAM    | Most distros' DE sessions read these files at login. Less uniform than Windows, but `/usr/bin`, `/usr/local/bin` are always there. |
| macOS    | launchd user-domain env (typically empty) + `_PATH_DEFPATH`                                     | GUI-launched apps see only `/usr/bin:/bin:/usr/sbin:/sbin`.                                                                        |

Windows almost never trips the bare-basename trap. Linux trips it occasionally for tools installed under `~/.local/bin` or non-standard prefixes, particularly under Wayland compositors that do not source `~/.profile`. Defensive resolution is still cheap and should be applied uniformly.

## 4. The convention klogg adopts

- **Never call `QProcess::start("toolname", ...)` directly with a bare basename for tools that ship outside the OS base set.** The OS base set is `/usr/bin`, `/bin`, `C:\Windows\System32`. For `adb`, `git`, `gh`, `make`, and similar developer tools, always go through a centralized resolver that returns an absolute path.
- **Prefer absolute paths in `QProcess::start`.** If the user has configured an explicit path through Configuration / OptionsDialog (External Tools), use that string verbatim, do not re-resolve it. Otherwise, resolve to an absolute path by probing known install locations.
- **Probe order:**
  1. Explicit user-configured path from settings.
  2. Tool-specific environment variables (for adb: `ANDROID_SDK_ROOT`, then `ANDROID_HOME`).
  3. Platform-default install locations (Homebrew Intel, Homebrew Apple Silicon, Program Files, LocalAppData, common user-local SDK directories).
  4. Bare basename as a last-resort `PATH` fallback. This succeeds in terminal-launched and Windows cases and gracefully fails everywhere else, where the user is then prompted to configure the path.

The reference implementation is `findAdbAtKnownLocation()` at `src/ui/src/adbprocesstransport.cpp:24`, called from the resolver at `src/ui/src/adbprocesstransport.cpp:80`. New external-tool integrations should mirror its structure.

**Trust boundary.** The probe consults environment variables (`ANDROID_SDK_ROOT`, `ANDROID_HOME`, `LOCALAPPDATA`, `ProgramFiles`) and follows whatever paths the user supplies through them. This is intentional and matches the trust model of the underlying tool itself: an attacker who can rewrite the user's shell rc files or launchd session env can already inject a malicious `adb`/`git`/etc. directly via PATH. Future contributors should NOT try to "harden" the resolver by skipping env-var probes -- the reduced functionality (Android Studio's standard SDK env var no longer being honored) is not paid for by any real security gain.

## 5. Adding a new external tool — checklist

Copy this into the PR description that introduces a new external-process dependency:

- [ ] Tool path is configurable via Configuration / OptionsDialog.
- [ ] A default-empty configuration value triggers known-location probing rather than failing immediately.
- [ ] The known-location list covers, at minimum:
  - Windows: relevant environment variables (e.g. `ANDROID_SDK_ROOT`), `%ProgramFiles%`, `%LocalAppData%\Android\Sdk\platform-tools`.
  - macOS: `/usr/local/bin` (Homebrew Intel), `/opt/homebrew/bin` (Apple Silicon), and any tool-specific user directory (e.g. `~/Library/Android/sdk/platform-tools`).
  - Linux: `/usr/local/bin`, `/usr/bin`, and a common user-local directory (e.g. `~/Android/Sdk/platform-tools`). User-provided locations like `~/.local/bin` are deliberately NOT auto-probed -- they are too tool-agnostic and would be surprising as a default; users running tools out of `~/.local/bin` should set the explicit path or extend `PATH` for klogg's own launch.
- [ ] A unit test asserts that, when the tool is installed at a known location (or the test fakes one via a temp directory and an env-var override), the resolver returns an absolute, existing path.
- [ ] `QProcess::ProcessError` from `errorOccurred` is surfaced to the user with a message that names the tool and points at the configuration entry.
- [ ] No naked `QProcess::start("toolname")` anywhere in `src/`. A grep for `QProcess::start\("[a-z]` should not find any new occurrences in the diff.

## 6. Other cross-platform footguns

A short list, with no deep dive — it is enough to know these exist before reaching for string concatenation or platform-specific APIs:

- **Path separators.** Use `QDir::cleanPath`, `QDir::toNativeSeparators`, `QFileInfo`, and `QDir::filePath` rather than concatenating with `/` or `\\`.
- **Line endings.** Treat `\r\n` and `\n` interchangeably on input; emit native line endings on output. `QTextStream::setAutoDetectUnicode` and `QIODevice::Text` flags exist for this.
- **`QProcess::setProcessChannelMode` differences.** On Windows, merged channels go through ConPTY; on Unix they go through a pty pair. Ordering and buffering are not identical; do not assume interleaved stdout/stderr line ordering matches across platforms.
- **File watching backends.** klogg's `klogg_filewatch` already abstracts over efsw (FSEvents on macOS, inotify on Linux, ReadDirectoryChangesW on Windows). Latency, batching, and rename-detection corner cases differ per backend; new file-watch features should be exercised on all three.
- **Qt 5 signal overload disambiguation.** Overloaded signals like `QProcess::finished(int, QProcess::ExitStatus)` versus the deprecated single-argument form must be disambiguated with `qOverload<int, QProcess::ExitStatus>(&QProcess::finished)`. See `src/ui/src/livesourcetransport.cpp:60` and `src/ui/src/livesourcetransport.cpp:178` for the established pattern.

## 7. CI guardrails to consider

A true regression test for the macOS GUI-launch case would launch `klogg.app` via `open` from a clean `env -i` shell, exercise the adb dialog, and assert that the resolver finds the tool. This guards against the exact scenario that motivated `findAdbAtKnownLocation`.

This test is currently NOT wired up in CI. The macOS job builds and runs unit tests under a terminal-inherited environment, which masks the launchd PATH trap. If a contributor wires up such a test (a small AppleScript or `osascript` driver around `open -a klogg.app` inside a `env -i` shell, plus a UI-test hook), this section is the place to document the runner, the expected failure modes, and how to update the known-location list when SDK install paths change.

`scripts/lint_platform_fragile.py` runs in `.github/workflows/lint.yml` on every push and PR; it fails the job for the two specific patterns klogg has tripped on in the past (`startsWith(QLatin1Char('/'))` for absolute-path tests, `endsWith("\\foo")` for path-suffix tests). Add new patterns there when a future Windows-only failure suggests one. Inline `// lint-allow: platform-fragile` to override on a single line when the use is intentional.

## 8. Test patterns to avoid

These patterns pass on the developer's macOS / Linux build and silently break on Windows CI. Each maps to at least one regression klogg has actually hit; the fixes are the conventions everyone should reach for first.

### 8.1 Path-shape assertions

```cpp
// AVOID -- Windows absolute paths look like "C:/Users/...".
REQUIRE( path.startsWith( QLatin1Char( '/' ) ) );
// AVOID -- Qt normalises path separators to '/' on every platform,
// so a literal containing '\\' never matches what Qt actually emits.
REQUIRE( path.endsWith( QStringLiteral( "\\adb.exe" ) ) );

// PREFER -- portable predicates from QFileInfo.
REQUIRE( QFileInfo( path ).isAbsolute() );
REQUIRE( QFileInfo( path ).fileName().compare(
             QStringLiteral( "adb.exe" ), Qt::CaseInsensitive ) == 0 );
```

`scripts/lint_platform_fragile.py` rejects both AVOID forms. Compare leaves via `QFileInfo::fileName()` and absoluteness via `QFileInfo::isAbsolute()`; never reach for the leading-slash convention.

### 8.2 Signal-timing assertions

```cpp
// AVOID -- relies on the throttler firing again AFTER an unthrottled
// progress == 100 emit; Windows timer granularity (~15.6ms) makes this
// path emit zero residue, and the test silently passes 0 > 0.
spy.clear();
QTest::qWait( 5000 );
REQUIRE( spy.count() > 0 );

// PREFER -- inspect the signals captured during the consume loop.
// runSearch()'s helper has been adjusted to leave the spy populated
// instead of clearing it; SCENARIOs read spy.at(i) for i < spy.count().
for ( int i = 0; i < spy.count(); ++i ) {
    const auto args = spy.at( i );
    // ... assert per-signal contract ...
}
```

If the test must wait for a specific deadline, drive the wait off a deterministic state-change predicate (`waitUiState([&]{ return getCount() >= N; })`) rather than off arbitrary signal-spy residue.

### 8.3 External-tool dependence

```cpp
// AVOID -- runner-state-dependent assumption; on a runner that does
// not satisfy the precondition (no adb installed, ping unavailable,
// limited entropy, etc.), CI fails for reasons unrelated to the code
// under test, while macOS / Linux luck into passing.
REQUIRE( transport.connectTransport() );

// PREFER -- explicit skip-with-warning when the precondition is the
// runner environment, not the production behaviour:
KLOGG_REQUIRE_OR_WARN_SKIP(
    transport.connectTransport(),
    "StreamingScriptTransport: connectTransport failed in this "
    "environment -- pipeline behaviour is exercised elsewhere" );
```

The `KLOGG_REQUIRE_OR_WARN_SKIP` helper lives in `tests/helpers/test_utils.h`. Use it whenever the predicate is "is the environment in the shape my test needs?" rather than "does the production code under test behave correctly?"; reserve plain `REQUIRE` / `CHECK` for the latter.

### 8.4 UI-thread sync via predicate, not fixed millisecond budget

```cpp
// AVOID -- runInUiThread internally pumps events for QTest::qWait(100),
// then control returns regardless of whether the queued lambda actually
// ran.  On a slow runner (Ubuntu 20.04 docker, busy CI host) the lambda
// has not been pumped within 100ms; the immediately-following REQUIRE
// then fires with a confusing "value is empty" diagnostic that has
// nothing to do with the production code under test.
runInUiThread( [&] { groupId = TabGroupManager::get().groups().back().id; } );
REQUIRE( !groupId.isEmpty() );

// PREFER -- waitUiState polls up to 10 s for the predicate.  On a
// healthy runner this returns within milliseconds; on a slow runner
// it absorbs the variance instead of producing a flake.
runInUiThread( [&] { groupId = TabGroupManager::get().groups().back().id; } );
REQUIRE( waitUiState( [&] { return !groupId.isEmpty(); } ) );
```

The same rule applies any time a test depends on a queued lambda
having been pumped, an event-loop-driven cache having been refreshed,
a layout-pass having run, etc.  When the assertion is "the world
should reach state X eventually", `waitUiState` (or any equivalent
poll-with-deadline helper) is the right tool; a fixed `QTest::qWait`
is not.

### 8.5 Why these four rules and not more

Every rule in this section is the *direct generalisation* of a specific past klogg bug:

- §8.1 generalises PR #12's two Windows-only ctest failures (`adb_ui_transport_test.cpp:103, 248, 455` in the original branch).
- §8.2 generalises PR #12's `runSearch()` helper drain-and-reread bug.
- §8.3 generalises PR #12's `StreamingScriptTransport.connectTransport()` Windows flake.
- §8.4 generalises PR #12's third-round `mainwindow_test.cpp:280` Tab-group-chip flake on the slow Ubuntu 20.04 runner.

When the next platform-specific failure shows up, add a §8.6+ rule grounded in the actual incident. Do not pre-emptively add abstract anti-patterns -- the value of this section is that each rule has a paid-for receipt.

---

_Last updated: 2026-04-25_
