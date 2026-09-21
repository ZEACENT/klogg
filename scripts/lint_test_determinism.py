#!/usr/bin/env python3
"""Reject timing-flaky patterns in test code before they reach CI.

Flaky tests erode CI trust and burn runner time on retries. The patterns
rejected here are the historical root causes of flaky failures in this
repository:

* ``QTest::qSleep`` never processes events; it is a pure stall and has no
  legitimate use in tests. Use ``QTest::qWait`` or, better, a
  predicate/deadline helper such as SafeQSignalSpy::safeWait.
* Long ``QTest::qWait`` calls (> 500 ms) almost always compensate for
  non-deterministic dispatch. Fix the dispatch mechanism instead.
* Raw thread sleeps (``QThread::*sleep``, ``std::this_thread::sleep_for``)
  in test code are hope-waits; production fixtures that deliberately
  simulate slow writers must carry an explicit allowance.
* Iteration-bounded event-drain loops (``for ( i = 0; i < 10000 && !pred()``)
  spin out faster than asynchronous socket/IO delivery on loaded runners,
  which flaked the Windows ASan CI leg (PR #76). Bound drains by wall-clock
  with QElapsedTimer instead, like ``pumpEventsUntil`` in
  adb_smart_socket_*_test.cpp.
* Wall-clock budget assertions (``CHECK( elapsed < N )``, including the
  ``timer.elapsed() < N`` call form, ``now() - start`` chrono diffs, and
  line-wrapped assertions) are performance
  gates. They must be marked ``lint-allow: perf-budget`` AND live inside a
  Catch2 case tagged ``[.perf]`` so CI (which runs the default tag set)
  never executes them; developers run them locally via ``ctest -L perf``.

When a pattern is genuinely intentional, add a trailing comment on the same
line: ``// lint-allow: test-timing`` (waits/sleeps) or
``// lint-allow: perf-budget`` (budget assertions). The legacy
``// lint-allow: platform-fragile`` marker is accepted for waits/sleeps.

Usage:
    python3 scripts/lint_test_determinism.py
    python3 scripts/lint_test_determinism.py --paths tests/unit tests/ui

Exit codes:
    0   No findings.
    1   At least one finding (printed to stdout).
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import namedtuple
from pathlib import Path
from typing import Iterable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lint_platform_fragile import _strip_cpp_comments, _strip_cpp_literals

TIMING_MARKERS = ("lint-allow: test-timing", "lint-allow: platform-fragile")
PERF_MARKER = "lint-allow: perf-budget"

MAX_QWAIT_MS = 500

Finding = namedtuple("Finding", ["rule", "path", "line", "message"])

QSLEEP_RE = re.compile(r"\bQTest::qSleep\s*\(")
QWAIT_RE = re.compile(r"\bQTest::qWait\s*\(\s*(\d+)")
THREAD_SLEEP_RE = re.compile(
    r"\bQThread::(?:m|u)?sleep\s*\(|\bstd::this_thread::sleep_for\s*\("
)
ASSERT_RE = re.compile(r"\b(?:CHECK|REQUIRE|CHECK_FALSE|REQUIRE_FALSE)\s*\(")
ELAPSED_BUDGET_RE = re.compile(r"\b\w*[eE]lapsed\w*(?:\s*\(\s*\))?\s*<")
CHRONO_DIFF_RE = re.compile(r"\bnow\s*\(\s*\)\s*-")
SPIN_DRAIN_RE = re.compile(r"\bfor\s*\([^;]*;\s*\w+\s*<\s*\d{4,}\s*&&\s*!")
TEST_CASE_RE = re.compile(r"\b(?:TEST_CASE|SCENARIO)\s*\(")
PERF_TAG = "[.perf]"

TEST_SOURCE_SUFFIXES = {".cpp", ".h", ".hpp", ".mm"}


def _is_test_source(path: Path) -> bool:
    parts = path.as_posix()
    return (
        "/tests/" in f"/{parts}" or parts.startswith("tests/")
    ) and path.suffix in TEST_SOURCE_SUFFIXES


def _has_timing_marker(line: str) -> bool:
    return any(marker in line for marker in TIMING_MARKERS)


def _enclosing_case_has_perf_tag(stripped_lines: list[str], line_index: int) -> bool:
    for start in range(line_index, -1, -1):
        if TEST_CASE_RE.search(stripped_lines[start]):
            return any(
                PERF_TAG in stripped_lines[i] for i in range(start, line_index + 1)
            )
    return False


def _assertion_span(stripped_lines: list[str], start: int) -> tuple[str, int]:
    """Join a (possibly multiline) assertion into one logical text.

    A line-wrapped ``REQUIRE(`` would otherwise evade the per-line budget
    scan. Parentheses are counted on literal-masked lines, so parens inside
    string literals cannot skew the balance.
    """
    parts: list[str] = []
    balance = 0
    seen_open = False
    end = start
    for i in range(start, min(start + 20, len(stripped_lines))):
        line = stripped_lines[i]
        parts.append(line)
        balance += line.count("(") - line.count(")")
        seen_open = seen_open or "(" in line
        end = i
        if seen_open and balance <= 0:
            break
    return " ".join(parts), end


def check_text(text: str, path: Path) -> list[Finding]:
    if not _is_test_source(path):
        return []
    original_lines = text.splitlines()
    uncommented = _strip_cpp_comments(text).splitlines()
    stripped = _strip_cpp_literals("\n".join(uncommented)).splitlines()
    findings: list[Finding] = []
    for index, code in enumerate(stripped):
        original = original_lines[index] if index < len(original_lines) else ""
        line_no = index + 1
        if QSLEEP_RE.search(code):
            findings.append(
                Finding(
                    "qsleep-in-tests",
                    path,
                    line_no,
                    "QTest::qSleep never processes events; use a predicate/deadline "
                    "helper (e.g. SafeQSignalSpy::safeWait) instead.",
                )
            )
        qwait = QWAIT_RE.search(code)
        if (
            qwait
            and int(qwait.group(1)) > MAX_QWAIT_MS
            and not _has_timing_marker(original)
        ):
            findings.append(
                Finding(
                    "long-qwait",
                    path,
                    line_no,
                    f"QTest::qWait above {MAX_QWAIT_MS} ms compensates for "
                    "non-deterministic dispatch; observe the completion signal "
                    "instead, or add '// lint-allow: test-timing' with a reason.",
                )
            )
        if THREAD_SLEEP_RE.search(code) and not _has_timing_marker(original):
            findings.append(
                Finding(
                    "thread-sleep-in-tests",
                    path,
                    line_no,
                    "Raw thread sleeps in tests are hope-waits; gate on a semantic "
                    "condition, or add '// lint-allow: test-timing' with a reason.",
                )
            )
        if SPIN_DRAIN_RE.search(code) and not _has_timing_marker(original):
            findings.append(
                Finding(
                    "spin-drain-loop",
                    path,
                    line_no,
                    "Iteration-bounded event drains outrun asynchronous delivery on "
                    "loaded runners; bound by wall-clock with QElapsedTimer (see "
                    "pumpEventsUntil in adb_smart_socket_*_test.cpp), or add "
                    "'// lint-allow: test-timing' with a reason.",
                )
            )
        if ASSERT_RE.search(code):
            logical, span_end = _assertion_span(stripped, index)
            if ELAPSED_BUDGET_RE.search(logical) or CHRONO_DIFF_RE.search(logical):
                marker_present = any(
                    PERF_MARKER in original_lines[j]
                    for j in range(index, min(span_end + 1, len(original_lines)))
                )
                if not marker_present:
                    findings.append(
                        Finding(
                            "perf-budget-assertion",
                            path,
                            line_no,
                            "Wall-clock budget assertions are performance gates; add "
                            "'// lint-allow: perf-budget' and tag the case '[.perf]' "
                            "so CI never gates on runner speed.",
                        )
                    )
                elif not _enclosing_case_has_perf_tag(uncommented, index):
                    findings.append(
                        Finding(
                            "perf-budget-needs-perf-tag",
                            path,
                            line_no,
                            "Perf-budget assertion must live in a Catch2 case tagged "
                            "'[.perf]' so the default CI test run excludes it.",
                        )
                    )
    return findings


def iter_test_sources(paths: Iterable[Path]) -> Iterable[Path]:
    for root in paths:
        if root.is_file() and _is_test_source(root):
            yield root
            continue
        if not root.is_dir():
            continue
        for candidate in sorted(root.rglob("*")):
            if candidate.is_file() and _is_test_source(candidate):
                yield candidate


def scan_repository(root: Path, paths: Iterable[str] = ("tests",)) -> list[Finding]:
    roots = [root / relative for relative in paths]
    findings: list[Finding] = []
    for source in iter_test_sources(roots):
        findings.extend(check_text(source.read_text(encoding="utf-8"), source))
    return findings


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--paths",
        nargs="+",
        default=["tests"],
        help="Directories or files to scan (default: tests).",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = Path(__file__).resolve().parents[1]
    findings = scan_repository(repo_root, args.paths)
    for finding in findings:
        print(f"[{finding.rule}] {finding.path}:{finding.line}: {finding.message}")
    if findings:
        print(f"Found {len(findings)} test-determinism issue(s).")
        return 1
    print("OK: no test-determinism issues found.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
