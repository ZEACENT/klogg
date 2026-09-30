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
* Double-checked wait predicates (``while ( !pred() && ... )`` followed by
  ``return pred();``) re-read volatile state after the loop observed it
  true; a torn re-read (e.g. a heartbeat file mid-rewrite) flips the result
  and flakes the test (macOS arm64 CI leg, PR #76). Evaluate once per
  iteration and return the observed value.
* Wall-clock budget assertions (``CHECK( elapsed < N )``, including the
  ``timer.elapsed() < N`` call form, ``now() - start`` chrono diffs, and
  line-wrapped assertions) are performance
  gates. They must have a ``// lint-allow: perf-budget -- <nonempty reason>``
  comment AND live inside a Catch2 case tagged ``[.perf]`` so CI (which runs
  the default tag set) never executes them; developers run them locally via
  ``ctest -L perf``.
  Assertion polarity matters: ``CHECK_FALSE( elapsed < N )`` and
  ``CHECK( elapsed >= N )`` demand a *minimum* duration, so they are
  correctness assertions and stay in the default CI run.
* ``KLOGG_CHECK_PERF_BUDGET( expr )`` never evaluates ``expr`` in CI --
  the macro only fires when ``KLOGG_PERF_GATES=1`` is set, and CI never
  sets it. Every call site therefore needs a
  ``// lint-allow: perf-budget -- <nonempty reason>`` comment stating why
  skipping it is safe. Routing a *correctness* or
  *liveness* property through the macro silently drops CI coverage (PR
  #76 did this to the "returns immediately" and "gate timeout" checks);
  those must be asserted deterministically instead -- observe the
  mechanism (dispatch thread, effective timeout) rather than the elapsed
  time.

When a wait/sleep is genuinely intentional, add a trailing comment on the
same line: ``// lint-allow: test-timing``. The legacy
``// lint-allow: platform-fragile`` marker is also accepted for waits/sleeps.
Budget assertions and ``KLOGG_CHECK_PERF_BUDGET`` call sites require a genuine
``// lint-allow: perf-budget -- <nonempty reason>`` comment within the
assertion's line span (including its closing line). The reason must be on
the same comment line as the marker; literals and unrelated comments do not
satisfy the allowance.

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
PERF_MARKER_RE = re.compile(r"//[ \t]*lint-allow: perf-budget[ \t]+--[ \t]+\S")

MAX_QWAIT_MS = 500

Finding = namedtuple("Finding", ["rule", "path", "line", "message"])

QSLEEP_RE = re.compile(r"\bQTest::qSleep\s*\(")
QWAIT_RE = re.compile(r"\bQTest::qWait\s*\(\s*(\d+)")
THREAD_SLEEP_RE = re.compile(
    r"\bQThread::(?:m|u)?sleep\s*\(|\bstd::this_thread::sleep_for\s*\("
)
ASSERT_RE = re.compile(r"\b(?:CHECK|REQUIRE|CHECK_FALSE|REQUIRE_FALSE)\s*\(")
# An assertion opcode that inverts its expression. The `<` matchers below only
# describe a runner-speed gate when the assertion demands a small duration;
# negated, the same comparison forbids one, i.e. it is a minimum-duration
# correctness assertion that must keep running in CI.
NEGATED_ASSERT_RE = re.compile(r"\b(?:CHECK|REQUIRE)_FALSE\s*\(")
# The sanctioned opt-in macro for wall-clock budgets. Its expression is skipped
# unless KLOGG_PERF_GATES=1, so a call site is an explicit statement that CI
# does not check the property (see the module docstring).
KLOGG_PERF_BUDGET_RE = re.compile(r"\bKLOGG_CHECK_PERF_BUDGET\s*\(")
ELAPSED_BUDGET_RE = re.compile(r"\b\w*[eE]lapsed\w*(?:\s*\(\s*\))?\s*<")
CHRONO_DIFF_RE = re.compile(r"\bnow\s*\(\s*\)\s*-")
# duration_cast<...> / static_cast<...> angle brackets are not comparisons;
# strip them before looking for the budget's upper-bound '<'.
CAST_RE = re.compile(r"\b\w*cast\s*<[^;<>]*>")
SPIN_DRAIN_CAP = r"\w+\s*<\s*\d{4,}"
SPIN_DRAIN_RE = re.compile(
    rf"\bfor\s*\([^;]*;\s*(?:{SPIN_DRAIN_CAP}\s*&&\s*!|!\w+\s*\(\s*\)\s*&&\s*{SPIN_DRAIN_CAP})"
)
DOUBLE_CHECK_WHILE_RE = re.compile(r"\bwhile\s*\(\s*!(\w+)\s*\(\s*\)\s*&&")
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


def _is_chrono_budget(logical: str) -> bool:
    """A chrono diff is only a budget when it feeds an upper-bound '<'.

    `CHECK( now() - started >= minimum )` is a correctness assertion, not a
    runner-speed gate. Strip cast<> angle brackets first so the '<' inside
    `duration_cast<milliseconds>( ... )` is not mistaken for the comparison.
    """
    diff = CHRONO_DIFF_RE.search(logical)
    if diff is None:
        return False
    tail = CAST_RE.sub("cast", logical[diff.end() :])
    return "<" in tail


def _is_negated_assertion(logical: str) -> bool:
    """True for `CHECK_FALSE` / `REQUIRE_FALSE` assertion bodies.

    `CHECK_FALSE( timer.elapsed() < minimum )` asserts a minimum duration, the
    opposite of a runner-speed budget, so the `<` matchers must not classify it
    as one: the mandatory gate would otherwise demand a `[.perf]` tag and push
    a correctness assertion out of the default CI run.
    """
    return NEGATED_ASSERT_RE.search(logical) is not None


def _span_has_perf_marker(
    comment_lines: list[str], start: int, end: int
) -> bool:
    return any(
        PERF_MARKER_RE.search(comment_lines[j]) is not None
        for j in range(start, min(end + 1, len(comment_lines)))
    )


def check_text(text: str, path: Path) -> list[Finding]:
    if not _is_test_source(path):
        return []
    original_lines = text.splitlines()
    comment_masked = _strip_cpp_comments(text)
    # The existing lexer preserves positions and literals. Invert its comment
    # mask so strings cannot spoof allowances and quotes in reasons stay intact.
    comment_lines = "".join(
        original if original != masked or original == "\n" else " "
        for original, masked in zip(text, comment_masked)
    ).splitlines()
    uncommented = comment_masked.splitlines()
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
        double_check = DOUBLE_CHECK_WHILE_RE.search(code)
        if double_check:
            predicate_name = double_check.group(1)
            trailing_return = re.compile(
                rf"\breturn\s+{re.escape(predicate_name)}\s*\(\s*\)\s*;"
            )
            lookahead_end = min(index + 9, len(stripped))
            for follow in range(index + 1, lookahead_end):
                if trailing_return.search(stripped[follow]) and not _has_timing_marker(
                    original_lines[follow] if follow < len(original_lines) else ""
                ):
                    findings.append(
                        Finding(
                            "double-checked-predicate",
                            path,
                            follow + 1,
                            f"Trailing `return {predicate_name}();` re-reads volatile "
                            "state after the while loop already observed it true; "
                            "evaluate once per iteration and return the observed "
                            "value, or add '// lint-allow: test-timing' with a reason.",
                        )
                    )
                    break
        if KLOGG_PERF_BUDGET_RE.search(code) and not code.lstrip().startswith("#"):
            # Preprocessor lines are the macro's own #define/#undef, not a
            # call site.
            _, budget_span_end = _assertion_span(stripped, index)
            if not _span_has_perf_marker(comment_lines, index, budget_span_end):
                findings.append(
                    Finding(
                        "perf-budget-unmarked",
                        path,
                        line_no,
                        "KLOGG_CHECK_PERF_BUDGET never runs in CI: its expression is "
                        "skipped unless KLOGG_PERF_GATES=1 is set, and CI never sets "
                        "it. Add '// lint-allow: perf-budget -- <nonempty reason>' "
                        "within the assertion span, and keep "
                        "the property covered in CI -- a genuine speed budget belongs "
                        "to scripts/run_perf_gates.py, while a correctness or liveness "
                        "property (which thread ran the work, which timeout reached "
                        "the lock) must be asserted deterministically instead.",
                    )
                )
        if ASSERT_RE.search(code):
            logical, span_end = _assertion_span(stripped, index)
            is_budget = (
                ELAPSED_BUDGET_RE.search(logical) is not None
                or _is_chrono_budget(logical)
            )
            if is_budget and not _is_negated_assertion(logical):
                if not _span_has_perf_marker(comment_lines, index, span_end):
                    findings.append(
                        Finding(
                            "perf-budget-assertion",
                            path,
                            line_no,
                            "Wall-clock budget assertions are performance gates; add "
                            "'// lint-allow: perf-budget -- <nonempty reason>' within "
                            "the assertion span and tag the case '[.perf]' "
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
