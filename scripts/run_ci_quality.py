#!/usr/bin/env python3
"""Run the repository's complete fast CI-quality gate through one entry point."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import time
from typing import Any


def quality_commands(root: pathlib.Path) -> list[list[str]]:
    del root
    return [
        [sys.executable, "scripts/lint_platform_fragile.py"],
        [sys.executable, "scripts/lint_linux_package_runtime.py"],
        [sys.executable, "scripts/lint_ci_quality.py"],
        [sys.executable, "scripts/lint_translation_catalogs.py"],
        [
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "-s",
            "tests/scripts",
            "-p",
            "test_*.py",
        ],
    ]


def repository_identity(root: pathlib.Path) -> tuple[str, bool]:
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=root,
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    status = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=root,
        text=True,
        capture_output=True,
        check=True,
    ).stdout
    return commit, bool(status.strip())


def run_quality_checks(root: pathlib.Path) -> dict[str, Any]:
    checks: list[dict[str, Any]] = []
    returncode = 0
    for command in quality_commands(root):
        started = time.monotonic()
        completed = subprocess.run(
            command,
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
        )
        duration = time.monotonic() - started
        checks.append(
            {
                "command": command,
                "durationSeconds": round(duration, 3),
                "returncode": completed.returncode,
                "stdout": completed.stdout,
                "stderr": completed.stderr,
            }
        )
        if completed.returncode != 0:
            returncode = completed.returncode
            break
    return {"returncode": returncode, "checks": checks}


def render_human(payload: dict[str, Any]) -> None:
    print(f"CI quality base: {payload['commit']} (dirty={str(payload['dirty']).lower()})")
    for check in payload["checks"]:
        command = " ".join(check["command"])
        print(f"\n==> {command}")
        if check["stdout"]:
            print(check["stdout"], end="" if check["stdout"].endswith("\n") else "\n")
        if check["stderr"]:
            print(
                check["stderr"],
                end="" if check["stderr"].endswith("\n") else "\n",
                file=sys.stderr,
            )
        print(
            f"<== exit {check['returncode']} in {check['durationSeconds']:.3f}s"
        )
    if payload["returncode"] == 0:
        print("\nCI quality checks passed.")
    else:
        print("\nCI quality checks failed; remaining commands were not run.", file=sys.stderr)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="Emit a JSON result receipt.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    commit, dirty = repository_identity(root)
    result = run_quality_checks(root)
    payload = {"commit": commit, "dirty": dirty, **result}
    if args.json:
        print(json.dumps(payload, indent=2))
    else:
        render_human(payload)
    return int(result["returncode"])


if __name__ == "__main__":
    raise SystemExit(main())
