#!/usr/bin/env python3
"""Reject CI patterns that silently weaken quality gates."""

from __future__ import annotations

import math
import re
import sys
from pathlib import Path


CHECKOUT_SHA_RE = re.compile(r"actions/checkout@[0-9a-f]{40}")
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


def has_unsupported_macos_lsan(text: str) -> bool:
    return re.search(r"ASAN_OPTIONS=.*detect_leaks=1", text) is not None


def coverage_workflow_issues(text: str) -> list[str]:
    issues: list[str] = []
    run_blocks = workflow_run_blocks(text)
    active = "\n".join(run_blocks)
    if "--filter '.*/src/.*'" in active or active.count("--filter '^src/'") < 2:
        issues.append("gcovr must include root-relative src/ paths in every report pass")
    build_commands = "\n".join(
        line for line in active.splitlines() if "cmake --build" in line and " -t " in line
    )
    for target in ("klogg", "klogg_grep", "klogg_tests", "klogg_itests"):
        if re.search(rf"(?:^|\s){re.escape(target)}(?:\s|$)", build_commands) is None:
            issues.append(f"coverage build must include target {target}")
    if "--gcov-ignore-parse-errors" in active:
        issues.append("authoritative coverage reports must fail on malformed gcov data")
    if any("--json" in block and "|| true" in block for block in run_blocks):
        issues.append("coverage JSON generation must fail closed")
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

    static_text = (workflows / "static-analysis.yml").read_text()
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
