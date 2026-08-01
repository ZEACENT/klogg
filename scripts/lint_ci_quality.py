#!/usr/bin/env python3
"""Reject CI patterns that silently weaken quality gates."""

from __future__ import annotations

import fnmatch
import math
import re
import shlex
import sys
from pathlib import Path


CHECKOUT_SHA_RE = re.compile(r"actions/checkout@[0-9a-f]{40}")
CODEQL_ACTION_SHA_RE = re.compile(
    r"github/codeql-action/(?P<action>init|analyze)@(?P<sha>[0-9a-f]{40})"
)
LIST_ITEM_RE = re.compile(r"^(?P<indent>\s*)-(?:\s+|$)")
KEY_VALUE_RE = re.compile(r"^(?P<indent>\s*)(?:-\s*)?(?P<key>[\w-]+):\s*(?P<value>.*)$")
BROAD_SUPPRESSION_RE = re.compile(
    r"^(?:race|leak):.*\*(?:Qt|tbb|mimalloc|glib)\*", re.MULTILINE
)
BROAD_CPPCHECK_IDS = (
    "functionStatic",
    "noExplicitConstructor",
    "returnByReference",
    "constParameterPointer",
    "constParameterReference",
    "constVariable",
    "shadowFunction",
    "variableScope",
    "unreadVariable",
    "assignBoolToFloat",
    "useStlAlgorithm",
    "useInitializationList",
    "missingOverride",
)


def strip_yaml_comment(value: str) -> str:
    quote: str | None = None
    for index, char in enumerate(value):
        if char in "'\"":
            if quote is None:
                quote = char
            elif quote == char:
                quote = None
        elif char == "#" and quote is None and (index == 0 or value[index - 1].isspace()):
            return value[:index].rstrip()
    return value.strip()


def scalar(value: str) -> str:
    value = strip_yaml_comment(value).strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
        return value[1:-1]
    return value


def yaml_value(block: list[str], offset: int, value: str, indent: int) -> str:
    candidate = scalar(value)
    if candidate not in ("|", "|-", "|+", ">", ">-", ">+"):
        return candidate

    content: list[str] = []
    for following in block[offset + 1 :]:
        if following.strip():
            following_indent = len(following) - len(following.lstrip())
            if following_indent <= indent:
                break
            content.append(following.strip())
        else:
            content.append("")
    separator = " " if candidate.startswith(">") else "\n"
    return separator.join(content).strip()


def workflow_run_blocks(text: str) -> list[str]:
    lines = text.splitlines()
    blocks: list[str] = []
    for index, line in enumerate(lines):
        entry = KEY_VALUE_RE.match(line)
        if entry is None or entry.group("key") != "run":
            continue
        indent = len(entry.group("indent"))
        value = yaml_value(lines, index, entry.group("value"), indent)
        active_lines = []
        for run_line in value.splitlines():
            active = strip_yaml_comment(run_line)
            if active:
                active_lines.append(active)
        blocks.append("\n".join(active_lines))
    return blocks


def has_open_shell_quote(value: str) -> bool:
    quote: str | None = None
    escaped = False
    for character in value:
        if escaped:
            escaped = False
            continue
        if quote == "'":
            if character == "'":
                quote = None
            continue
        if character == "\\":
            escaped = True
            continue
        if quote == '"':
            if character == '"':
                quote = None
            continue
        if character in "'\"":
            quote = character
    return quote is not None


def shell_commands(text: str) -> list[str]:
    commands: list[str] = []
    for block in workflow_run_blocks(text):
        continued: list[str] = []
        for line in block.splitlines():
            fragment = line.strip()
            line_continues = fragment.endswith("\\")
            if line_continues:
                fragment = fragment[:-1].rstrip()
            continued.append(fragment)
            command = " ".join(part for part in continued if part)
            if line_continues or has_open_shell_quote(command):
                continue
            if command:
                commands.append(command)
            continued = []
        if continued:
            commands.append(" ".join(part for part in continued if part))
    return commands


def shell_tokens(command: str) -> list[str]:
    try:
        return shlex.split(command)
    except ValueError:
        return []


def xargs_bash_script_index(tokens: list[str]) -> int | None:
    if not tokens or tokens[0] != "xargs":
        return None

    seen_null = False
    seen_max_args = False
    seen_parallel = False
    index = 1
    while index < len(tokens):
        token = tokens[index]
        if token == "-0" and not seen_null:
            seen_null = True
            index += 1
        elif token == "-n" and not seen_max_args and index + 1 < len(tokens):
            if tokens[index + 1] != "1":
                return None
            seen_max_args = True
            index += 2
        elif token == "-P" and not seen_parallel and index + 1 < len(tokens):
            seen_parallel = True
            index += 2
        elif token == "bash":
            if (
                not seen_null
                or not seen_max_args
                or index + 1 >= len(tokens)
                or tokens[index + 1] != "-c"
            ):
                return None
            return index + 2
        else:
            return None
    return None


def clang_tidy_consumes_xargs_argument(script: str) -> bool:
    tokens = shell_tokens(script)
    if not tokens or tokens[0] != "clang-tidy":
        return False
    for control in (";", "&&", "||", "\n"):
        if control in tokens:
            tokens = tokens[: tokens.index(control)]
    return bool(tokens) and tokens[-1] in {"$1", "${1}"}


def cmake_cache_assignments(tokens: list[str], name: str) -> list[str]:
    assignments: list[str] = []
    for index, token in enumerate(tokens):
        candidate = None
        if token == "-D" and index + 1 < len(tokens):
            candidate = tokens[index + 1]
        elif token.startswith("-D"):
            candidate = token[2:]
        if candidate is None:
            continue
        match = re.fullmatch(rf"{re.escape(name)}(?::[^=]+)?=(.*)", candidate)
        if match is not None:
            assignments.append(match.group(1).upper())
    return assignments


def cmake_unsets_cache_entry(tokens: list[str], name: str) -> bool:
    for index, token in enumerate(tokens):
        pattern = None
        if token == "-U" and index + 1 < len(tokens):
            pattern = tokens[index + 1]
        elif token.startswith("-U"):
            pattern = token[2:]
        if pattern and fnmatch.fnmatchcase(name, pattern):
            return True
    return False


def option_value(tokens: list[str], option: str) -> str | None:
    try:
        index = tokens.index(option)
    except ValueError:
        return None
    return tokens[index + 1] if index + 1 < len(tokens) else None


def unique_option_value(tokens: list[str], option: str) -> str | None:
    if tokens.count(option) != 1:
        return None
    return option_value(tokens, option)


def unique_cmake_option_value(tokens: list[str], option: str) -> str | None:
    values: list[str] = []
    for index, token in enumerate(tokens):
        if token == option and index + 1 < len(tokens):
            values.append(tokens[index + 1])
        elif token.startswith(option) and token != option:
            value = token[len(option) :]
            values.append(value[1:] if value.startswith("=") else value)
    return values[0] if len(values) == 1 else None


def checkout_steps(text: str) -> list[tuple[int, str, str | None]]:
    lines = text.splitlines()
    checkouts: list[tuple[int, str, str | None]] = []
    for index, line in enumerate(lines):
        item = LIST_ITEM_RE.match(line)
        if item is None:
            continue
        item_indent = len(item.group("indent"))
        end = len(lines)
        for following in range(index + 1, len(lines)):
            next_item = LIST_ITEM_RE.match(lines[following])
            if next_item is not None and len(next_item.group("indent")) <= item_indent:
                end = following
                break

        block = lines[index:end]
        entries: list[tuple[int, str, str, int]] = []
        for offset, block_line in enumerate(block):
            entry = KEY_VALUE_RE.match(block_line)
            if entry is not None:
                entries.append(
                    (
                        offset,
                        entry.group("key"),
                        entry.group("value"),
                        len(entry.group("indent")),
                    )
                )

        uses: str | None = None
        uses_line = index + 1
        for offset, key, value, entry_indent in entries:
            if key == "uses":
                candidate = yaml_value(block, offset, value, entry_indent)
                if candidate.startswith("actions/checkout@"):
                    uses = candidate
                    uses_line = index + offset + 1
                    break
        if uses is None:
            continue

        credentials: str | None = None
        with_entries = [entry for entry in entries if entry[1] == "with"]
        if with_entries:
            with_offset, _, _, with_indent = with_entries[0]
            mapping_entries: list[tuple[str, str, int]] = []
            for offset in range(with_offset + 1, len(block)):
                block_line = block[offset]
                stripped = block_line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                indent = len(block_line) - len(block_line.lstrip())
                if indent <= with_indent:
                    break
                entry = KEY_VALUE_RE.match(block_line)
                if entry is not None:
                    mapping_entries.append(
                        (entry.group("key"), entry.group("value"), indent)
                    )
            if mapping_entries:
                child_indent = min(entry[2] for entry in mapping_entries)
                for key, value, indent in mapping_entries:
                    if key == "persist-credentials" and indent == child_indent:
                        credentials = scalar(value).lower()
                        break

        checkouts.append((uses_line, uses, credentials))
    return checkouts


def check_checkout_blocks(path: Path, text: str) -> list[str]:
    issues: list[str] = []
    for line, uses, credentials in checkout_steps(text):
        if CHECKOUT_SHA_RE.fullmatch(uses) is None:
            issues.append(f"{path}:{line}: actions/checkout must use a reviewed 40-char SHA")
        if credentials != "false":
            issues.append(
                f"{path}:{line}: checkout must set with.persist-credentials to false"
            )
    return issues


def codeql_workflow_issues(text: str) -> list[str]:
    """Validate the fail-closed CodeQL job and its remote action revisions."""
    lines = text.splitlines()
    job_start: int | None = None
    job_indent = 0
    for index, line in enumerate(lines):
        match = re.match(r"^(?P<indent>\s*)analyze:\s*(?:#.*)?$", line)
        if match is not None:
            job_start = index
            job_indent = len(match.group("indent"))
            break

    if job_start is None:
        return ["CodeQL workflow must define the analyze job"]

    job_end = len(lines)
    for index in range(job_start + 1, len(lines)):
        line = lines[index]
        active = strip_yaml_comment(line)
        if not active:
            continue
        indent = len(line) - len(line.lstrip())
        if indent <= job_indent:
            job_end = index
            break

    job_lines = lines[job_start + 1 : job_end]
    direct_entries: list[tuple[str, str]] = []
    nested_entries: list[tuple[str, str]] = []
    child_indents = [
        len(line) - len(line.lstrip())
        for line in job_lines
        if strip_yaml_comment(line)
        and (len(line) - len(line.lstrip())) > job_indent
        and KEY_VALUE_RE.match(line) is not None
    ]
    direct_indent = min(child_indents) if child_indents else job_indent + 2

    for line in job_lines:
        entry = KEY_VALUE_RE.match(line)
        if entry is None:
            continue
        key = entry.group("key")
        value = scalar(entry.group("value"))
        indent = len(entry.group("indent"))
        nested_entries.append((key, value))
        if indent == direct_indent:
            direct_entries.append((key, value))

    issues: list[str] = []
    timeout_values = [value for key, value in direct_entries if key == "timeout-minutes"]
    if timeout_values != ["30"]:
        issues.append("CodeQL analyze job must set timeout-minutes: 30")

    if any(key == "continue-on-error" for key, _ in nested_entries):
        issues.append("CodeQL workflow must not use continue-on-error")

    action_refs: dict[str, list[str]] = {"init": [], "analyze": []}
    for key, value in nested_entries:
        if key != "uses" or not value.startswith("github/codeql-action/"):
            continue
        action = value.partition("github/codeql-action/")[2].partition("@")[0]
        if action in action_refs:
            action_refs[action].append(value)

    pinned_shas: dict[str, str] = {}
    for action in ("init", "analyze"):
        refs = action_refs[action]
        if len(refs) != 1:
            issues.append(f"CodeQL workflow must use exactly one {action} action")
            continue
        match = CODEQL_ACTION_SHA_RE.fullmatch(refs[0])
        if match is None:
            issues.append(f"CodeQL {action} must use a reviewed 40-char SHA")
            continue
        pinned_shas[action] = match.group("sha")

    if len(pinned_shas) == 2 and pinned_shas["init"] != pinned_shas["analyze"]:
        issues.append("CodeQL init and analyze must use the same reviewed SHA")

    return issues


def has_unsupported_macos_lsan(text: str) -> bool:
    return re.search(r"ASAN_OPTIONS=.*detect_leaks=1", text) is not None


def static_analysis_workflow_issues(text: str) -> list[str]:
    commands = shell_commands(text)
    issues: list[str] = []

    expected_producer = [
        "python3",
        "scripts/first_party_compile_units.py",
        "$KLOGG_BUILD_ROOT/compile_commands.json",
        "$KLOGG_WORKSPACE/src",
        "--null",
        ">",
        "tidy_files.nul",
    ]
    tidy_scope_commands = [
        command for command in commands if shell_tokens(command) == expected_producer
    ]
    if len(tidy_scope_commands) != 1 or any(
        command.startswith("find src ") and "tidy_files.txt" in command
        for command in commands
    ):
        issues.append(
            "clang-tidy TU scope must come from the configured compile database"
        )

    strict_consumers = []
    report_only_consumers = []
    for command in commands:
        tokens = shell_tokens(command)
        script_index = xargs_bash_script_index(tokens)
        if (
            script_index is None
            or option_value(tokens, "<") != "tidy_files.nul"
            or tokens.count("<") != 1
            or tokens.count("-c") != 1
        ):
            continue
        if (
            script_index + 2 >= len(tokens)
            or tokens[script_index + 1] != "_"
            or tokens[script_index + 2] != "<"
        ):
            continue
        script = tokens[script_index].strip()
        if not script.startswith("clang-tidy ") or not clang_tidy_consumes_xargs_argument(
            script
        ):
            continue
        if (
            '--line-filter="$TIDY_LINE_FILTER"' in script
            and not any(control in script for control in (";", "&&", "||", "\n"))
            and not any(control in tokens for control in ("&&", "||", ";"))
        ):
            strict_consumers.append(command)
        if "|| exit 0" in script and command.rstrip().endswith("|| true"):
            report_only_consumers.append(command)

    if (
        len(strict_consumers) != 1
        or len(report_only_consumers) != 1
        or any("tidy_files.txt" in command for command in commands)
    ):
        issues.append(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers"
        )

    cmake_commands = []
    for command in commands:
        tokens = shell_tokens(command)
        if tokens and tokens[0] == "cmake" and "--build" not in tokens:
            cmake_commands.append(tokens)
    sentry_configures = [
        tokens
        for tokens in cmake_commands
        if unique_cmake_option_value(tokens, "-S") == "$KLOGG_WORKSPACE"
        and unique_cmake_option_value(tokens, "-B") == "$KLOGG_BUILD_ROOT"
        and "-P" not in tokens
    ]
    sentry_assignments = [
        value
        for tokens in sentry_configures
        for value in cmake_cache_assignments(tokens, "KLOGG_USE_SENTRY")
    ]
    later_sentry_commands = [
        tokens
        for tokens in cmake_commands
        if tokens not in sentry_configures
        and unique_cmake_option_value(tokens, "-B") == "$KLOGG_BUILD_ROOT"
        and "-P" not in tokens
    ]
    sentry_unset = any(
        cmake_unsets_cache_entry(tokens, "KLOGG_USE_SENTRY")
        for tokens in sentry_configures
    )
    cmake_true_values = {"1", "ON", "TRUE", "YES", "Y"}
    if (
        len(sentry_configures) != 1
        or len(sentry_assignments) != 1
        or sentry_assignments[-1] not in cmake_true_values
        or later_sentry_commands
        or sentry_unset
    ):
        issues.append("static analysis must configure optional Sentry production code")

    if not re.search(
        r"^\s*KLOGG_CPM_CACHE_KEY_SUFFIX:\s*-sentry\s*$", text, re.MULTILINE
    ):
        issues.append("static analysis must use a Sentry-specific CPM cache key")
    return issues


def coverage_build_targets(command: str) -> set[str]:
    tokens = shell_tokens(command)
    if not tokens or tokens[:2] != ["cmake", "--build"]:
        return set()
    for control in ("&&", "||", ";", "|"):
        if control in tokens:
            tokens = tokens[: tokens.index(control)]
    target_index = None
    for option in ("-t", "--target"):
        if option in tokens:
            target_index = tokens.index(option) + 1
            break
    if target_index is None:
        return set()
    targets = set()
    for token in tokens[target_index:]:
        if token.startswith("-"):
            break
        targets.add(token)
    return targets


def coverage_workflow_issues(text: str) -> list[str]:
    issues: list[str] = []
    commands = shell_commands(text)
    gcovr_commands = [command for command in commands if command.startswith("gcovr ")]
    gcovr_tokens = [shell_tokens(command) for command in gcovr_commands]
    if len(gcovr_commands) != 2 or any(
        unique_option_value(tokens, "--filter") != "^src/"
        for tokens in gcovr_tokens
    ):
        issues.append("gcovr must include root-relative src/ paths in every report pass")

    if len(gcovr_commands) == 2:
        html_reports = []
        json_reports = []
        for command in gcovr_commands:
            tokens = shell_tokens(command)
            output = unique_option_value(tokens, "-o")
            if (
                "--html-details" in tokens
                and "--print-summary" in tokens
                and output == "coverage_report/index.html"
            ):
                html_reports.append(command)
            if "--json" in tokens and output == "coverage_report/coverage.json":
                json_reports.append(command)
        if len(html_reports) != 1 or len(json_reports) != 1:
            issues.append(
                "coverage must produce the authoritative HTML summary and JSON report"
            )

    build_targets = set()
    for command in commands:
        build_targets.update(coverage_build_targets(command))
    for target in ("klogg", "klogg_grep", "klogg_tests", "klogg_itests"):
        if target not in build_targets:
            issues.append(f"coverage build must include target {target}")

    negative_hits_option = "--gcov-ignore-parse-errors"
    negative_hits_value = "negative_hits.warn_once_per_file"
    if len(gcovr_commands) != 2 or any(
        tokens.count(negative_hits_option) != 1
        or option_value(tokens, negative_hits_option) != negative_hits_value
        for tokens in gcovr_tokens
    ):
        issues.append(
            "every coverage report must narrowly handle GCC's negative branch-hit bug"
        )
    if any(
        negative_hits_option in tokens
        and (
            tokens.count(negative_hits_option) != 1
            or option_value(tokens, negative_hits_option) != negative_hits_value
        )
        for tokens in gcovr_tokens
    ):
        issues.append(
            "coverage may ignore only negative_hits.warn_once_per_file parse errors"
        )
    if any(
        any(control in tokens for control in ("&&", "||", ";"))
        for tokens in gcovr_tokens
    ):
        issues.append("coverage report generation must fail closed")
    return issues


def coverage_floor(path: Path) -> float:
    value = float(path.read_text().strip())
    if not math.isfinite(value) or value <= 0 or value > 100:
        raise ValueError
    return value


def cppcheck_suppression_issues(text: str) -> list[str]:
    issues: list[str] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        entry = line.strip()
        if not entry:
            continue
        parts = entry.rsplit(":", 2)
        if len(parts) != 3 or not parts[1] or not parts[2].isdigit():
            issues.append(
                f"tests/cppcheck_suppressions.txt:{line_number}: "
                "cppcheck baselines must be qualified by path and line"
            )
    return issues


def ci_manifests(root: Path) -> list[Path]:
    paths = set()
    for pattern in ("*.yml", "*.yaml"):
        paths.update((root / ".github" / "workflows").glob(pattern))
    for pattern in ("action.yml", "action.yaml"):
        paths.update((root / ".github" / "actions").glob(f"**/{pattern}"))
    return sorted(paths)


def check_repo(root: Path) -> list[str]:
    issues: list[str] = []
    workflows = root / ".github" / "workflows"
    for path in ci_manifests(root):
        issues.extend(check_checkout_blocks(path.relative_to(root), path.read_text()))

    for path in sorted((root / "tests" / "sanitizers").glob("*_suppressions.txt")):
        text = path.read_text()
        for match in BROAD_SUPPRESSION_RE.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            issues.append(
                f"{path.relative_to(root)}:{line}: broad sanitizer suppression can hide first-party defects"
            )

    coverage_text = (workflows / "coverage.yml").read_text()
    issues.extend(
        f".github/workflows/coverage.yml: {issue}"
        for issue in coverage_workflow_issues(coverage_text)
    )
    if "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02" not in coverage_text:
        issues.append(
            ".github/workflows/coverage.yml: coverage artifact upload must use a reviewed commit SHA"
        )

    codeql_text = (workflows / "codeql-analysis.yml").read_text()
    issues.extend(
        f".github/workflows/codeql-analysis.yml: {issue}"
        for issue in codeql_workflow_issues(codeql_text)
    )

    static_text = (workflows / "static-analysis.yml").read_text()
    issues.extend(
        f".github/workflows/static-analysis.yml: {issue}"
        for issue in static_analysis_workflow_issues(static_text)
    )
    if 'python3 "$CLANG_TIDY_DIFF"' not in static_text:
        issues.append(
            ".github/workflows/static-analysis.yml: clang-tidy-diff must use the discovered Ubuntu tool path"
        )
    for extension in ("*.cxx", "*.hh", "*.hxx"):
        if extension not in static_text:
            issues.append(
                f".github/workflows/static-analysis.yml: static analysis must include {extension} files"
            )
    if "git diff --name-only -z" not in static_text:
        issues.append(
            ".github/workflows/static-analysis.yml: changed paths must use NUL-delimited Git output"
        )
    header_filter = (root / ".clang-tidy").read_text()
    if ".*/klogg/src/.*\\.(h|hh|hpp|hxx)$" not in header_filter:
        issues.append(
            ".clang-tidy: header filter must admit headers in new first-party modules"
        )
    for diagnostic_id in BROAD_CPPCHECK_IDS:
        if re.search(rf"--suppress={re.escape(diagnostic_id)}(?:\s|\\|$)", static_text):
            issues.append(
                f".github/workflows/static-analysis.yml: cppcheck suppression {diagnostic_id} must be location-qualified"
            )
    issues.extend(
        cppcheck_suppression_issues(
            (root / "tests" / "cppcheck_suppressions.txt").read_text()
        )
    )

    ci_text = (workflows / "ci-build.yml").read_text()
    if "cancel-in-progress: ${{ github.event_name == 'pull_request' }}" not in ci_text:
        issues.append(
            ".github/workflows/ci-build.yml: master publication runs must not be cancelable"
        )
    if "detect_container_overflow=0" in ci_text:
        issues.append(
            ".github/workflows/ci-build.yml: container-overflow detection must not be disabled globally"
        )
    mac_section = ci_text.partition("  Mac:")[2].partition("  Windows:")[0]
    if has_unsupported_macos_lsan(mac_section):
        issues.append(
            ".github/workflows/ci-build.yml: Apple ASan does not support detect_leaks=1"
        )

    for name in ("coverage-line.txt", "coverage-branch.txt"):
        path = root / ".github" / "baselines" / name
        try:
            coverage_floor(path)
        except (OSError, ValueError):
            issues.append(
                f"{path.relative_to(root)}: coverage floor must be a finite positive percentage no greater than 100"
            )

    return issues


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    issues = check_repo(root)
    if issues:
        print("\n".join(issues))
        return 1
    print("OK: CI quality gates are not silently weakened.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
