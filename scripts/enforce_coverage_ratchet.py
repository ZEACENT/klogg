#!/usr/bin/env python3
"""Enforce measured coverage and monotonic baseline floors."""

from __future__ import annotations

import argparse
import math
import re
import subprocess
import sys
from pathlib import Path


def percentage(label: str, text: str) -> float:
    match = re.search(label + r":.*?([0-9]+(?:\.[0-9]+)?)\s*%", text)
    if match is None:
        raise ValueError(f"coverage summary is missing a parseable {label!r} percentage")
    return float(match.group(1))


def floor_value(label: str, text: str, *, allow_zero: bool = False) -> float:
    value_text = text.strip()
    if not value_text:
        raise ValueError(f"{label} is empty")
    try:
        value = float(value_text)
    except ValueError as error:
        raise ValueError(f"{label} is not a number: {value_text!r}") from error
    minimum = 0.0 if allow_zero else 0.0
    if not math.isfinite(value) or value > 100.0 or value < minimum:
        raise ValueError(f"{label} must be a finite percentage between 0 and 100")
    if not allow_zero and value == 0.0:
        raise ValueError(f"{label} must be positive")
    return value


def evaluate(
    summary: str,
    line_floor_text: str,
    branch_floor_text: str,
    *,
    base_line_floor_text: str | None = None,
    base_branch_floor_text: str | None = None,
) -> list[str]:
    line = percentage("lines", summary)
    branch = percentage("branches", summary)
    line_floor = floor_value("line coverage floor", line_floor_text)
    branch_floor = floor_value("branch coverage floor", branch_floor_text)
    issues: list[str] = []

    if base_line_floor_text is not None:
        base_line = floor_value(
            "base line coverage floor", base_line_floor_text, allow_zero=True
        )
        if line_floor < base_line:
            issues.append(
                f"line coverage floor {line_floor}% is below the base floor {base_line}%"
            )
    if base_branch_floor_text is not None:
        base_branch = floor_value(
            "base branch coverage floor", base_branch_floor_text, allow_zero=True
        )
        if branch_floor < base_branch:
            issues.append(
                f"branch coverage floor {branch_floor}% is below the base floor {base_branch}%"
            )

    if line < line_floor:
        issues.append(f"line coverage {line}% is below the ratchet floor {line_floor}%")
    if branch < branch_floor:
        issues.append(
            f"branch coverage {branch}% is below the ratchet floor {branch_floor}%"
        )
    return issues


def git_file(base_sha: str, path: Path) -> str:
    commit = subprocess.run(
        ["git", "cat-file", "-e", f"{base_sha}^{{commit}}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if commit.returncode != 0:
        raise ValueError(f"cannot read base commit {base_sha}: {commit.stderr.strip()}")

    result = subprocess.run(
        ["git", "show", f"{base_sha}:{path.as_posix()}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        # The first ratcheted PR bootstraps a positive floor from a base that
        # predates the baseline files. Missing thereafter is equivalent to the
        # only permitted prior value: zero.
        return "0"
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--line-floor", type=Path, required=True)
    parser.add_argument("--branch-floor", type=Path, required=True)
    parser.add_argument("--base-sha")
    args = parser.parse_args()

    try:
        summary = args.summary.read_text(encoding="utf-8")
        line_floor_text = args.line_floor.read_text(encoding="utf-8")
        branch_floor_text = args.branch_floor.read_text(encoding="utf-8")
        base_line = git_file(args.base_sha, args.line_floor) if args.base_sha else None
        base_branch = git_file(args.base_sha, args.branch_floor) if args.base_sha else None
        issues = evaluate(
            summary,
            line_floor_text,
            branch_floor_text,
            base_line_floor_text=base_line,
            base_branch_floor_text=base_branch,
        )
        line = percentage("lines", summary)
        branch = percentage("branches", summary)
        line_floor = floor_value("line coverage floor", line_floor_text)
        branch_floor = floor_value("branch coverage floor", branch_floor_text)
    except (OSError, ValueError) as error:
        print(f"::error::{error}")
        return 1

    print(f"Line coverage:   measured {line}%   floor {line_floor}%")
    print(f"Branch coverage: measured {branch}%   floor {branch_floor}%")
    for issue in issues:
        print(f"::error::{issue}")
    return int(bool(issues))


if __name__ == "__main__":
    sys.exit(main())
