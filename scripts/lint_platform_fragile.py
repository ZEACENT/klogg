#!/usr/bin/env python3
"""
Catches platform-fragile patterns that pass on the developer's macOS / Linux
build but fail on Windows CI.  Run before pushing or as a CI gate.

Background: PR #12 surfaced two such patterns -- `startsWith(QLatin1Char('/'))`
treating the leading-slash convention as portable, and `endsWith("\\adb.exe")`
treating Windows backslash separators as canonical even though Qt normalises
paths to forward slashes on every platform.  Both passed local Apple Clang
and Linux runners and only failed on the Windows runners, where the absolute
path has a drive-letter prefix and Qt-normalised forward slashes.

This script is intentionally narrow: high-precision patterns that have ZERO
false positives on the current klogg tree and that a future contributor is
likely to reach for again.  When the lint genuinely needs to be skipped,
add a trailing comment `// lint-allow: platform-fragile` on the same line.

Usage:
    python3 scripts/lint_platform_fragile.py
    python3 scripts/lint_platform_fragile.py --paths src tests benchmarks
    python3 scripts/lint_platform_fragile.py --check-staged   # pre-commit mode

Exit codes:
    0   No findings.
    1   At least one finding (printed to stdout).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable

ALLOW_MARKER = "lint-allow: platform-fragile"

PATTERNS: list[dict] = [
    {
        "name": "leading-slash absolute-path test",
        # Matches startsWith('/'), startsWith("/"), startsWith(QLatin1Char('/')),
        # startsWith(QStringLiteral("/")) and a few minor variants.
        "regex": re.compile(
            r"\.startsWith\s*\(\s*"
            r"(?:QLatin1Char\s*\(\s*'/'\s*\)"
            r"|QLatin1String\s*\(\s*\"/\"\s*\)"
            r"|QStringLiteral\s*\(\s*\"/\"\s*\)"
            r"|\"/\""
            r")\s*\)"
        ),
        "fix": (
            "Use QFileInfo(path).isAbsolute() to test for absolute paths. "
            "Windows absolute paths look like 'C:/Users/...', not '/Users/...', "
            "and Qt normalises path separators to '/' on every platform."
        ),
    },
    {
        "name": "Windows-backslash path-suffix test",
        # Matches endsWith(...) where the literal contains a `\\` (two
        # backslash chars in source = one backslash in the runtime string).
        # We look for `\\` inside any string literal that's the argument to
        # endsWith, regardless of whether it's wrapped in QStringLiteral / etc.
        "regex": re.compile(
            r"\.endsWith\s*\([^)]*\"[^\"]*\\\\[^\"]*\"[^)]*\)"
        ),
        "fix": (
            "Qt normalises path separators to '/' on every platform, so a "
            "literal containing '\\\\' will not match what Qt actually emits "
            "for paths on Windows. Use endsWith(\"/foo\", Qt::CaseInsensitive) "
            "or compare QFileInfo(path).fileName() instead."
        ),
    },
    {
        "name": "whoami fake process-failure test",
        "regex": re.compile(r'QStringLiteral\s*\(\s*"whoami\.exe"\s*\)'),
        "fix": (
            "Do not use whoami.exe to simulate a failed Windows process. It can "
            "outlive short startup grace windows on slower CI runners and make "
            "connectTransport() report success. Create a temporary script that "
            "exits with a non-zero status instead."
        ),
    },
]

# Multi-line patterns: checked separately via whole-file analysis.
# Each entry has a name, a description, and a check function that
# receives the file text and path, returning a list of
# (line_number, message) tuples.

def _check_unguarded_platform_helper(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag namespace-scope function definitions that appear *before* an
    #ifdef Q_OS_* but whose *only* call sites are inside that #ifdef.

    This catches the pattern that broke Linux CI in PR #14: a helper
    function defined outside any platform guard but only called inside
    #ifdef Q_OS_MAC, which triggers -Werror=unused-function on other
    platforms.

    The check is heuristic-based (regex, not a real preprocessor) and
    focuses on anonymous-namespace and static free functions.
    """
    if ALLOW_MARKER in text:
        return []

    findings: list[tuple[int, str]] = []

    # Collect top-level function definitions (anonymous-namespace or static).
    # Matches lines like:  QStringList functionName( ... )  or  static void foo()
    func_def_re = re.compile(
        r"^(?:static\s+)?[\w:<>]+\s+\*?\s*(\w+)\s*\([^)]*\)\s*$"
    )
    # Collect #ifdef Q_OS_* guards.
    ifdef_re = re.compile(r"^\s*#\s*if(?:def|n?def)?\s+(Q_OS_\w+)")
    endif_re = re.compile(r"^\s*#\s*endif")

    lines = text.splitlines()
    # Build a map: function_name -> definition_line_number
    func_defs: dict[str, int] = {}
    for i, line in enumerate(lines, start=1):
        m = func_def_re.match(line)
        if m:
            func_defs[m.group(1)] = i

    if not func_defs:
        return findings

    # For each function, check whether all call sites are inside the same
    # #ifdef Q_OS_* block, and the definition is NOT.
    for func_name, def_line in func_defs.items():
        call_re = re.compile(rf"\b{re.escape(func_name)}\s*\(")
        # Find all call-site lines.
        call_lines = [
            (i, line)
            for i, line in enumerate(lines, start=1)
            if i != def_line and call_re.search(line) and not line.strip().startswith("#")
        ]
        if not call_lines:
            # Unused function — that's a different problem (-Wunused),
            # not a platform-guard mismatch. Skip.
            continue

        # Determine the #ifdef context of the definition line.
        def_guard = _guard_at_line(lines, def_line)
        # Determine the #ifdef context of each call site.
        call_guards = {_guard_at_line(lines, cl[0]) for cl in call_lines}

        # If *all* call sites share a guard that the definition does NOT have,
        # the definition is unguarded and will cause -Werror=unused-function
        # on other platforms.  If any call site is outside any guard (None),
        # the function IS used on all platforms and this is not a problem.
        if None in call_guards:
            continue
        common_guards = call_guards - {None}
        if common_guards and def_guard not in common_guards:
            for guard in common_guards:
                findings.append(
                    (
                        def_line,
                        f"Function '{func_name}' is defined outside #ifdef {guard} "
                        f"but all call sites are inside it. This will cause "
                        f"-Werror=unused-function on other platforms. "
                        f"Move the definition inside the same #ifdef {guard} guard.",
                    )
                )
                break  # one report per function is enough

    return findings


def _check_main_view_text_pixel_probe(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag main-view text pixel probes that assert on viewport grabs.

    PR #17 exposed this on Windows x86 / Qt5: even when the test waited and
    repeatedly grabbed the offscreen viewport, that runner could still return a
    blank frame. Keep this check narrow to main-view text pixel counters; use
    deterministic cache/layout assertions instead.
    """
    if path.name != "crawlerwidget_test.cpp" or ALLOW_MARKER in text:
        return []

    lines = text.splitlines()
    findings: list[tuple[int, str]] = []
    for i, line in enumerate(lines, start=1):
        if "grabMainViewport(" not in line or "=" not in line:
            continue

        context_after = "\n".join(lines[i - 1 : min(len(lines), i + 25)])
        if "textPixelsInLeftBand" in context_after and "textPixelsInRightBand" in context_after:
            findings.append(
                (
                    i,
                    "Main-view text pixel probes based on viewport grabs are flaky "
                    "on Windows Qt5 offscreen runners. Assert deterministic cache "
                    "or layout state instead of sampled text pixels.",
                )
            )

    return findings


_GUARD_RE = re.compile(r"^\s*#\s*if(?:def|n?def)?\s+(Q_OS_\w+)")
_ELSE_RE = re.compile(r"^\s*#\s*else")
_ENDIF_RE = re.compile(r"^\s*#\s*endif")


def _guard_at_line(lines: list[str], target: int) -> str | None:
    """Return the innermost active #ifdef Q_OS_* guard at the given
    1-based line number, or None if the line is not inside any such guard.

    Returns None for lines inside #else branches (they run on the
    complementary platform set, so a function used there IS used on
    other platforms).
    """
    # Each entry is (guard_name, in_else: bool)
    guard_stack: list[tuple[str, bool]] = []
    for i, line in enumerate(lines, start=1):
        if i > target:
            break
        m = _GUARD_RE.match(line)
        if m:
            guard_stack.append((m.group(1), False))
        elif _ELSE_RE.match(line):
            if guard_stack:
                name, _ = guard_stack[-1]
                guard_stack[-1] = (name, True)
        elif _ENDIF_RE.match(line):
            if guard_stack:
                guard_stack.pop()
    if guard_stack:
        name, in_else = guard_stack[-1]
        # If we're in the #else branch, the code runs on all platforms
        # EXCEPT the guarded one — treat as unguarded (None).
        return None if in_else else name
    return None


MULTI_LINE_CHECKS: list[dict] = [
    {
        "name": "unguarded-platform-helper",
        "check": _check_unguarded_platform_helper,
    },
    {
        "name": "main-view-text-pixel-probe",
        "check": _check_main_view_text_pixel_probe,
    },
]


def iter_target_files(paths: Iterable[Path]) -> Iterable[Path]:
    for root in paths:
        if not root.exists():
            continue
        for ext in ("*.cpp", "*.h", "*.hpp", "*.cc"):
            yield from root.rglob(ext)


def iter_staged_files() -> Iterable[Path]:
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    for raw in out.splitlines():
        if not raw.strip():
            continue
        if raw.endswith((".cpp", ".h", ".hpp", ".cc")):
            yield Path(raw)


def lint_file(path: Path) -> int:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0
    issues = 0

    # Single-line patterns.
    for line_num, raw in enumerate(text.splitlines(), start=1):
        if ALLOW_MARKER in raw:
            continue
        for pat in PATTERNS:
            if pat["regex"].search(raw):
                print(f"[platform-fragile] {pat['name']}")
                print(f"  at {path}:{line_num}")
                print(f"      {raw.strip()}")
                print(f"  fix: {pat['fix']}")
                print()
                issues += 1

    # Multi-line checks.
    for check in MULTI_LINE_CHECKS:
        findings = check["check"](text, path)
        for line_num, message in findings:
            print(f"[platform-fragile] {check['name']}")
            print(f"  at {path}:{line_num}")
            print(f"  {message}")
            print()
            issues += 1

    return issues


def main(argv: list[str]) -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--paths",
        nargs="*",
        default=["src", "tests", "benchmarks"],
        help="Source directories to scan, relative to repo root.",
    )
    parser.add_argument(
        "--check-staged",
        action="store_true",
        help="Only scan files staged for commit (pre-commit hook mode).",
    )
    args = parser.parse_args(argv)

    if args.check_staged:
        files = list(iter_staged_files())
    else:
        roots = [repo_root / p for p in args.paths]
        files = list(iter_target_files(roots))

    issues = 0
    for f in files:
        issues += lint_file(f if f.is_absolute() else repo_root / f)

    if issues:
        print(f"Found {issues} platform-fragile pattern(s).")
        print(
            f"Add `// {ALLOW_MARKER}` on a specific line to override an intentional use."
        )
        return 1
    print("OK: no platform-fragile patterns found.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
