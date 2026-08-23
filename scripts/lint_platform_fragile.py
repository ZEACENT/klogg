#!/usr/bin/env python3
"""
Catches platform-fragile patterns that pass on the developer's macOS / Linux
build but fail in CI (Windows runners, or Linux-docker packaging).  Run before
pushing or as a CI gate.

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


def _strip_cpp_comments(text: str) -> str:
    """Blank C++ line and block comments while preserving literals and lines."""
    output: list[str] = []
    state = "code"
    escaped = False
    raw_terminator = ""
    i = 0
    while i < len(text):
        ch = text[i]
        next_ch = text[i + 1] if i + 1 < len(text) else ""

        if state == "line-comment":
            if ch == "\n":
                output.append(ch)
                state = "code"
            else:
                output.append(" ")
        elif state == "block-comment":
            if ch == "*" and next_ch == "/":
                output.extend((" ", " "))
                state = "code"
                i += 1
            else:
                output.append(ch if ch == "\n" else " ")
        elif state == "raw-string":
            if text.startswith(raw_terminator, i):
                output.extend(raw_terminator)
                i += len(raw_terminator) - 1
                state = "code"
            else:
                output.append(ch)
        elif state in ("string", "character"):
            output.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif (state == "string" and ch == '"') or (
                state == "character" and ch == "'"
            ):
                state = "code"
        elif ch == "/" and next_ch == "/":
            output.extend((" ", " "))
            state = "line-comment"
            i += 1
        elif ch == "/" and next_ch == "*":
            output.extend((" ", " "))
            state = "block-comment"
            i += 1
        else:
            raw_open = -1
            if ch == "R" and next_ch == '"':
                candidate_open = text.find("(", i + 2, min(len(text), i + 19))
                if candidate_open >= 0:
                    delimiter = text[i + 2 : candidate_open]
                    if not any(char.isspace() or char in "()\\" for char in delimiter):
                        raw_open = candidate_open

            if raw_open >= 0:
                output.extend(text[i : raw_open + 1])
                raw_terminator = ")" + text[i + 2 : raw_open] + '"'
                state = "raw-string"
                i = raw_open
            else:
                output.append(ch)
                if ch == '"':
                    state = "string"
                    escaped = False
                elif ch == "'" and not (
                    i > 0
                    and i + 1 < len(text)
                    and text[i - 1].isalnum()
                    and text[i + 1].isalnum()
                ):
                    state = "character"
                    escaped = False
        i += 1

    return "".join(output)


def _strip_cpp_literals(text: str) -> str:
    """Blank C++ string/character literals while preserving code and lines."""
    output: list[str] = []
    state = "code"
    escaped = False
    raw_terminator = ""
    i = 0
    while i < len(text):
        ch = text[i]
        next_ch = text[i + 1] if i + 1 < len(text) else ""

        if state == "raw-string":
            if text.startswith(raw_terminator, i):
                output.extend(" " * len(raw_terminator))
                i += len(raw_terminator) - 1
                state = "code"
            else:
                output.append("\n" if ch == "\n" else " ")
        elif state in ("string", "character"):
            output.append("\n" if ch == "\n" else " ")
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif (state == "string" and ch == '"') or (
                state == "character" and ch == "'"
            ):
                state = "code"
        else:
            raw_open = -1
            if ch == "R" and next_ch == '"':
                candidate_open = text.find("(", i + 2, min(len(text), i + 19))
                if candidate_open >= 0:
                    delimiter = text[i + 2 : candidate_open]
                    if not any(char.isspace() or char in "()\\" for char in delimiter):
                        raw_open = candidate_open

            if raw_open >= 0:
                output.extend(" " * (raw_open - i + 1))
                raw_terminator = ")" + text[i + 2 : raw_open] + '"'
                state = "raw-string"
                i = raw_open
            elif ch == '"':
                output.append(" ")
                state = "string"
                escaped = False
            elif ch == "'" and not (
                i > 0
                and i + 1 < len(text)
                and text[i - 1].isalnum()
                and text[i + 1].isalnum()
            ):
                output.append(" ")
                state = "character"
                escaped = False
            else:
                output.append(ch)
        i += 1

    return "".join(output)


def _strip_line_comment(line: str) -> str:
    """Return the code portion of a C++ line: everything before an
    unquoted ``//`` comment.

    The substring-matching checks below must not flag documentation that
    merely mentions a banned token (e.g. ``// routed via
    currentCrawlerWidget() historically``). A naive ``//`` split would
    mis-handle ``//`` inside a string literal, so this walks the line
    tracking whether we are inside a double-quoted string. Block comments
    (``/* */``) are intentionally not handled: the klogg tree does not use
    them on the lines these checks scan, and partial single-line handling
    would be less correct than leaving them to the (rare) multi-line case.
    """
    in_string = False
    escaped = False
    i = 0
    while i < len(line):
        ch = line[i]
        if escaped:
            escaped = False
        elif ch == "\\":
            escaped = True
        elif ch == '"':
            in_string = not in_string
        elif not in_string and ch == "/" and i + 1 < len(line) and line[i + 1] == "/":
            return line[:i]
        i += 1
    return line


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
    {
        "name": "private macro access override",
        "regex": re.compile(r"^\s*#\s*define\s+private\s+public\b"),
        "fix": (
            "Do not rewrite C++ access specifiers with a macro in tests. MSVC "
            "encodes access level in decorated symbols, so a test translation "
            "unit that sees a private method as public can fail to link against "
            "the implementation object. Add a narrow access_by<T> test adapter "
            "to the class instead."
        ),
    },
    {
        "name": "very-coarse-timer-in-test",
        # Qt::VeryCoarseTimer in a test file creates non-deterministic dispatch
        # delays on Qt 5.12 (Ubuntu 20.04).  Use PreciseTimer or direct calls.
        "regex": re.compile(r"\bQt::VeryCoarseTimer\b"),
        "fix": (
            "Qt::VeryCoarseTimer rounds up to ~1 s granularity on Qt 5.12 "
            "(Ubuntu 20.04), making UI dispatch non-deterministic. Use "
            "Qt::PreciseTimer in tests so qWait reliably processes the "
            "scheduled callback. (PR #43 master CI failure: flaky "
            "mainwindow_test.cpp folder-dispatch test.)"
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


def _extract_call_args(code: str, open_paren_pos: int) -> str:
    """Return the substring inside the parentheses starting at
    ``open_paren_pos``, balancing nested ``()``. Returns '' if unbalanced.

    Used by the qsizetype check so that ``.indexOf( QLatin1Char( '\\n' ), start )``
    is recognised as passing ``start`` (the nested QLatin1Char call would defeat
    a naive ``[^)]*`` capture).
    """
    masked = _strip_cpp_literals(_strip_cpp_comments(code))
    depth = 0
    start = open_paren_pos + 1
    for i in range(start, len(masked)):
        ch = masked[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            if depth == 0:
                return code[start:i]
            depth -= 1
    return ""


# Qt string/container APIs that take `int` on Qt 5 and `qsizetype` on Qt 6.
# Passing a raw qsizetype value narrows to int on Qt 5 -> -Werror=conversion.
# (QStringView is the exception: it has been qsizetype-native since Qt 5.8, so
# its indexOf/mid/left/right never narrow. QStringRef, like QString, takes int
# on Qt 5 and IS flagged.)
# `.at(i)` / `.value(i)` join the list: QString/QByteArray::at and value take
# int on Qt 5 (PR #56 CI failure: FolderSearchResults::readMarkLineSeek indexed
# QByteArray::at with a qsizetype loop counter).
#
# Two tiers (PR #56 review): indexOf/mid/left/right/truncate/chop/chopped
# are essentially only Qt string/sequence APIs, so they are checked on ANY
# receiver. But .at/.value/.remove also exist on non-narrowing containers
# (QHash::value, QMap::value, std::vector::at, and QSet/QMap/QHash/QCache::
# remove(const Key&) -- a qsizetype key passes without narrowing) where a
# qsizetype argument is correct on Qt 5, so those are only flagged when the
# receiver is a KNOWN int-indexed Qt type (collected below) -- otherwise
# legitimate QHash/QMap/std::vector use would be a false positive.
_QT_INT_ONLY_API_RE = re.compile(
    r"\.(?:indexOf|mid|left|right|truncate|chop|chopped)\s*\("
)
_QT_INT_AMBIGUOUS_API_RE = re.compile(r"\.(?:at|value|remove)\s*\(")
# Receivers whose .at/.value take int on Qt 5 (string/sequence containers).
_QT_INT_RECEIVER_TYPES = (
    "QString", "QStringRef", "QByteArray", "QStringList", "QVector", "QList",
    "QVarLengthArray", "QLatin1String",
)
_QSIZETYPE_DECL_RE = re.compile(r"\b(?:const\s+)?qsizetype\s+(\w+)\b")
# `using <Alias> = QStringView;` -- the wrappedstring.h code uses this idiom.
_QSTRINGVIEW_ALIAS_RE = re.compile(r"\busing\s+(\w+)\s*=\s*QStringView\s*;")
# A #if whose condition is Qt-6-only (so qsizetype code inside it is safe).
_QT6_IF_RE = re.compile(
    r"#\s*if\b.*\bQT_VERSION(?:_MAJOR)?\b\s*"
    r"(?:>=\s*(?:QT_VERSION_CHECK\s*\(\s*6\b|0x0*6[0-9A-Fa-f]{4,}|6\b)"
    r"|>\s*(?:QT_VERSION_CHECK\s*\(\s*5\b|0x0*5[0-9A-Fa-f]{4,}|5\b))"
)


def _qt6_guarded_lines(lines: list[str]) -> set[int]:
    """Return 1-based line numbers inside a Qt-6-only ``#if QT_VERSION >= 6``
    (or ``> 5``) branch, *excluding* its ``#else`` branch (which is Qt 5).

    qsizetype code behind such a guard only compiles on Qt 6, where these APIs
    take qsizetype, so it is not a -Werror=conversion risk. The Qt 5 ``#else``
    branch IS checked.
    """
    qt6: set[int] = set()
    # Each stack frame: {"qt6": bool, "in_else": bool}
    stack: list[dict] = []
    for i, line in enumerate(lines, start=1):
        if re.match(r"#\s*if(n?def)?\b", line):
            stack.append({"qt6": bool(_QT6_IF_RE.search(line)), "in_else": False})
        elif re.match(r"#\s*elif\b", line):
            # Conservatively treat the #elif branch as not the qt6 branch.
            if stack:
                stack[-1]["in_else"] = True
        elif re.match(r"#\s*else\b", line):
            if stack:
                stack[-1]["in_else"] = True
        elif re.match(r"#\s*endif\b", line):
            if stack:
                stack.pop()
        if any(s["qt6"] and not s["in_else"] for s in stack):
            qt6.add(i)
    return qt6


def _extract_receiver(code: str, dot_pos: int) -> str:
    """Return the receiver expression immediately before the '.' at dot_pos.

    For ``text.mid`` -> "text"; for ``QStringView{ line }.mid`` -> "QStringView";
    for ``QStringView(line).mid`` -> "QStringView". Used to tell QStringView
    receivers (qsizetype-native on Qt 5) from QString/QByteArray ones.
    """
    j = dot_pos - 1
    while j >= 0 and code[j].isspace():
        j -= 1
    if j < 0:
        return ""

    def _ident_before(pos: int) -> str:
        t = pos
        while t >= 0 and (code[t].isalnum() or code[t] == "_"):
            t -= 1
        return code[t + 1 : pos + 1]

    if code[j] in "})]":
        open_ch = {"}": "{", ")": "(", "]": "["}[code[j]]
        depth = 1
        k = j - 1
        while k >= 0 and depth > 0:
            if code[k] == code[j]:
                depth += 1
            elif code[k] == open_ch:
                depth -= 1
            k -= 1
        # k+1 is the matching opener; read the type identifier before it.
        t = k
        while t >= 0 and code[t].isspace():
            t -= 1
        return _ident_before(t) if t >= 0 else ""
    return _ident_before(j)


def _strip_literals(s: str) -> str:
    """Return ``s`` with char/string literal *contents* blanked so that an
    identifier search cannot match text inside them (e.g. the ``n`` in
    ``'\\n'`` must not be confused with a variable named ``n``).
    """
    out: list[str] = []
    i = 0
    n = len(s)
    while i < n:
        ch = s[i]
        if ch == "'" or ch == '"':
            quote = ch
            out.append(quote)
            j = i + 1
            while j < n:
                if s[j] == "\\" and j + 1 < n:
                    j += 2
                elif s[j] == quote:
                    out.append(quote)
                    j += 1
                    break
                else:
                    j += 1
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def _check_qmessagebox_in_tests(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag executable QMessageBox references under tests/.

    Modal message boxes block headless/offscreen test runs. Includes and text in
    comments or string literals are harmless and intentionally ignored.
    """
    if "tests" not in path.parts:
        return []

    findings: list[tuple[int, str]] = []
    source_lines = text.splitlines()
    code = _strip_cpp_literals(_strip_cpp_comments(text))
    for line_num, line in enumerate(code.splitlines(), start=1):
        if line_num <= len(source_lines) and ALLOW_MARKER in source_lines[line_num - 1]:
            continue
        if re.match(r"^\s*#\s*include\b", line):
            continue
        if re.search(r"\bQMessageBox\b", line):
            findings.append(
                (
                    line_num,
                    "Do not execute QMessageBox code in tests/: modal dialogs can "
                    "block indefinitely on headless/offscreen CI runners. Exercise "
                    "the underlying action or inject a non-modal test seam instead.",
                )
            )
    return findings


_WATCHDOG_LITERAL_RE = re.compile(
    r'"(?:\\.|[^"\\])*watchdog expired(?:\\.|[^"\\])*"'
)
_ZERO_DELAY_RE = re.compile(
    r"(?:0(?:[uUlL]*|ns|us|ms|s|min|h)"
    r"|std::chrono::(?:nano|micro|milli)?seconds[({]0[)}])"
)


def _first_call_argument(arguments: str) -> str:
    """Return the first top-level argument from a balanced call body."""
    masked = _strip_cpp_literals(_strip_cpp_comments(arguments))
    depth = 0
    angle_depth = 0
    previous_significant = ""
    for index, ch in enumerate(masked):
        if ch in "([{":
            depth += 1
        elif ch in ")]}" and depth > 0:
            depth -= 1
        elif ch == "<" and depth == 0 and (
            previous_significant.isalnum() or previous_significant in "_:>"
        ):
            angle_depth += 1
        elif ch == ">" and angle_depth > 0:
            angle_depth -= 1
        elif ch == "," and depth == 0 and angle_depth == 0:
            return arguments[:index]

        if not ch.isspace():
            previous_significant = ch
    return arguments


def _is_zero_timer_delay(expression: str) -> bool:
    compact = re.sub(r"[\s']", "", expression)
    if re.fullmatch(r"std::chrono::duration<[^>]+>[({]0[)}]", compact):
        return True
    return bool(_ZERO_DELAY_RE.fullmatch(compact))


def _check_nonzero_watchdog_timer(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag nonzero singleShot callbacks that implement wall-clock failures.

    Requiring the ``watchdog expired`` marker inside the timer call binds the
    diagnostic to one execution scope without guessing Catch macro boundaries.
    Zero-delay dispatch and unrelated timers remain allowed.
    """
    if "tests" not in path.parts:
        return []

    source_lines = text.splitlines()
    code = _strip_cpp_comments(text)
    findings: list[tuple[int, str]] = []
    timer_re = re.compile(r"\bQTimer\s*::\s*singleShot\s*\(")

    for timer_match in timer_re.finditer(code):
        arguments = _extract_call_args(code, timer_match.end() - 1)
        if not arguments or not _WATCHDOG_LITERAL_RE.search(arguments):
            continue
        delay = _first_call_argument(arguments)
        if _is_zero_timer_delay(delay):
            continue
        line_num = code[: timer_match.start()].count("\n") + 1
        if line_num <= len(source_lines) and ALLOW_MARKER in source_lines[line_num - 1]:
            continue
        findings.append(
            (
                line_num,
                "A nonzero QTimer::singleShot callback reports 'watchdog expired'. "
                "Wall-clock watchdogs make test success depend on CI runner speed; "
                "wait on a deterministic state/signal or invoke the dispatch path "
                "directly instead.",
            )
        )

    return findings


_PERFORMANCE_ASSERTION_RE = re.compile(
    r"\b(?:CHECK|REQUIRE)\s*\(\s*elapsedMs\s*<\s*"
    r"(?P<budget>(?:[1-9]\d{3,}|[A-Za-z_]\w*BudgetMs))\s*\)"
)
_CATCH_CASE_RE = re.compile(
    r"\b(?:TEST_CASE|SCENARIO|TEST_CASE_METHOD|TEMPLATE_TEST_CASE)\s*\("
)
_SANITIZER_EXCLUSION_RE = re.compile(
    r"^\s*#\s*(?:if\s+.*!\s*defined\s*\(\s*KLOGG_SANITIZER_BUILD\s*\)"
    r"|ifndef\s+KLOGG_SANITIZER_BUILD)"
)
_NDEBUG_DEFINED_RE = re.compile(r"defined\s*\(\s*NDEBUG\s*\)")
_NDEBUG_IFDEF_RE = re.compile(r"^\s*#\s*ifdef\s+NDEBUG\b")


def _requires_optimized_build(guard_line: str) -> bool:
    if _NDEBUG_IFDEF_RE.search(guard_line):
        return True
    for match in _NDEBUG_DEFINED_RE.finditer(guard_line):
        if not guard_line[: match.start()].rstrip().endswith("!"):
            return True
    return False


def _check_uninstrumented_performance_budget(
    text: str, path: Path
) -> list[tuple[int, str]]:
    """Require strict performance budgets to exclude instrumented builds.

    Absolute wall-clock limits in algorithmic performance tests are useful on
    optimized builds, but TSan/ASan, coverage, and Debug instrumentation distort
    those timings and make hosted-runner load decide whether CI passes. The
    correctness assertions still run everywhere; only the performance budget is
    gated.
    """
    if "tests" not in path.parts:
        return []

    source_lines = text.splitlines()
    code_lines = _strip_cpp_literals(_strip_cpp_comments(text)).splitlines()
    findings: list[tuple[int, str]] = []

    for line_num, line in enumerate(code_lines, start=1):
        if not _PERFORMANCE_ASSERTION_RE.search(line):
            continue
        if line_num <= len(source_lines) and ALLOW_MARKER in source_lines[line_num - 1]:
            continue

        case_start = 1
        for candidate in range(line_num, 0, -1):
            if _CATCH_CASE_RE.search(source_lines[candidate - 1]):
                case_start = candidate
                break

        case_header = " ".join(
            source_lines[case_start - 1 : min(line_num, case_start + 3)]
        )
        if "budget" not in case_header.lower():
            continue

        guard_lines = [
            _strip_line_comment(source_lines[index - 1])
            for index in range(case_start, line_num)
        ]
        excludes_sanitizers = any(
            _SANITIZER_EXCLUSION_RE.search(guard_line) for guard_line in guard_lines
        )
        requires_optimized = any(
            _requires_optimized_build(guard_line) for guard_line in guard_lines
        )
        if excludes_sanitizers and requires_optimized:
            continue

        findings.append(
            (
                line_num,
                "Strict wall-clock performance budgets must run only in optimized "
                "non-sanitized builds. TSan/ASan, coverage, and Debug instrumentation "
                "make hosted-runner speed part of the result. Keep correctness checks "
                "on every build, and guard the timing assertion with "
                "#if !defined(KLOGG_SANITIZER_BUILD) && defined(NDEBUG).",
            )
        )

    return findings


def _check_qsizetype_to_int_conversion(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag a `qsizetype`-typed variable passed as an argument to a Qt
    string/container API that takes `int` on Qt 5.

    On Qt 5 (Ubuntu 20.04 / 22.04 CI), QString/QStringRef/QByteArray::indexOf,
    mid, left, right, truncate, chop, ... take `int`; on Qt 6 they take
    `qsizetype`. A value held in a raw `qsizetype` variable therefore narrows to
    `int` on Qt 5 and trips -Werror=conversion -- but the narrowing is invisible
    on the macOS / Ubuntu-24.04 Qt 6 builds developers run locally, so it only
    surfaces in the Linux CI matrix. PR #48 failed CI this way in
    FolderSearchResults::ensureMarkLines (qsizetype loop indices passed to
    QString::indexOf / QString::mid); PR #56 failed the same way with a
    qsizetype loop counter passed to QByteArray::at.

    QStringView is excluded: it has been qsizetype-native since Qt 5.8, so its
    indexOf/mid/left/right do not narrow on Qt 5. Code behind a Qt-6-only
    ``#if QT_VERSION >= 6`` guard is also excluded. The adaptive klogg types
    (LineLength / LineColumn, whose UnderlyingType is decltype(QString::size()))
    and an explicit static_cast<int>(...) with a documented size bound are the
    portable alternatives -- both are already used in-tree. `auto` deduced from
    QString::size()/indexOf() is also safe (it tracks the Qt version), so only
    explicit `qsizetype` declarations are flags.
    """
    findings: list[tuple[int, str]] = []
    lines = text.splitlines()
    qt6_lines = _qt6_guarded_lines(lines)

    # Collect every identifier declared `qsizetype` OUTSIDE Qt-6-only guards
    # (inside the guard, qsizetype only compiles on Qt 6 where it is safe).
    declared: set[str] = set()
    for i, line in enumerate(lines, start=1):
        if i in qt6_lines:
            continue
        for m in _QSIZETYPE_DECL_RE.finditer(_strip_line_comment(line)):
            declared.add(m.group(1))
    if not declared:
        return findings

    # QStringView type names: the real one plus `using X = QStringView;` aliases
    # (wrappedstring.h uses `using WrappedStringPart = QStringView;`).
    qstrview_types: set[str] = {"QStringView"}
    for line in lines:
        m = _QSTRINGVIEW_ALIAS_RE.search(line)
        if m:
            qstrview_types.add(m.group(1))
    # Variables of a QStringView type (incl. aliases): `<Type> <ident>` or
    # `<Type> <ident>(...)` constructions. Receivers matching these are safe.
    qstrview_vars: set[str] = set()
    type_alt = "|".join(re.escape(t) for t in qstrview_types)
    var_decl_re = re.compile(rf"\b(?:const\s+)?({type_alt})\s*[&*]?\s*(\w+)\b")
    for line in lines:
        code = _strip_line_comment(line)
        for m in var_decl_re.finditer(code):
            qstrview_vars.add(m.group(2))

    # Variables of a KNOWN int-indexed Qt string/sequence type. `.at/.value`
    # are only flagged on these receivers (QHash/QMap::value and std::vector::at
    # take a key/size_type that legitimately accepts qsizetype on Qt 5).
    # Longest-first alternation: otherwise "QString" wins over "QStringList"
    # and the captured "variable" becomes "List". The optional template-argument
    # group admits QVector<int>/QList<T>/QVarLengthArray<char, 256> declarations
    # (the character class excludes statement/block delimiters, so a single
    # backtracking pass handles nested templates and C++11 ">>" closers).
    int_receiver_vars: set[str] = set()
    int_type_alt = "|".join(
        re.escape(t) for t in sorted(_QT_INT_RECEIVER_TYPES, key=len, reverse=True)
    )
    int_var_decl_re = re.compile(
        rf"\b(?:const\s+)?(?:{int_type_alt})"
        rf"\s*(?:<[^;(){{}}]*>)?"
        rf"\s*[&*]?\s*(\w+)\b"
    )
    for line in lines:
        code = _strip_line_comment(line)
        for m in int_var_decl_re.finditer(code):
            int_receiver_vars.add(m.group(1))

    def report(line_no: int, name: str, receiver: str, args: str) -> None:
        findings.append(
            (
                line_no,
                f"qsizetype variable '{name}' is passed to a Qt "
                f"string API (.indexOf/.mid/.left/...) that takes "
                f"`int` on Qt 5 (receiver '{receiver}' is not a "
                f"QStringView). This narrows qsizetype->int and "
                f"trips -Werror=conversion on the Qt 5 Linux CI "
                f"builds (invisible on Qt 6 / macOS). Use the "
                f"adaptive klogg::ContainerIndex (containers.h: "
                f"int on Qt 5 / qsizetype on Qt 6, never narrows), "
                f"LineLength/LineColumn for line semantics, or "
                f"static_cast<int>(...) with a documented size "
                f"bound. (PR #48/#56 CI failures.)",
            )
        )

    for i, line in enumerate(lines, start=1):
        if i in qt6_lines or ALLOW_MARKER in line:
            continue
        code = _strip_line_comment(line)
        # (regex, require_known_int_receiver) pairs. indexOf/mid/... run on any
        # receiver; at/value only on a known int-indexed Qt receiver.
        for api_re, needs_int_receiver in (
            (_QT_INT_ONLY_API_RE, False),
            (_QT_INT_AMBIGUOUS_API_RE, True),
        ):
            for m in api_re.finditer(code):
                args = _extract_call_args(code, m.end() - 1)
                if not args:
                    continue
                receiver = _extract_receiver(code, m.start())
                # QStringView receiver: qsizetype-native on Qt 5, no narrowing.
                if receiver in qstrview_types or receiver in qstrview_vars:
                    continue
                if needs_int_receiver and receiver not in int_receiver_vars:
                    continue
                for name in declared:
                    if re.search(r"\b" + re.escape(name) + r"\b", _strip_literals(args)):
                        report(i, name, receiver, args)
                        break  # one report per call is enough
                if findings and findings[-1][0] == i:
                    break  # one report per line is enough
    return findings


def _check_qstringlist_brace_assignment(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag assignment of a raw braced list to an existing QStringList.

    Qt 6 accepts ``names = { ... }``, but Qt 5 exposes assignment overloads for
    both QStringList and QList<QString>; the untyped initializer list cannot
    choose between them. This passed the local Qt 6 build and failed every Qt 5
    CI leg in PR #50. Construct an explicit ``QStringList{ ... }`` temporary so
    overload resolution is portable.
    """
    lines = text.splitlines()
    declared: set[str] = set()
    declaration_re = re.compile(r"\bQStringList\s+(\w+)\s*;")

    for line in lines:
        for match in declaration_re.finditer(_strip_line_comment(line)):
            declared.add(match.group(1))

    if not declared:
        return []

    findings: list[tuple[int, str]] = []
    for line_num, line in enumerate(lines, start=1):
        if ALLOW_MARKER in line:
            continue
        code = _strip_line_comment(line)
        for name in declared:
            if re.search(rf"\b{re.escape(name)}\s*=\s*\{{", code):
                findings.append(
                    (
                        line_num,
                        f"QStringList variable '{name}' is assigned a raw braced "
                        "initializer. Qt 5 cannot choose between QStringList and "
                        "QList<QString> assignment overloads, although Qt 6 accepts "
                        "the code. Assign an explicit QStringList{ ... } temporary "
                        "instead. (PR #50 Qt 5 matrix compile failure.)",
                    )
                )
                break

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


def _check_test_private_current_crawler(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag test files that call MainWindow::currentCrawlerWidget() directly.

    currentCrawlerWidget() is private — tests cannot access it. The CI
    build (GCC 13, -Werror) rejects this as a hard error even if a
    developer's local build happens to succeed.  Use the public API
    path instead:
        qobject_cast<CrawlerWidget*>(tabArea->currentWidget()).
    """
    if "test" not in path.name or ALLOW_MARKER in text:
        return []

    findings: list[tuple[int, str]] = []
    for i, line in enumerate(text.splitlines(), start=1):
        # Skip comments so doc text mentioning the private API does not
        # false-positive (e.g. "// routed via currentCrawlerWidget() ...").
        if "currentCrawlerWidget()" in _strip_line_comment(line):
            findings.append(
                (
                    i,
                    "currentCrawlerWidget() is private in MainWindow. "
                    "Use qobject_cast<CrawlerWidget*>(tabArea->currentWidget()) "
                    "instead. The CI build (GCC 13, -Werror) rejects direct access "
                    "as a hard compilation error.",
                )
            )
    return findings


def _check_close_without_loading_wait(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag mainwindow tests that trigger tab-close after file-load without
    an intervening ``isFirstLoadDone`` wait.

    The test calls loadInitialFile / loadFile and then triggers a
    destructive close action on the resulting tab.  If the only waits
    are for UI surface signals (tab count, path label) the file-load may
    still be in progress on slower runners (especially Windows x86).
    Closing a tab while the background loading thread holds references
    to heap allocations causes corruption in the loader's simdutf layer
    and a downstream free() SIGSEGV.

    The fix is a waitUiState on CrawlerWidget::isFirstLoadDone() before
    any close, reload, or overlap-sensitive action.
    """
    if path.name != "mainwindow_test.cpp" or ALLOW_MARKER in text:
        return []

    has_load = any(
        "loadInitialFile" in line or "loadFile(" in line
        for line in text.splitlines()
    )
    has_close = any(
        "closeAction" in line or "->close()" in line
        or "closeTab(" in line or "closeAll(" in line
        for line in text.splitlines()
    )
    if not (has_load and has_close):
        return []

    if "isFirstLoadDone" in text:
        return []

    return [
        (
            1,
            "mainwindow_test.cpp triggers a destructive close action after "
            "loadInitialFile/loadFile without waiting for isFirstLoadDone(). "
            "Add waitUiState([&]{ return "
            "qobject_cast<CrawlerWidget*>(tabArea->currentWidget())->isFirstLoadDone(); }) "
            "before any close or reload in load-then-close scenarios.  "
            "Without this, background loading threads can hold heap "
            "references during teardown, causing SIGSEGV on slower CI "
            "runners (esp. Windows x86).",
        )
    ]


def _check_qt5_arg_limit(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag QString::arg() calls with 10+ arguments, exceeding Qt5's 9-value limit.

    Qt6 supports higher-arity QString::arg overloads, so multi-argument calls
    compile fine on macOS (Qt6 via Homebrew) but fail on Linux CI jobs that
    still use Qt5 (e.g. ubuntu-20.04 AppImage).

    PR #28 exposed this in issuereporter.cpp.
    """
    if ALLOW_MARKER in text:
        return []

    findings: list[tuple[int, str]] = []

    # Walk through text finding .arg( calls, then count top-level arguments
    # (commas not nested inside parens/brackets/braces) until the matching ).
    import re as _re

    arg_call_re = _re.compile(r"\.arg\s*\(")
    for m in arg_call_re.finditer(text):
        start = m.end()  # position right after .arg(
        # Find matching closing paren, tracking nesting of () [] {}
        depth = 1
        pos = start
        while pos < len(text) and depth > 0:
            ch = text[pos]
            if ch == "(" or ch == "[" or ch == "{":
                depth += 1
            elif ch == ")" or ch == "]" or ch == "}":
                depth -= 1
            pos += 1
        if depth != 0:
            continue  # malformed — skip

        arg_text = text[start : pos - 1]  # text between ( and matching )

        # Count top-level commas.
        comma_count = 0
        nest = 0
        for ch in arg_text:
            if ch == "(" or ch == "[" or ch == "{":
                nest += 1
            elif ch == ")" or ch == "]" or ch == "}":
                nest -= 1
            elif ch == "," and nest == 0:
                comma_count += 1

        # comma_count of N means N+1 arguments.  Qt5 limit is 9 args → 8 commas.
        if comma_count < 9:
            continue

        # Find the source line number for this call.
        line_num = text[: m.start()].count("\n") + 1

        findings.append(
            (
                line_num,
                f"QString::arg() call with {comma_count + 1} arguments exceeds "
                f"Qt5's 9-value limit. Split into multiple .arg() calls or "
                f"use separate template strings to stay within the limit. "
                f"(PR #28: issuereporter.cpp AppImage build failure.)",
            )
        )

    return findings


def _check_vectorscan_capability_assertion(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag test assertions that unconditionally REQUIRE hasBufferScan().

    PatternMatcher::hasBufferScan() is compiled to ``return false`` on
    KLOGG_USE_VECTORSCAN=OFF builds -- e.g. the Windows x86-qt5 [QTRegex]
    CI job -- so an unguarded REQUIRE passes on every vectorscan-enabled
    dev machine (macOS / Linux / Windows x64) and fails only there.
    PR #42 broke exactly that job with
    ``REQUIRE( matcher->hasBufferScan() )`` in foldersearchengine_test.cpp.

    Gate the test on the capability instead of asserting it: early-return
    (vacuous pass) when the matcher has no buffer scanner, or guard on
    ``config.regexpEngine() != RegexpEngine::Vectorscan`` -- the same
    contract as patternmatcher_test's block-scan parity test. A REQUIRE
    preceded by either guard inside the same TEST_CASE is accepted.
    """
    if "test" not in path.name or ALLOW_MARKER in text:
        return []

    findings: list[tuple[int, str]] = []

    assertion_re = re.compile(r"\b(?:REQUIRE|CHECK)\s*\([^;]*hasBufferScan\s*\(")
    # Guards that make the assertion reachable only when block scan exists:
    #   if ( !matcher->hasBufferScan() ) { return; }
    #   if ( config.regexpEngine() != RegexpEngine::Vectorscan ) { return; }
    guard_re = re.compile(
        r"!\s*\w+\s*->\s*hasBufferScan\s*\("
        r"|regexpEngine\s*\(\s*\)\s*!=\s*RegexpEngine::Vectorscan"
    )

    lines = text.splitlines()
    for i, line in enumerate(lines, start=1):
        # Skip comments so doc text mentioning the macro does not
        # false-positive (e.g. "// do not REQUIRE( hasBufferScan() ) ...").
        if not assertion_re.search(_strip_line_comment(line)):
            continue
        # Find the start of the enclosing TEST_CASE (1 = file scope, so the
        # backward scan starts at line 1 and never indexes lines[-1]).
        case_start = 1
        for j in range(i - 1, 0, -1):
            if "TEST_CASE(" in lines[j - 1]:
                case_start = j
                break
        guarded = any(
            guard_re.search(_strip_line_comment(lines[k - 1])) for k in range(case_start, i)
        )
        if not guarded:
            findings.append(
                (
                    i,
                    "Do not REQUIRE/CHECK matcher->hasBufferScan() unconditionally: "
                    "it is always false on KLOGG_USE_VECTORSCAN=OFF builds "
                    "(Windows x86-qt5 [QTRegex] CI job). Early-return with a "
                    "vacuous pass when the capability is absent instead "
                    "(PR #42).",
                )
            )

    return findings


def _check_data_variable_shadowing(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag local variables named ``data`` inside QWidget subclass methods.

    QWidget has a protected member ``data`` (QScopedPointer<QWidgetData>).
    GCC/Clang ``-Wshadow`` does NOT warn about shadowing protected members,
    but MSVC C4458 (enabled by /W4, promoted to error by /WX) does.  This
    causes Windows-only CI failures that pass on macOS/Linux.

    PR #38 exposed this in AdbLogcatDialog::sessionData() and
    IosLogDialog::sessionData().

    The check looks for:
    - Files whose name contains "dialog" (heuristic for QDialog subclasses).
    - A local variable declaration ``Type data;`` or ``Type data{}``.
    """
    if "dialog" not in path.name.lower() and "widget" not in path.name.lower():
        return []

    # Only flag if the file actually includes QWidget/QDialog headers
    # or inherits from QDialog — avoids false positives in non-Qt files.
    if "QDialog" not in text and "QWidget" not in text:
        return []

    findings: list[tuple[int, str]] = []
    # Match: TypeIdentifier data;  or  TypeIdentifier data{...}
    # Captures patterns like:  AdbLogcatSessionData data;
    decl_re = re.compile(
        r"^\s+\w+(?:::\w+)*\s+data\s*[;={]"
    )
    for i, line in enumerate(text.splitlines(), start=1):
        if ALLOW_MARKER in line:
            continue
        if decl_re.match(line):
            findings.append(
                (
                    i,
                    "Local variable named 'data' shadows QWidget::data "
                    "(protected member). GCC/Clang -Wshadow does NOT catch "
                    "this, but MSVC C4458 does (promoted to error by /WX). "
                    "Rename the variable (e.g. 'sessionData') to avoid a "
                    "Windows-only CI failure. (PR #38)",
                )
            )
    return findings


def _check_hardcoded_text_viewport_row_assertion(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag text viewport height assertions that assume fixed font metrics.

    PR #26 exposed this in the offscreen UI tests: a resize that produced a
    text viewport taller than three rows locally produced only 59 pixels on
    Windows Qt5 and older Linux containers, so
    ``TextViewportHeight() > CharHeight() * 3`` failed in CI. Size the view
    from runtime metrics instead of asserting that one fixed pixel resize has
    a fixed row capacity.
    """
    if path.name != "crawlerwidget_test.cpp" or ALLOW_MARKER in text:
        return []

    row_assertion_re = re.compile(
        r"TextViewportHeight\s*\(\s*\)\s*>\s*.*CharHeight\s*\(\s*\)\s*\*\s*\d+"
    )

    findings: list[tuple[int, str]] = []
    for i, line in enumerate(text.splitlines(), start=1):
        if row_assertion_re.search(line):
            findings.append(
                (
                    i,
                    "Text viewport row-count assertions based on a fixed resize "
                    "are platform-fragile because Qt font/style metrics differ "
                    "across CI runners. Resize until the runtime viewport metrics "
                    "satisfy the row precondition, then assert the behavior under test.",
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


_QT_VERSION_GUARD_RE = re.compile(
    r"#\s*(?:if|elif)\b[^\n]*\bQT_VERSION(?:_CHECK|_MAJOR)?\b"
)


def _check_qt_version_macro_in_tests(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag Qt-version preprocessor conditionals inside tests/.

    Test (and business) code must not open-code Qt version splits: PR #57's
    QWheelEvent constructor guard compiled on the developer's Qt 6 macOS build
    but failed every Qt 5.15 CI leg with -Werror=deprecated-declarations
    (Qt 5.12 only has the qt4Delta overloads, 5.14 added the new constructor,
    5.15 deprecates the old ones, Qt 6 removes them). Version/API splits belong
    in the platform abstraction layer (src/utils/include/platform/, e.g.
    klogg::platform::makeWheelEvent); tests should express pure intent.
    """
    if "tests" not in path.parts or ALLOW_MARKER in text:
        return []

    findings: list[tuple[int, str]] = []
    for i, line in enumerate(text.splitlines(), start=1):
        if _QT_VERSION_GUARD_RE.search(_strip_line_comment(line)):
            findings.append(
                (
                    i,
                    "Qt-version preprocessor guards are not allowed in tests/: "
                    "the split belongs in the platform abstraction layer "
                    "(src/utils/include/platform/, e.g. "
                    "klogg::platform::makeWheelEvent). A guard open-coded in a "
                    "test compiled on Qt 6 but failed every Qt 5.15 CI leg with "
                    "-Werror=deprecated-declarations (PR #57).",
                )
            )
    return findings


MULTI_LINE_CHECKS: list[dict] = [
    {
        "name": "unguarded-platform-helper",
        "check": _check_unguarded_platform_helper,
    },
    {
        "name": "main-view-text-pixel-probe",
        "check": _check_main_view_text_pixel_probe,
    },
    {
        "name": "test-private-current-crawler",
        "check": _check_test_private_current_crawler,
    },
    {
        "name": "close-without-loading-wait",
        "check": _check_close_without_loading_wait,
    },
    {
        "name": "hardcoded-text-viewport-row-assertion",
        "check": _check_hardcoded_text_viewport_row_assertion,
    },
    {
        "name": "qt5-string-arg-limit",
        "check": _check_qt5_arg_limit,
    },
    {
        "name": "vectorscan-capability-assertion",
        "check": _check_vectorscan_capability_assertion,
    },
    {
        "name": "data-variable-shadowing",
        "check": _check_data_variable_shadowing,
    },
    {
        "name": "qsizetype-to-qt-int-api",
        "check": _check_qsizetype_to_int_conversion,
    },
    {
        "name": "qstringlist-brace-assignment",
        "check": _check_qstringlist_brace_assignment,
    },
    {
        "name": "qt-version-macro-in-tests",
        "check": _check_qt_version_macro_in_tests,
    },
    {
        "name": "qmessagebox-in-tests",
        "check": _check_qmessagebox_in_tests,
    },
    {
        "name": "nonzero-watchdog-timer",
        "check": _check_nonzero_watchdog_timer,
    },
    {
        "name": "uninstrumented-performance-budget",
        "check": _check_uninstrumented_performance_budget,
    },
]


def iter_target_files(paths: Iterable[Path]) -> Iterable[Path]:
    for root in paths:
        if not root.exists():
            continue
        for ext in ("*.cpp", "*.h", "*.hpp", "*.cc", "*.yml", "*.yaml"):
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
        if raw.endswith((".cpp", ".h", ".hpp", ".cc", ".yml", ".yaml")):
            yield Path(raw)


def _check_ci_tarball_into_build_root(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag workflow/action YAML that writes package tarballs into
    ``$KLOGG_BUILD_ROOT``.

    PR #54's package-upload tarball step wrote the archive into the build root
    and failed on every Linux leg with ``tar: ... Cannot open: Permission
    denied``. On Linux the build root is created by docker as root (build and
    packaging steps run in containers with the workspace mounted at
    ``/usr/local``), so the host runner cannot create files inside it. Upload
    tarballs must be written to ``$KLOGG_WORKSPACE``, which the runner owns —
    the ``cpm-cache.tar.gz`` prefetch already followed that rule.

    Two manifestations are caught, both zero-false-positive on the current
    tree:
      1. An upload-artifact ``path:`` (or any path line) referencing a
         ``.tar.gz`` under ``KLOGG_BUILD_ROOT``.
      2. A host step that ``cd "$KLOGG_BUILD_ROOT"`` and then ``tar -czf``s a
         file into it (the write lands in the root-owned build root).
    Reading from the build root (e.g. ``tar ... -C "$KLOGG_BUILD_ROOT/packages"``)
    is fine and is not flagged.
    """
    findings: list[tuple[int, str]] = []
    lines = text.splitlines()

    for i, line in enumerate(lines, start=1):
        if "path:" in line and "KLOGG_BUILD_ROOT" in line and ".tar.gz" in line:
            findings.append(
                (
                    i,
                    "package tarball referenced under $KLOGG_BUILD_ROOT; that "
                    "directory is root-owned on Linux (docker-created), so the "
                    "host runner cannot write there. Write/upload tarballs "
                    "from $KLOGG_WORKSPACE instead.",
                )
            )
        if 'cd "$KLOGG_BUILD_ROOT"' in line:
            for j in range(i, min(i + 15, len(lines) + 1)):
                if "tar -czf" in lines[j - 1]:
                    findings.append(
                        (
                            i,
                            '`cd "$KLOGG_BUILD_ROOT"` followed by `tar -czf` '
                            "writes the archive into the root-owned build root "
                            "on Linux (docker-created); the host runner cannot "
                            "create files there. Write the tarball into "
                            "$KLOGG_WORKSPACE instead.",
                        )
                    )
                    break
    return findings


def _check_unfiltered_cross_run_artifact_download(text: str, path: Path) -> list[tuple[int, str]]:
    """Flag workflow YAML steps that download artifacts from another workflow
    run without restricting which artifacts they fetch.

    Release run 31241902650 failed with "Invalid or unsupported zip format.
    No END header found" because ci-release.yml used
    dawidd6/action-download-artifact with no ``name`` filter, so it pulled
    every artifact of the source CI run. Since 1ddee61d added docker
    ``type=gha`` layer caching, docker/build-push-action also uploads a
    ``<owner>~<repo>~<id>.dockerbuild`` build-record artifact per build leg;
    those are gzipped OCI tarballs, not zips, and dawidd6 aborts when its
    unzip step reaches one.

    Any cross-run download step (``workflow:`` / ``run_id:`` inputs — i.e. it
    can see artifacts produced by OTHER jobs, not just the current one) must
    set an explicit artifact selector:
      - dawidd6/action-download-artifact: ``name:`` (optionally with
        ``name_is_regexp: true``)
      - actions/download-artifact@v4: ``name:`` or ``pattern:``
    """
    findings: list[tuple[int, str]] = []
    lines = text.splitlines()

    i = 0
    while i < len(lines):
        line = lines[i]
        if "uses:" in line and (
            "dawidd6/action-download-artifact" in line
            or "actions/download-artifact" in line
        ):
            # Scan the step block (until the next step or a dedent to column 0)
            # for a cross-run marker and for an artifact selector.
            block: list[str] = []
            j = i + 1
            while j < len(lines) and not (
                lines[j].lstrip().startswith("- ")
                and (len(lines[j]) - len(lines[j].lstrip())) <= 6
            ):
                block.append(lines[j])
                j += 1
            joined = "\n".join(block)
            cross_run = "workflow:" in joined or "run_id:" in joined
            has_selector = any(
                b.lstrip().startswith(("name:", "pattern:")) for b in block
            )
            if cross_run and not has_selector:
                findings.append(
                    (
                        i + 1,
                        "cross-run artifact download without a name:/pattern: "
                        "filter fetches every artifact of the source run, "
                        "including docker/build-push-action *.dockerbuild "
                        "build records (not zips), which breaks the unzip "
                        "step (release run 31241902650). Add a restrictive "
                        "name:/pattern: filter.",
                    )
                )
            i = j
        else:
            i += 1
    return findings


def lint_file(path: Path) -> int:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return 0
    issues = 0

    if path.suffix in (".yml", ".yaml"):
        # CI workflow/action YAML has its own platform-fragile class: package
        # uploads must not write into the docker-root-owned build root, and
        # cross-run artifact downloads must be name-filtered.
        for rule_name, check in (
            ("ci-tarball-into-build-root", _check_ci_tarball_into_build_root),
            (
                "unfiltered-cross-run-artifact-download",
                _check_unfiltered_cross_run_artifact_download,
            ),
        ):
            for line_num, message in check(text, path):
                print(f"[platform-fragile] {rule_name}")
                print(f"  at {path}:{line_num}")
                print(f"  {message}")
                print()
                issues += 1
        return issues

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
        default=["src", "tests", "benchmarks", ".github"],
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
