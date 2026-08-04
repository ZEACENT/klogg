#!/usr/bin/env python3
"""Run the PR changed-line clang-tidy policy locally and in CI."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from clang_tidy_header_filter import build_filter
from first_party_compile_units import load_first_party_compile_units


SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx"}
HEADER_EXTENSIONS = {".h", ".hh", ".hpp", ".hxx"}
CPP_EXTENSIONS = SOURCE_EXTENSIONS | HEADER_EXTENSIONS
SOURCE_FILE_RE = re.compile(r".*\.(?:c|cc|cpp|cxx)$", re.IGNORECASE)
# No inline (?i) flag: clang-tidy-diff.py wraps this pattern as "^%s$",
# which moves the flag off the start of the expression (a hard error on
# Python 3.14); the driver already matches with re.IGNORECASE.
CLANG_TIDY_SOURCE_IREGEX = r".*\.(c|cc|cpp|cxx)$"


def has_unsupported_path_character(path: Path) -> bool:
    return any(
        character in {'"', "\\"} or ord(character) < 32 or ord(character) == 127
        for character in str(path)
    )


def git_output(root: Path, *arguments: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *arguments])


def validate_paths(paths: list[Path]) -> list[Path]:
    for path in paths:
        if has_unsupported_path_character(path):
            raise ValueError(
                f"unsupported quoted/control character in path: {str(path)!r}"
            )
    return paths


def changed_paths(root: Path, base: str) -> list[Path]:
    output = git_output(
        root,
        "-c",
        "core.quotePath=false",
        "diff",
        "--name-only",
        "-z",
        # Exclude deleted files: clang-tidy cannot analyze a path that no
        # longer exists, so a file-deleting PR would break the gate.
        "--diff-filter=d",
        base,
        "--",
        "src/",
    )
    return validate_paths(
        [Path(value.decode()) for value in output.split(b"\0") if value]
    )


def ensure_no_untracked_cpp_paths(root: Path) -> None:
    output = git_output(
        root,
        "ls-files",
        "--others",
        "--exclude-standard",
        "-z",
        "--",
        "src/",
    )
    paths = validate_paths(
        [Path(value.decode()) for value in output.split(b"\0") if value]
    )
    untracked_cpp = [
        path for path in paths if path.suffix.lower() in CPP_EXTENSIONS
    ]
    if untracked_cpp:
        joined = ", ".join(str(path) for path in untracked_cpp)
        raise ValueError(
            f"stage or add untracked C/C++ files before analysis: {joined}"
        )


def unified_patch(root: Path, base: str) -> str:
    return git_output(
        root,
        "-c",
        "core.quotePath=false",
        "diff",
        "-U0",
        base,
        "--",
        "src/",
    ).decode()


def find_clang_tidy(explicit: str | None) -> Path:
    if explicit:
        candidate = Path(explicit)
        if candidate.is_file():
            return candidate.resolve()
        resolved = shutil.which(explicit)
        if resolved:
            return Path(resolved).resolve()
        raise FileNotFoundError(f"clang-tidy not found: {explicit}")

    resolved = shutil.which("clang-tidy")
    if resolved:
        return Path(resolved).resolve()
    raise FileNotFoundError("clang-tidy not found; pass --clang-tidy")


def find_clang_tidy_diff(explicit: str | None, clang_tidy: Path) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(Path(explicit))
    for command in ("clang-tidy-diff", "clang-tidy-diff-18.py"):
        resolved = shutil.which(command)
        if resolved:
            candidates.append(Path(resolved))
    candidates.extend(
        [
            clang_tidy.parent.parent / "share" / "clang" / "clang-tidy-diff.py",
            Path("/usr/lib/llvm-18/share/clang/clang-tidy-diff.py"),
        ]
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError("clang-tidy-diff.py not found; pass --clang-tidy-diff")


def clang_tidy_extra_arguments(arguments: list[str]) -> list[str]:
    return [
        "-extra-arg=-Wno-unknown-warning-option",
        *(f"-extra-arg={argument}" for argument in arguments),
    ]


def require_configured_sources(
    root: Path, source_paths: list[Path], units: list[Path]
) -> None:
    configured = {unit.resolve() for unit in units}
    missing = [
        path
        for path in source_paths
        if (root / path).resolve() not in configured
    ]
    if missing:
        joined = ", ".join(str(path) for path in missing)
        raise ValueError(
            f"changed source file is not configured in compile_commands.json: {joined}"
        )


def header_analysis_command(
    clang_tidy: Path,
    build_dir: Path,
    header: Path,
    line_filter: str,
    extra_arguments: list[str],
) -> list[str]:
    return [
        str(clang_tidy),
        "-p",
        str(build_dir),
        f"--line-filter={line_filter}",
        *clang_tidy_extra_arguments(extra_arguments),
        str(header),
    ]


def run_process(
    command: list[str], *, input_text: str | None = None, environment: dict[str, str]
) -> tuple[int, str]:
    completed = subprocess.run(
        command,
        input=input_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=environment,
        check=False,
    )
    return completed.returncode, completed.stdout


def finding_lines(output: str, paths: list[Path]) -> list[str]:
    path_tokens = [f"{path.as_posix()}:" for path in paths]
    findings: list[str] = []
    seen: set[str] = set()
    for line in output.splitlines():
        if " warning:" not in line and " error:" not in line:
            continue
        if not any(token in line for token in path_tokens):
            continue
        if line not in seen:
            findings.append(line)
            seen.add(line)
    return findings


def run_header_pass(
    clang_tidy: Path,
    build_dir: Path,
    units: list[Path],
    line_filter: str,
    jobs: int,
    extra_arguments: list[str],
    environment: dict[str, str],
) -> tuple[int, str]:
    def analyze(unit: Path) -> tuple[int, str]:
        return run_process(
            [
                str(clang_tidy),
                "-p",
                str(build_dir),
                f"--line-filter={line_filter}",
                *clang_tidy_extra_arguments(extra_arguments),
                str(unit),
            ],
            environment=environment,
        )

    status = 0
    outputs: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as executor:
        for unit_status, unit_output in executor.map(analyze, units):
            status = status or unit_status
            outputs.append(unit_output)
    return status, "".join(outputs)


def run_direct_header_pass(
    clang_tidy: Path,
    build_dir: Path,
    headers: list[Path],
    line_filter: str,
    jobs: int,
    extra_arguments: list[str],
    environment: dict[str, str],
) -> tuple[int, str]:
    def analyze(header: Path) -> tuple[int, str]:
        return run_process(
            header_analysis_command(
                clang_tidy,
                build_dir,
                header,
                line_filter,
                extra_arguments,
            ),
            environment=environment,
        )

    status = 0
    outputs: list[str] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as executor:
        for header_status, header_output in executor.map(analyze, headers):
            status = status or header_status
            outputs.append(header_output)
    return status, "".join(outputs)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", required=True, help="base commit to diff against")
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=Path("src"))
    parser.add_argument("--clang-tidy")
    parser.add_argument("--clang-tidy-diff")
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="additional compiler argument forwarded to clang-tidy; repeat as needed",
    )
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    build_dir = args.build_dir.resolve()
    source_root = (root / args.source_root).resolve()
    compile_database = build_dir / "compile_commands.json"

    try:
        ensure_no_untracked_cpp_paths(root)
        paths = changed_paths(root, args.base)
        patch = unified_patch(root, args.base)
        clang_tidy = find_clang_tidy(args.clang_tidy)
        clang_tidy_diff = find_clang_tidy_diff(args.clang_tidy_diff, clang_tidy)
        units = load_first_party_compile_units(compile_database, source_root)
    except (FileNotFoundError, OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    source_paths = [path for path in paths if path.suffix.lower() in SOURCE_EXTENSIONS]
    header_paths = [path for path in paths if path.suffix.lower() in HEADER_EXTENSIONS]
    if not source_paths and not header_paths:
        print("clang-tidy: no changed C/C++ files under src/")
        return 0
    if not units:
        print("error: compile database contains no first-party translation units", file=sys.stderr)
        return 1
    try:
        require_configured_sources(root, source_paths, units)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    environment = os.environ.copy()
    environment["PATH"] = f"{clang_tidy.parent}{os.pathsep}{environment.get('PATH', '')}"
    status = 0
    output_parts: list[str] = []

    if source_paths:
        source_status, source_output = run_process(
            [
                sys.executable,
                str(clang_tidy_diff),
                "-clang-tidy-binary",
                str(clang_tidy),
                "-p",
                "1",
                "-j",
                str(max(1, args.jobs)),
                "-path",
                str(build_dir),
                "-iregex",
                CLANG_TIDY_SOURCE_IREGEX,
                *clang_tidy_extra_arguments(args.extra_arg),
                "-quiet",
            ],
            input_text=patch,
            environment=environment,
        )
        status = status or source_status
        output_parts.append(source_output)

    live_header_paths = [
        path for path in header_paths if (root / path).is_file()
    ]
    header_filter = build_filter(patch)
    if live_header_paths and not header_filter:
        print(
            "error: changed headers were not represented in the unified diff filter",
            file=sys.stderr,
        )
        return 1
    if header_filter:
        line_filter = json.dumps(header_filter, separators=(",", ":"))
        header_status, header_output = run_header_pass(
            clang_tidy,
            build_dir,
            units,
            line_filter,
            args.jobs,
            args.extra_arg,
            environment,
        )
        status = status or header_status
        output_parts.append(header_output)

        direct_status, direct_output = run_direct_header_pass(
            clang_tidy,
            build_dir,
            [(root / path).resolve() for path in live_header_paths],
            line_filter,
            args.jobs,
            args.extra_arg,
            environment,
        )
        status = status or direct_status
        output_parts.append(direct_output)

    output = "".join(output_parts)
    if output:
        print(output, end="" if output.endswith("\n") else "\n")
    findings = finding_lines(output, source_paths + header_paths)
    if status != 0 or "Error while processing " in output:
        print("error: clang-tidy failed to analyze one or more translation units", file=sys.stderr)
        return 1
    if findings:
        print("error: clang-tidy reported issues on changed src/ files:", file=sys.stderr)
        print("\n".join(findings), file=sys.stderr)
        return 1

    print("clang-tidy: clean (no findings on changed src/ lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
