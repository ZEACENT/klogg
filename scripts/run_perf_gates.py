#!/usr/bin/env python3
"""Run the performance gates locally.

CI never gates on wall-clock budgets: every budget assertion in the test
suite goes through KLOGG_CHECK_PERF_BUDGET (tests/helpers/test_utils.h) and
only fires when KLOGG_PERF_GATES=1 is set. This script is the developer-side
entry point that sets the variable and runs the suite against an existing
build tree.

Run it against a RelWithDebInfo (optimized, non-sanitizer) build; sanitizer
and Debug builds distort timings and are meant for correctness, not budget
enforcement.

Usage:
    python3 scripts/run_perf_gates.py [--build-dir build_root] [-- ctest args...]

Exit codes:
    0   ctest passed with perf gates enabled.
    1   ctest failed.
    2   The build directory does not exist.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys


def run_perf_gates(build_dir: pathlib.Path, extra_args: list[str] | None = None) -> int:
    if not build_dir.is_dir():
        print(f"error: build directory not found: {build_dir}", file=sys.stderr)
        return 2
    env = dict(os.environ)
    env["KLOGG_PERF_GATES"] = "1"
    command = ["ctest", "--output-on-failure", *(extra_args or [])]
    print(f"==> KLOGG_PERF_GATES=1 {' '.join(command)} (cwd={build_dir})")
    completed = subprocess.run(command, cwd=build_dir, env=env, check=False)
    return int(completed.returncode)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        default="build_root",
        help="Existing CMake build tree (default: build_root).",
    )
    parser.add_argument(
        "ctest_args",
        nargs=argparse.REMAINDER,
        help="Extra arguments forwarded to ctest after '--'.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    extra = list(args.ctest_args)
    if extra and extra[0] == "--":
        extra = extra[1:]
    return run_perf_gates(pathlib.Path(args.build_dir), extra or None)


if __name__ == "__main__":
    raise SystemExit(main())
