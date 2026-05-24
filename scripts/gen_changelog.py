#!/usr/bin/env python3
import argparse
import re
import subprocess
from collections import defaultdict
from dataclasses import dataclass
from datetime import date, datetime
from typing import Dict, List, Optional, Tuple


COMMIT_RE = re.compile(r"([0-9-]{10}) ([0-9a-f]+) (.*)")
CONVENTIONAL_RE = re.compile(r"([a-zA-Z]+)(?:\([^)]+\))?!?:\s*(.*)")


@dataclass
class CommitMessage:
    date: date
    sha: str
    group: str
    message: str


YearMonthKey = Tuple[int, int]
CommitsList = List[CommitMessage]
CommitsGroup = Dict[str, CommitsList]


GROUPS_MAPPING = [
    ("feature", "New features"),
    ("fixes", "Bug fixes"),
    ("docs", "Documentation"),
    ("perf", "Performance"),
    ("refactor", "Code refactoring"),
    ("tests", "Tests"),
    ("build", "Build system"),
    ("ci", "Continuous integration workflow"),
    ("chore", "Maintenance"),
    ("other", "Other commits"),
]


def git(args: List[str]) -> str:
    return subprocess.check_output(["git", *args], text=True).strip()


def version_tags() -> List[str]:
    output = git(["tag", "--list", "v[0-9]*", "--sort=-v:refname", "--merged", "HEAD"])
    return [line.strip() for line in output.splitlines() if line.strip()]


def latest_version_tag(excluding: Optional[str] = None) -> Optional[str]:
    for tag in version_tags():
        if tag != excluding:
            return tag
    return None


def resolve_from_ref(args: argparse.Namespace) -> Optional[str]:
    if args.from_ref:
        return args.from_ref
    if args.mode == "release":
        return latest_version_tag(args.current_tag)
    return latest_version_tag()


def normalize_group(group: str, message: str) -> str:
    group = group.lower()
    if group == "feat":
        return "feature"
    if group == "fix":
        return "fixes"
    if group == "doc":
        return "docs"
    if group == "test":
        return "tests"
    if group in {key for key, _ in GROUPS_MAPPING} and group != "other":
        return group
    if "feature" in message.lower():
        return "feature"
    if group == "other":
        return group
    return "other"


def commits_for_range(from_ref: Optional[str], to_ref: str) -> List[CommitMessage]:
    revision_range = f"{from_ref}..{to_ref}" if from_ref else to_ref
    log = git(["--no-pager", "log", "--format=%as %h %s (%an)", revision_range])
    commits: List[CommitMessage] = []

    for line in log.splitlines():
        match = COMMIT_RE.match(line)
        if not match:
            continue

        commit_date = datetime.strptime(match.group(1), "%Y-%m-%d").date()
        commit_sha = match.group(2)
        commit_message = match.group(3).strip()
        commit_group = "other"

        conventional = CONVENTIONAL_RE.match(commit_message)
        if conventional:
            commit_group = normalize_group(conventional.group(1), conventional.group(2))
            commit_message = conventional.group(2).strip()
        else:
            commit_group = normalize_group(commit_group, commit_message)

        commits.append(CommitMessage(commit_date, commit_sha, commit_group, commit_message))

    return commits


def render(commits: List[CommitMessage]) -> str:
    grouped: Dict[YearMonthKey, CommitsGroup] = defaultdict(lambda: defaultdict(list))
    for commit in commits:
        grouped[(commit.date.year, commit.date.month)][commit.group].append(commit)

    lines: List[str] = []
    for key in sorted(grouped.keys(), reverse=True):
        lines.append(f"# {key[0]}-{key[1]:02}:")
        for group, header in GROUPS_MAPPING:
            commits_in_group = grouped[key][group]
            if not commits_in_group:
                continue
            lines.append(f"## {header}:")
            for commit in commits_in_group:
                lines.append(
                    f" - [{commit.sha}](https://github.com/ZEACENT/klogg/commit/{commit.sha}): "
                    f"{commit.message}"
                )

    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--mode",
        choices=("prerelease", "release"),
        default="release",
        help="prerelease starts at the latest version release; release starts at the previous version release",
    )
    parser.add_argument("--from-ref", help="explicit changelog start ref")
    parser.add_argument("--to-ref", default="HEAD", help="changelog end ref")
    parser.add_argument("--current-tag", help="current release tag to exclude when mode=release")
    parser.add_argument(
        "--print-range",
        action="store_true",
        help="print the resolved git range before the changelog",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    from_ref = resolve_from_ref(args)
    if args.print_range:
        start = from_ref if from_ref else "repository start"
        print(f"Changes from {start} to {args.to_ref}\n")
    print(render(commits_for_range(from_ref, args.to_ref)))


if __name__ == "__main__":
    main()
