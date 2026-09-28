#!/usr/bin/env python3
"""Reject unreviewed macOS iOS producer tools before the expensive source build."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys

from ci_dependency_catalog import validate_catalog

DEVELOPER_DIR = "/Applications/Xcode_26.6.app/Contents/Developer"
COMMANDS = {
    "xcode": ("xcodebuild", "-version"),
    "sdk_version": ("xcrun", "--show-sdk-version"),
    "sdk_path": ("xcrun", "--show-sdk-path"),
    "clang": ("clang", "--version"),
    "cmake": ("cmake", "--version"),
    "ninja": ("ninja", "--version"),
}
HOST_TOOLS = ("autoreconf", "autoconf", "automake", "aclocal", "glibtoolize",
              "pkg-config", "perl", "m4")
BREW_FORMULAS = ("autoconf", "automake", "libtool", "pkgconf", "m4", "perl")


class ToolchainError(ValueError):
    """This host cannot produce a qualified native iOS binary core."""


def check_ios_toolchain(*, target_id: str, repo_root: pathlib.Path, host_arch: str,
                        developer_dir: str, run_command=subprocess.run) -> dict:
    """Observe selected build tools; a receipt's self-report is insufficient."""
    try:
        document = json.loads((pathlib.Path(repo_root) / "ci/dependencies/catalog.json")
                              .read_text(encoding="utf-8"))
        targets = validate_catalog(document)
    except (OSError, ValueError) as error:
        raise ToolchainError("invalid native dependency catalog") from error
    if (target_id not in ("ios-arm64", "ios-x86_64")
            or host_arch != targets[target_id]["target"]
            or developer_dir != DEVELOPER_DIR):
        raise ToolchainError("unapproved native iOS architecture or Xcode selection")
    expected = targets[target_id]["toolchain"]
    environment = {**os.environ, "DEVELOPER_DIR": developer_dir}
    observed = {}
    try:
        for field, command in COMMANDS.items():
            result = run_command(list(command), check=True, timeout=30,
                                 capture_output=True, text=True, env=environment)
            lines = result.stdout.strip().splitlines()
            if field == "xcode":
                observed[field] = lines
            elif field in ("clang", "cmake"):
                observed[field] = lines[0] if lines else ""
            else:
                observed[field] = result.stdout.strip()
    except (OSError, subprocess.SubprocessError) as error:
        raise ToolchainError("unable to observe pinned iOS producer tools") from error
    if observed != expected:
        raise ToolchainError(f"iOS producer toolchain mismatch: expected {expected!r}, observed {observed!r}")
    return observed


def observe_ios_host_tools(*, target_id: str, host_arch: str, developer_dir: str,
                           find_executable=shutil.which, run_command=subprocess.run) -> dict:
    """Report actual host tools for review, never as qualification evidence."""
    if (target_id not in ("ios-arm64", "ios-x86_64")
            or host_arch != target_id[len("ios-"):]
            or developer_dir != DEVELOPER_DIR):
        raise ToolchainError("unapproved native iOS architecture or Xcode selection")
    environment = {**os.environ, "DEVELOPER_DIR": developer_dir,
                   "HOMEBREW_NO_AUTO_UPDATE": "1"}
    tools = {}
    try:
        for name in HOST_TOOLS:
            selected = find_executable(name)
            if not selected or not pathlib.Path(selected).is_absolute():
                raise ToolchainError("missing selected iOS host tool: " + name)
            executable = pathlib.Path(selected).resolve(strict=True)
            if (not executable.is_file() or not executable.stat().st_mode & 0o111
                    or executable.stat().st_size > 128 * 1024 * 1024):
                raise ToolchainError("unsafe selected iOS host tool: " + name)
            digest = hashlib.sha256()
            with executable.open("rb") as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(chunk)
            result = run_command([selected, "--version"], check=True, timeout=30,
                                 capture_output=True, text=True, env=environment)
            lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
            if not lines:
                raise ToolchainError("host tool did not report a version: " + name)
            tools[name] = {"path": selected, "resolved_path": str(executable),
                           "sha256": digest.hexdigest(), "version": lines[0]}
        homebrew = {}
        for formula in BREW_FORMULAS:
            result = run_command(["brew", "list", "--versions", formula], check=False,
                                 timeout=30, capture_output=True, text=True, env=environment)
            homebrew[formula] = result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.SubprocessError) as error:
        raise ToolchainError("cannot observe native iOS host tool closure") from error
    return {"schema_version": 1, "kind": "unreviewed-ios-host-tools",
            "target_id": target_id, "host_arch": host_arch,
            "developer_dir": developer_dir,
            "runner_image": {"os": os.environ.get("ImageOS"),
                             "version": os.environ.get("ImageVersion")},
            "tools": tools, "homebrew": homebrew}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-id", required=True, choices=("ios-arm64", "ios-x86_64"))
    parser.add_argument("--repo-root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--probe-unreviewed-host-tools", action="store_true")
    args = parser.parse_args(argv)
    host_arch = platform.machine()
    developer_dir = os.environ.get("DEVELOPER_DIR", "")
    try:
        observed = check_ios_toolchain(
            target_id=args.target_id, repo_root=args.repo_root,
            host_arch=host_arch, developer_dir=developer_dir,
        )
        if args.probe_unreviewed_host_tools:
            host_tools = observe_ios_host_tools(
                target_id=args.target_id, host_arch=host_arch,
                developer_dir=developer_dir,
            )
            print(json.dumps(host_tools, sort_keys=True), flush=True)
            parser.exit(1, "native iOS host tool closure remains unreviewed; qualification blocked\n")
    except ToolchainError as error:
        parser.exit(1, f"native iOS toolchain rejected: {error}\n")
    print(json.dumps(observed, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
