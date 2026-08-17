#!/usr/bin/env python3
"""
Verifies that first-party headers are self-contained: each header must
compile standalone (``-fsyntax-only`` on a TU that includes nothing but the
header) with the same flags the real build uses.

Background: PR #58's clang-tidy leg failed in CI because
``src/ui/include/viewinterface.h`` used ``QString`` without including it.
Every in-tree consumer happened to include ``<QString>`` first, so local
builds (which never compile a header on its own) stayed green; only the CI
changed-header clang-tidy sweep, which analyzes the header as its own
translation unit, saw the error. This script closes that gap locally.

Header set (mirrors the repo convention used by run_changed_clang_tidy.py):
    * default        -- headers changed vs ``origin/master...HEAD`` under src/
    * ``--all``      -- every tracked header under src/
    * explicit paths -- exactly those files (may live anywhere)

For each header a tiny .cpp is synthesized that includes only that header,
and it is compiled with flags derived from ``build_root/compile_commands.json``:
a representative translation unit from the same module (longest common path
prefix with the header) contributes its define/std/warning flags; the source
file and ``-o`` output are swapped out for the synthesized TU. Include paths
(-I/-isystem/-iframework) are the UNION across all first-party units, because
a module's own TUs do not always carry every include path its public headers
need (``src/utils`` TUs compile without simdutf's -isystem path even though
``simdutf_wrapper.h`` includes <simdutf.h>; only consuming modules add it).
This is a lint, not a build: wider include resolution cannot hide the class of
bug this gate targets (a missing #include of a first-party/Qt type). Because
the real flags (``-Werror``, ``QT_NO_KEYWORDS``, ...) are reused, a header that
passes here compiles under the same strictness CI applies.

Requires a configured build: ``build_root/compile_commands.json`` must exist
(run the cmake configure step from CLAUDE.md first).

Usage:
    python3 scripts/lint_header_self_contained.py                 # changed headers
    python3 scripts/lint_header_self_contained.py --all           # all src/ headers
    python3 scripts/lint_header_self_contained.py src/ui/include/viewinterface.h
    python3 scripts/lint_header_self_contained.py --base origin/master...HEAD~1

Exit codes:
    0   All checked headers compiled standalone (or the header set was empty).
    1   At least one header failed, or the compile database is unusable.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

HEADER_EXTENSIONS = {".h", ".hh", ".hpp", ".hxx"}
SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx"}

# How many diagnostic lines of a failing compile to print. The first handful of
# lines carries the actual "unknown type name" error; the rest is usually note
# spam from template instantiation backtraces.
MAX_DIAGNOSTIC_LINES = 12


def git_output(root: Path, *arguments: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *arguments])


def changed_headers(root: Path, base: str) -> list[Path]:
    """Headers changed vs ``base`` under src/ (deleted files excluded -- a
    deleted header cannot be compiled and would break the gate)."""
    output = git_output(
        root,
        "-c",
        "core.quotePath=false",
        "diff",
        "--name-only",
        "-z",
        "--diff-filter=d",
        base,
        "--",
        "src/",
    )
    return [
        root / value.decode()
        for value in output.split(b"\0")
        if value and Path(value.decode()).suffix.lower() in HEADER_EXTENSIONS
    ]


def all_headers(root: Path) -> list[Path]:
    """Every tracked header under src/ (git ls-files, so generated and
    untracked files are not swept in)."""
    output = git_output(
        root, "-c", "core.quotePath=false", "ls-files", "-z", "--", "src/"
    )
    return [
        root / value.decode()
        for value in output.split(b"\0")
        if value and Path(value.decode()).suffix.lower() in HEADER_EXTENSIONS
    ]


def entry_file(entry: dict[str, Any]) -> str:
    if "file" in entry:
        return str(entry["file"])
    files = entry.get("files") or []
    return str(files[0]) if files else ""


def entry_arguments(entry: dict[str, Any]) -> list[str]:
    """Command-line tokens of a compile database entry, whether the entry
    uses the ``command`` string or the ``arguments`` array form."""
    if "arguments" in entry:
        return [str(token) for token in entry["arguments"]]
    return shlex.split(str(entry.get("command", "")))


def load_compile_database(build_dir: Path) -> list[dict[str, Any]]:
    database_path = build_dir / "compile_commands.json"
    if not database_path.is_file():
        raise FileNotFoundError(
            f"{database_path} not found; configure the build first "
            f"(see the Build section of CLAUDE.md) so cmake generates "
            f"compile_commands.json"
        )
    database = json.loads(database_path.read_text())
    if not isinstance(database, list):
        raise ValueError("compile_commands.json must contain a JSON array")
    return database


def first_party_units(
    database: list[dict[str, Any]], source_root: Path
) -> list[tuple[Path, Path, list[str]]]:
    """(source path, working directory, command tokens) for translation units
    whose source lives under the repo's src/ tree.

    Autogen units (mocs_compilation.cpp etc.) live under build_root, so they
    are excluded here; a real module TU is a better flag donor anyway because
    it carries the module's own include directories.
    """
    source_root = source_root.resolve()
    units: list[tuple[Path, Path, list[str]]] = []
    for entry in database:
        filename = entry_file(entry)
        if not filename:
            continue
        path = Path(filename)
        if not path.is_absolute():
            path = Path(str(entry.get("directory", "."))) / path
        path = path.resolve()
        try:
            path.relative_to(source_root)
        except ValueError:
            continue
        if path.suffix.lower() not in SOURCE_EXTENSIONS:
            continue
        units.append(
            (path, Path(str(entry.get("directory", "."))), entry_arguments(entry))
        )
    return units


def representative_unit(
    header: Path, units: list[tuple[Path, Path, list[str]]]
) -> tuple[Path, Path, list[str]]:
    """Pick the TU whose flags best fit ``header``: the one with the longest
    common directory prefix (i.e. from the same module), so the module's -D/std
    set is reused. Falls back to the first unit when nothing shares a prefix
    (e.g. a header outside src/, such as a /tmp probe)."""
    best: tuple[Path, Path, list[str]] | None = None
    best_common = -1
    for unit in units:
        common = len(os.path.commonpath([unit[0], header]))
        if common > best_common:
            best_common = common
            best = unit
    if best is None:
        raise ValueError("compile database contains no first-party translation units")
    return best


# Flags that take their include path as a separate token.
_INCLUDE_PAIR_FLAGS = ("-I", "-isystem", "-iquote", "-iframework")
# Joined forms (-I/path, -isystem/path, ...). -iquote/-iframework joined forms
# are not emitted by cmake, so only -I/-isystem need the prefix match.
_INCLUDE_JOINED_PREFIXES = ("-I", "-isystem")


def include_flag_union(units: list[tuple[Path, Path, list[str]]]) -> list[str]:
    """Union of every include-path flag across all first-party units.

    A module's TUs do not necessarily carry every include path its public
    headers rely on: src/utils/src/cpu_info.cpp compiles without simdutf's
    -isystem path, yet src/utils/include/simdutf_wrapper.h includes
    <simdutf.h> (only the consuming modules add it). Checking such a header
    with only its own module's flags would report a spurious 'file not found'.
    """
    seen: set[str] = set()
    union: list[str] = []
    for _, _, arguments in units:
        i = 1  # arguments[0] is the compiler
        while i < len(arguments):
            token = arguments[i]
            if token in _INCLUDE_PAIR_FLAGS and i + 1 < len(arguments):
                pair = token + "\0" + arguments[i + 1]
                if pair not in seen:
                    seen.add(pair)
                    union.extend((token, arguments[i + 1]))
                i += 2
                continue
            if any(
                token.startswith(prefix) and len(token) > len(prefix)
                for prefix in _INCLUDE_JOINED_PREFIXES
            ):
                if token not in seen:
                    seen.add(token)
                    union.append(token)
            i += 1
    return union


def syntax_only_command(
    unit: tuple[Path, Path, list[str]], probe: Path, extra_includes: list[str]
) -> list[str]:
    """Rewrite a real compile command into a -fsyntax-only check of ``probe``:
    keep the compiler, all -D/-I/-isystem/-iframework/std/warning flags, and
    drop the output/dependency-generation/source tokens. ``extra_includes``
    (the cross-module include-path union) is appended last so headers whose own
    module lacks a needed path still resolve it.

    Anything not recognised is kept verbatim; only the tokens that would
    conflict with a swapped-in source are stripped:
      -o <file>            the real object path
      -c <source>          the real source file
      -MD/-MMD + -MF/-MT   depfile generation (meaningless with -fsyntax-only
                           and -MT's target would dangle)
    """
    _, _, arguments = unit
    if not arguments:
        raise ValueError("compile database entry has an empty command")

    stripped: list[str] = []
    skip_next = False
    for token in arguments[1:]:  # arguments[0] is the compiler
        if skip_next:
            skip_next = False
            continue
        if token in ("-o", "-c", "-MF", "-MT", "-MQ"):
            skip_next = True
            continue
        if token in ("-MD", "-MMD"):
            continue
        stripped.append(token)

    # The source file itself is normally the token right after -c, which the
    # loop above already consumed; defensive: also drop any bare token that
    # resolves to the original source path (some generators emit it without -c).
    source = str(unit[0])
    stripped = [token for token in stripped if token != source]

    # Deduplicate the union against what the representative already carries.
    present = set(stripped)
    additional: list[str] = []
    i = 0
    while i < len(extra_includes):
        token = extra_includes[i]
        if token in _INCLUDE_PAIR_FLAGS and i + 1 < len(extra_includes):
            if token not in present or extra_includes[i + 1] not in present:
                additional.extend((token, extra_includes[i + 1]))
            i += 2
            continue
        if token not in present:
            additional.append(token)
        i += 1

    return [
        arguments[0],
        *stripped,
        *additional,
        "-fsyntax-only",
        "-x",
        "c++",
        str(probe),
    ]


def check_header(
    header: Path,
    unit: tuple[Path, Path, list[str]],
    probe_dir: Path,
    extra_includes: list[str],
) -> tuple[Path, bool, str]:
    """Compile a TU that includes only ``header``; return (header, ok, output)."""
    # One probe per header: probe files live in a shared tempdir, so the name
    # must be unique per header.
    probe = probe_dir / f"probe_{abs(hash(str(header))) & 0xFFFFFFFF:x}.cpp"
    probe.write_text(f'#include "{header}"\n')
    command = syntax_only_command(unit, probe, extra_includes)
    completed = subprocess.run(
        command,
        cwd=str(unit[1]),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    return header, completed.returncode == 0, completed.stdout


def first_diagnostics(output: str) -> str:
    """The leading error/warning lines of a compiler invocation, for display."""
    lines = [
        line
        for line in output.splitlines()
        if " error:" in line or " warning:" in line
    ]
    if not lines:
        # No recognisable diagnostics (e.g. a crash): fall back to the head of
        # the raw output so the failure is still actionable.
        lines = output.splitlines()
    return "\n".join(lines[:MAX_DIAGNOSTIC_LINES])


def main(argv: list[str]) -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "headers",
        nargs="*",
        type=Path,
        help="explicit header files to check (overrides --all/--base)",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="check every tracked header under src/ instead of only changed ones",
    )
    parser.add_argument(
        "--base",
        default="origin/master...HEAD",
        help="git revision range to diff for changed headers "
        "(default: %(default)s)",
    )
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=repo_root / "build_root",
        help="build directory containing compile_commands.json "
        "(default: %(default)s)",
    )
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args(argv)

    if args.headers:
        headers = [
            path if path.is_absolute() else (Path.cwd() / path)
            for path in args.headers
        ]
        headers = [path.resolve() for path in headers]
        missing = [str(path) for path in headers if not path.is_file()]
        if missing:
            print(f"error: header not found: {', '.join(missing)}", file=sys.stderr)
            return 1
    elif args.all:
        headers = all_headers(repo_root)
    else:
        try:
            headers = changed_headers(repo_root, args.base)
        except subprocess.CalledProcessError as error:
            print(f"error: git diff failed for base {args.base!r}: {error}", file=sys.stderr)
            return 1

    if not headers:
        print("header-self-contained: no headers to check")
        return 0

    try:
        database = load_compile_database(args.build_dir)
        units = first_party_units(database, (repo_root / "src").resolve())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    if not units:
        print(
            "error: compile database contains no first-party translation units",
            file=sys.stderr,
        )
        return 1

    failures = 0
    extra_includes = include_flag_union(units)
    with tempfile.TemporaryDirectory(prefix="klogg-header-lint-") as temp:
        probe_dir = Path(temp)
        with concurrent.futures.ThreadPoolExecutor(
            max_workers=max(1, args.jobs)
        ) as executor:
            futures = [
                executor.submit(
                    check_header,
                    header,
                    representative_unit(header, units),
                    probe_dir,
                    extra_includes,
                )
                for header in headers
            ]
            # Report in input order (not completion order) so output is stable
            # across runs and diffable in CI logs.
            for future in futures:
                header, ok, output = future.result()
                rel = header
                try:
                    rel = header.relative_to(repo_root)
                except ValueError:
                    pass
                if ok:
                    print(f"PASS {rel}")
                else:
                    failures += 1
                    print(f"FAIL {rel}")
                    diagnostics = first_diagnostics(output)
                    if diagnostics:
                        for line in diagnostics.splitlines():
                            print(f"    {line}")
                    print()

    if failures:
        print(f"header-self-contained: {failures} of {len(headers)} header(s) failed")
        return 1
    print(f"header-self-contained: OK ({len(headers)} header(s) compile standalone)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
