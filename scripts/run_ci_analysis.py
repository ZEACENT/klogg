#!/usr/bin/env python3
"""Run ordinary CI phases using the unchanged qualification engine's primitives.

The signed engine exposes complete static/coverage roles, not independent phases.
Ordinary CI needs changed-only analysis and a separately transported coverage
ratchet, so this adapter owns that sequencing. Configure, environment, execution,
first-party filtering and cppcheck policy come from the original engine. Command
parity tests protect the remaining orchestration until a future genuinely
qualified engine can expose phase APIs. This adapter neither qualifies an image
nor emits qualification evidence; it is not an approved verifier in the lock.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys

import qualify_ci_analysis as engine


PHASES = ("configure", "static-prepare", "static-tidy", "static-cppcheck",
          "coverage-build", "coverage-test", "coverage-report", "coverage-ratchet")


def require_base(base):
    engine.require(isinstance(base, str) and re.fullmatch(r"[0-9a-f]{40}", base) is not None
                   and base != "0" * 40, "analysis requires a full nonzero base SHA")


def configure(role, workspace, build, runner, environment, document, *, cpm_source_cache=None):
    # Adapt only this runtime path, never the canonical profile or engine source.
    # The private coverage clone consumes the original checkout's prefetched cache.
    def configure_runner(command, **kwargs):
        if cpm_source_cache is not None:
            expected = "-DCPM_SOURCE_CACHE=" + str(workspace / "cpm_cache")
            engine.require(command.count(expected) == 1, "configure requires the canonical CPM cache option")
            command = ["-DCPM_SOURCE_CACHE=" + str(cpm_source_cache) if value == expected else value
                       for value in command]
        return runner(command, **kwargs)
    engine.configure(role, workspace, build, configure_runner, environment, document)


def static_database(workspace, build):
    database = engine.read_json(build / "compile_commands.json")
    engine.require(isinstance(database, list), "compile database must be an array")
    units = engine.first_party_compile_units(database, workspace / "src")
    engine.require(bool(units), "static analysis requires first-party translation units")
    return database, units


def static_phase(phase, workspace, build, mode, base, jobs, clang_tidy_diff, run):
    if phase == "static-prepare":
        run(["cmake", "--build", build, "--target", "klogg_ui_autogen", "generate_version", "-j" + str(jobs)])
        return
    engine.require(mode in ("full-report", "changed-strict"), "static analysis requires an explicit analysis mode")
    strict = mode == "changed-strict"
    if strict:
        require_base(base)
    if phase == "static-tidy" and strict:
        command = ["python3", workspace / "scripts/run_changed_clang_tidy.py", "--base", base,
                   "--build-dir", build, "--jobs", str(jobs)]
        if clang_tidy_diff is not None:
            command += ["--clang-tidy-diff", clang_tidy_diff]
        run(command)
        return
    database, units = static_database(workspace, build)
    if phase == "static-tidy":
        def tidy(path):
            return run(["clang-tidy", "-p", build, "--extra-arg=-Wno-unknown-warning-option", path])
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            list(pool.map(tidy, units))
        return
    selected = units
    if strict:
        changed = run(["git", "-c", "core.quotePath=false", "diff", "--name-only", "-z", "--diff-filter=d",
                       base, "HEAD", "--", "src/"], emit_stdout=False).stdout.split("\0")
        headers = any(pathlib.Path(path).suffix.lower() in (".h", ".hh", ".hpp", ".hxx") for path in changed)
        if not headers:
            selected = sorted({(workspace / path).resolve() for path in changed
                               if pathlib.Path(path).suffix.lower() in (".c", ".cc", ".cpp", ".cxx")})
    if selected:
        project = build / ("cppcheck-changed.json" if strict else "cppcheck-full.json")
        engine._filtered_database(database, selected, project, workspace)
        run(engine._cppcheck(workspace, project, strict))


def coverage_phase(phase, workspace, build, report, base, jobs, run):
    if phase == "coverage-build":
        run(["cmake", "--build", build, "-t", "klogg", "klogg_grep", "klogg_test_build", "-j" + str(jobs)])
    elif phase == "coverage-test":
        run(["ctest", "--output-on-failure", "--parallel", str(jobs)], cwd=build)
    elif phase == "coverage-report":
        report.mkdir(parents=True, exist_ok=True)
        common = ["gcovr", "--root", workspace, "--filter", "^src/", "--gcov-ignore-parse-errors", "negative_hits.warn_once_per_file"]
        summary = run(common + ["--html-details", "--html-self-contained", "--html-title", "Klogg line/branch coverage",
                                "-o", report / "index.html", "--print-summary", build]).stdout
        (report / "summary.txt").write_text(summary, encoding="utf-8")
        run(common + ["--json", "--json-pretty", "-o", report / "coverage.json", build])
    else:
        require_base(base)
        run(["python3", workspace / "scripts/enforce_coverage_ratchet.py", "--summary", report / "summary.txt",
             "--line-floor", ".github/baselines/coverage-line.txt", "--branch-floor", ".github/baselines/coverage-branch.txt",
             "--base-sha", base])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("phase", choices=PHASES)
    parser.add_argument("--role", choices=("static", "coverage"), required=True)
    parser.add_argument("--workspace", type=pathlib.Path, required=True)
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--cpm-source-cache", type=pathlib.Path)
    parser.add_argument("--analysis-mode", choices=("full-report", "changed-strict"))
    parser.add_argument("--analysis-base-sha")
    parser.add_argument("--clang-tidy-diff")
    parser.add_argument("--report-dir", type=pathlib.Path)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    args = parser.parse_args(argv)
    try:
        engine.require(args.jobs > 0, "analysis jobs must be positive")
        engine.require(args.phase == "configure" or args.phase.startswith(args.role + "-"),
                       "analysis phase does not match its role")
        workspace = args.workspace.resolve()
        document, environment = engine.role_environment(workspace, args.role)
        build = args.build_dir.resolve() if args.build_dir is not None else None
        report = args.report_dir.resolve() if args.report_dir is not None else None
        if args.phase in ("coverage-report", "coverage-ratchet"):
            engine.require(report is not None, "coverage reporting requires --report-dir")
        if args.phase != "coverage-ratchet":
            engine.require(build is not None, "ordinary analysis phase requires --build-dir")
        if args.phase == "configure":
            cache = args.cpm_source_cache.resolve() if args.cpm_source_cache is not None else None
            configure(args.role, workspace, build, subprocess.run, environment, document, cpm_source_cache=cache)
            return 0
        if args.phase != "coverage-ratchet":
            engine.require(build.is_dir(), "analysis build directory has not been configured")

        def run(command, *, cwd=workspace, emit_stdout=True):
            return engine.execute(subprocess.run, command, cwd=cwd, env=environment, emit_stdout=emit_stdout)

        if args.role == "static":
            static_phase(args.phase, workspace, build, args.analysis_mode, args.analysis_base_sha,
                         args.jobs, args.clang_tidy_diff, run)
        else:
            coverage_phase(args.phase, workspace, build, report, args.analysis_base_sha, args.jobs, run)
        return 0
    except (engine.ci.ContractError, OSError, ValueError, RuntimeError) as error:
        print("run_ci_analysis: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
