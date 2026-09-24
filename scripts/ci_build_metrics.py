#!/usr/bin/env python3
"""Report compilation work after one unchanged build using its fresh Ninja v5 log.

The caller supplies a log from a fresh build, not accumulated/recompacted history.
No build is run or changed. Parallel work-ms is a sum, not wall time or a critical
path estimate. Union time includes only observed compilation intervals. p95 uses
nearest rank. Ownership is lexical and conservative; no source files are opened.
"""

from __future__ import annotations

import argparse
import json
import math
import ntpath
import posixpath
import re
import shlex
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from first_party_compile_units import SOURCE_EXTENSIONS, entry_file


CATEGORIES = ("first_party", "third_party", "unknown")
DEPENDENCY_DIRS = {"cpm_cache", "_deps", "3rdparty"}
COMPILE_EXTENSIONS = SOURCE_EXTENSIONS | {".m", ".mm", ".s", ".asm", ".c++"}
OBJECT_EXTENSIONS = {".o", ".obj"}


class Paths:
    """Normalize foreign Windows paths without depending on the reporting host."""

    def __init__(self, repo: str, build: str):
        self.windows = bool(ntpath.splitdrive(repo)[0])
        self.module = ntpath if self.windows else posixpath
        if not self.module.isabs(repo) or not self.module.isabs(build):
            raise ValueError("repo and build roots must be absolute paths")
        self.repo = self.normalize(repo, repo)
        self.build = self.normalize(build, build)

    def normalize(self, path: str, base: str) -> str:
        value = self.module.normpath(self.module.join(base, path))
        if self.windows:
            value = ntpath.normcase(value).replace("\\", "/")
        return value

    def source_owner(self, source: str) -> str:
        if DEPENDENCY_DIRS.intersection(source.split("/")):
            return "third_party"
        if any(source.startswith(self.repo.rstrip("/") + "/" + root + "/")
               for root in ("src", "tests", "benchmarks")):
            return "first_party"
        return "unknown"


def target_for(output: str) -> str | None:
    match = re.match(r"(.*/CMakeFiles/[^/]+\.dir)/", output, re.IGNORECASE)
    return match.group(1) if match else None


def qt_generated(source: str, paths: Paths) -> bool:
    if not source.startswith(paths.build.rstrip("/") + "/"):
        return False
    name = posixpath.basename(source)
    return bool(re.fullmatch(r"(?:mocs_compilation(?:_[^.]+)?|moc_.+|qrc_.+|ui_.+)\.(?:cpp|cxx|cc)", name))


def command_output(entry: dict[str, Any], windows: bool) -> str | None:
    if "output" in entry:
        output = entry["output"]
        if not isinstance(output, str) or not output:
            raise ValueError("compile database output must be a nonempty string")
        return output
    if "arguments" in entry:
        tokens = entry["arguments"]
        if not isinstance(tokens, list) or not all(isinstance(token, str) for token in tokens):
            raise ValueError("compile database arguments must be an array of strings")
    else:
        command = entry.get("command", "")
        if not isinstance(command, str):
            raise ValueError("compile database command must be a string")
        if windows:
            # Windows compiler paths use backslashes, not POSIX shell escapes.
            if command.count('"') % 2:
                raise ValueError("unbalanced quotes in Windows compile command")
            tokens = [token.replace('"', '') for token in re.findall(r'(?:[^\s"]|"[^"]*")+', command)]
        else:
            tokens = shlex.split(command)
    outputs = []
    for index, token in enumerate(tokens):
        if token in ("-o", "/Fo", "-Fo"):
            if index + 1 >= len(tokens):
                raise ValueError("compile command output option has no path")
            outputs.append(tokens[index + 1])
        elif token.startswith(("/Fo", "-Fo")) and len(token) > 3:
            outputs.append(token[3:])
        elif token.startswith("-o") and len(token) > 2:
            outputs.append(token[2:])
    # Missing/ambiguous output mappings remain unknown; never infer by basename.
    return outputs[0] if len(set(outputs)) == 1 and outputs[0] else None


def compile_owners(database: Any, paths: Paths) -> dict[str, str]:
    if not isinstance(database, list):
        raise ValueError("compile_commands.json must contain a JSON array")
    units: list[tuple[str, str]] = []
    targets: dict[str, set[str]] = {}
    for item in database:
        if not isinstance(item, dict):
            raise ValueError("compile database entries must be objects")
        output = command_output(item, paths.windows)
        directory = item.get("directory", paths.build)
        filename = entry_file(item)
        if not isinstance(directory, str):
            raise ValueError("compile database directory must be a string")
        if not output or not filename:
            continue
        directory = paths.normalize(directory, paths.build)
        source = paths.normalize(filename, directory)
        output = paths.normalize(output, directory)
        if posixpath.splitext(source)[1].lower() not in COMPILE_EXTENSIONS:
            continue
        units.append((source, output))
        target = target_for(output)
        source_owner = paths.source_owner(source)
        # Learn ownership from original sources, never from other generated
        # units. Full output prefixes keep identically named targets separate.
        generated = source.startswith(paths.build.rstrip("/") + "/")
        if target and (source_owner != "unknown" or not generated):
            targets.setdefault(target, set()).add(source_owner)

    owners: dict[str, set[str]] = {}
    for source, output in units:
        owner = paths.source_owner(source)
        if owner == "unknown" and qt_generated(source, paths):
            target_owners = targets.get(target_for(output), set())
            if len(target_owners) == 1:
                owner = next(iter(target_owners))
        if DEPENDENCY_DIRS.intersection(output.split("/")):
            owner = "unknown" if owner == "first_party" else "third_party"
        owners.setdefault(output, set()).add(owner)
    return {output: next(iter(values)) if len(values) == 1 else "unknown"
            for output, values in owners.items()}


def log_edges(text: str, paths: Paths) -> tuple[list[dict[str, Any]], int]:
    lines = text.splitlines()
    # Ninja 1.13 (Ubuntu 26.04) writes v7; v6/v7 only changed the mtime field,
    # which this report never reads. start/end keep millisecond units.
    if not lines or not re.fullmatch(r"# ninja log v[5-7]", lines[0]):
        raise ValueError("expected a fresh Ninja v5-v7 log")
    edges: list[dict[str, Any]] = []
    records = 0
    for number, line in enumerate(lines[1:], 2):
        fields = line.split("\t")
        if (len(fields) != 5 or not all(re.fullmatch(r"[0-9]+", field) for field in fields[:3])
                or not fields[3] or not re.fullmatch(r"[0-9a-fA-F]{1,16}", fields[4])):
            raise ValueError(f"malformed Ninja log record at line {number}")
        start, end = int(fields[0]), int(fields[1])
        if end < start:
            raise ValueError(f"negative Ninja edge duration at line {number}")
        output = paths.normalize(fields[3], paths.build)
        identity = (start, end, int(fields[4], 16))
        # Ninja writes a multi-output edge's records together, with the same
        # command hash and interval. mtime may differ per output. A repeated
        # output is another execution, even with zero-ms duration. Do not group
        # nonadjacent records or equal timestamps alone.
        if edges and edges[-1]["identity"] == identity and output not in edges[-1]["outputs"]:
            edges[-1]["outputs"].append(output)
        else:
            edges.append({"identity": identity, "outputs": [output]})
        records += 1
    return edges, records


def summarize(intervals: list[tuple[int, int]]) -> dict[str, Any]:
    durations = sorted(end - start for start, end in intervals)
    active = 0
    boundary = 0
    for start, end in sorted(intervals):
        active += max(0, end - max(start, boundary))
        boundary = max(boundary, end)
    return {
        "edges": len(intervals),
        "parallel_work_ms": sum(durations),
        "union_active_ms": active,
        "max_ms": durations[-1] if durations else None,
        "median_ms": statistics.median(durations) if durations else None,
        "p95_ms": durations[math.ceil(len(durations) * 0.95) - 1] if durations else None,
    }


def measure(text: str, database: Any, repo: str, build: str,
            wall_ms: int | None = None, object_cache: str = "unmeasured") -> dict[str, Any]:
    paths = Paths(repo, build)
    owners = compile_owners(database, paths)
    edges, records = log_edges(text, paths)
    last_end = max((edge["identity"][1] for edge in edges), default=0)
    if wall_ms is not None and (wall_ms < 0 or wall_ms < last_end):
        raise ValueError("build wall-ms must be nonnegative and cover all observed Ninja intervals")
    categories: dict[str, list[tuple[int, int]]] = {name: [] for name in CATEGORIES}
    unknown: set[str] = set()
    unmatched: set[str] = set()
    unmatched_edges = 0
    for edge in edges:
        outputs = edge["outputs"]
        mapped = [output for output in outputs if output in owners]
        objects = [output for output in outputs if posixpath.splitext(output)[1].lower() in OBJECT_EXTENSIONS]
        if not mapped and not objects:
            unmatched.update(outputs)
            unmatched_edges += 1
            continue
        evidence = {owners[output] for output in mapped}
        owner = next(iter(evidence)) if len(evidence) == 1 else "unknown"
        start, end, _ = edge["identity"]
        categories[owner].append((start, end))
        if owner == "unknown":
            unknown.update(mapped + objects)
    return {
        "schema_version": 1,
        "log_records": records,
        "compilation": summarize([interval for values in categories.values() for interval in values]),
        "categories": {name: summarize(values) for name, values in categories.items()},
        "unknown_outputs": sorted(unknown),
        "unmatched_outputs": sorted(unmatched),
        "unmatched_edges": unmatched_edges,
        "build_wall_ms": wall_ms,
        "object_cache": object_cache,
    }


class MetricsError(RuntimeError):
    """A report could not be produced from the completed build's evidence."""


CACHE_COUNTERS = ("direct_cache_hit", "preprocessed_cache_hit", "cache_miss")


def ccache_counters(runner) -> dict[str, int] | None:
    """Read global ccache counters; absence of the tool is not a build failure."""
    try:
        result = runner(["ccache", "--print-stats"], check=True, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return None
    values: dict[str, int] = {}
    for line in result.stdout.splitlines():
        name, separator, value = line.partition("\t")
        if not separator or not re.fullmatch(r"[A-Za-z_]+", name) or not re.fullmatch(r"[0-9]+", value):
            raise MetricsError("unrecognized ccache --print-stats record: " + line)
        values[name] = int(value)
    if not {"stats_zeroed_timestamp", *CACHE_COUNTERS} <= set(values):
        raise MetricsError("ccache --print-stats lacks the expected global counters")
    return values


def cache_deltas(before: dict[str, int] | None, after: dict[str, int] | None) -> dict[str, int] | None:
    """Global counter deltas only; never per-party hit rates or duration inference."""
    if before is None or after is None:
        return None
    if after["stats_zeroed_timestamp"] != before["stats_zeroed_timestamp"]:
        return None
    deltas = {name: after[name] - before[name] for name in CACHE_COUNTERS}
    if any(delta < 0 for delta in deltas.values()):
        return None
    return deltas


def run_build(command, *, repo_root, build_root, output, object_cache, runner=None, clock=None) -> dict[str, Any]:
    """Run the caller's exact build once, then report its fresh Ninja evidence."""
    runner = subprocess.run if runner is None else runner
    clock = time.monotonic if clock is None else clock
    if object_cache not in ("measured", "disabled", "unmeasured"):
        raise MetricsError("unknown object cache mode: " + object_cache)
    repo_root = str(Path(repo_root).resolve())
    build_root = str(Path(build_root).resolve())
    output = Path(output)
    if output.exists() or output.is_symlink():
        raise MetricsError("metrics output already exists: " + str(output))
    before = ccache_counters(runner) if object_cache == "measured" else None
    started = clock()
    result = runner(list(command), check=False)
    wall_ms = max(0, round((clock() - started) * 1000))
    if result.returncode:
        raise subprocess.CalledProcessError(result.returncode, command, result.stdout, result.stderr)
    after = ccache_counters(runner) if object_cache == "measured" else None
    deltas = cache_deltas(before, after)
    try:
        report = measure((Path(build_root) / ".ninja_log").read_text(encoding="utf-8"),
                         json.loads((Path(build_root) / "compile_commands.json").read_text(encoding="utf-8")),
                         repo_root, build_root, wall_ms,
                         object_cache if deltas is not None or object_cache != "measured" else "unmeasured")
    except (OSError, UnicodeError, ValueError) as error:
        raise MetricsError("build succeeded but its metrics evidence is invalid: " + str(error)) from error
    report["ninja_log"] = ".ninja_log"
    if deltas is not None:
        report["cache_deltas"] = deltas
    temporary = output.with_name(output.name + ".part")
    temporary.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(output)
    return report


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv[:1] == ["run-build"]:
        parser = argparse.ArgumentParser(prog="ci_build_metrics.py run-build",
                                         description="Run one unchanged build, then report its fresh Ninja evidence")
        parser.add_argument("--repo-root", required=True)
        parser.add_argument("--build-root", required=True)
        parser.add_argument("--output", required=True, type=Path)
        parser.add_argument("--object-cache", choices=("measured", "disabled", "unmeasured"), default="measured")
        parser.add_argument("command", nargs=argparse.REMAINDER)
        args = parser.parse_args(argv[1:])
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        if not command:
            parser.error("run-build requires the exact build command after --")
        try:
            run_build(command, repo_root=args.repo_root, build_root=args.build_root,
                      output=args.output, object_cache=args.object_cache)
        except subprocess.CalledProcessError as error:
            return error.returncode or 1
        except MetricsError as error:
            print(f"error: {error}", file=sys.stderr)
            return 1
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ninja-log", required=True, type=Path)
    parser.add_argument("--compile-commands", required=True, type=Path)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--build-root", required=True)
    parser.add_argument("--build-wall-ms", type=int, help="separately measured complete build wall time")
    parser.add_argument("--object-cache", choices=("unmeasured", "disabled"), default="unmeasured",
                        help="set disabled for Windows/CodeQL; cache hits are never inferred from duration")
    args = parser.parse_args()
    try:
        report = measure(args.ninja_log.read_text(encoding="utf-8"),
                         json.loads(args.compile_commands.read_text(encoding="utf-8")),
                         args.repo_root, args.build_root, args.build_wall_ms, args.object_cache)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
