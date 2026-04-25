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
