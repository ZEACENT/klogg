#!/usr/bin/env python3
"""Verify that an ADB helper release build runs on its exactly locked toolchain."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import re
import subprocess


def first_line(command: list[str], accepted_returncodes: tuple[int, ...] = (0,)) -> str:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode not in accepted_returncodes:
        raise RuntimeError(f"toolchain command failed: {' '.join(command)}")
    lines = (result.stdout or result.stderr).splitlines()
    if not lines:
        raise RuntimeError(f"toolchain command produced no identity: {' '.join(command)}")
    return lines[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--target", required=True)
    parser.add_argument("--allow-unlocked-local", action="store_true")
    parser.add_argument("--containerized", action="store_true")
    parser.add_argument("--container-image")
    args = parser.parse_args()

    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    expected = lock.get("toolchains", {}).get(args.target)
    if not isinstance(expected, dict):
        raise RuntimeError(f"unknown locked ADB toolchain: {args.target}")

    if args.containerized:
        expected_container = f"{expected.get('container_image')}@{expected.get('container_digest')}"
        if args.container_image != expected_container:
            raise RuntimeError(
                f"container image mismatch for {args.target}: expected {expected_container}, got {args.container_image}"
            )
        expected_glibc = expected.get("glibc_version")
        if not isinstance(expected_glibc, str):
            raise RuntimeError(f"containerized ADB toolchain lacks locked glibc_version: {args.target}")
        ldd_line = first_line(["ldd", "--version"])
        match = re.search(r"(\d+\.\d+)\s*$", ldd_line)
        actual_glibc = match.group(1) if match else ""
        if actual_glibc != expected_glibc:
            raise RuntimeError(
                f"container GLIBC mismatch for {args.target}: expected {expected_glibc}, got {actual_glibc or ldd_line}"
            )
        expected_arch = lock.get("targets", {}).get(args.target, {}).get("arch")
        machine = platform.machine().lower()
        actual_arch = {"amd64": "x86_64", "aarch64": "arm64"}.get(machine, machine)
        if actual_arch != expected_arch:
            raise RuntimeError(
                f"container architecture mismatch for {args.target}: expected {expected_arch}, got {actual_arch}"
            )
        print(
            f"verified digest-bound container toolchain {expected['identifier']} "
            f"on {actual_arch}; glibc={actual_glibc}"
        )
        return 0

    image_os = os.environ.get("ImageOS")
    image_version = os.environ.get("ImageVersion")
    if not image_os or not image_version:
        if args.allow_unlocked_local:
            print(f"local inspection build: skipping hosted runner identity check for {args.target}")
            return 0
        raise RuntimeError("release ADB build lacks GitHub hosted runner ImageOS/ImageVersion identity")
    if image_version != expected["runner_image_revision"]:
        raise RuntimeError(
            f"runner image revision mismatch for {args.target}: expected {expected['runner_image_revision']}, got {image_version}"
        )

    cmake_line = first_line(["cmake", "--version"])
    if expected["cmake_version"] not in cmake_line:
        raise RuntimeError(f"CMake version mismatch: expected {expected['cmake_version']}, got {cmake_line}")

    compiler = expected["compiler"]
    if compiler == "gcc":
        compiler_line = first_line(["c++", "--version"])
    elif compiler == "appleclang":
        compiler_line = first_line(["clang++", "--version"])
    elif compiler == "msvc":
        # cl.exe prints its identity and exits 2 when invoked without an input file.
        compiler_line = first_line(["cl"], accepted_returncodes=(0, 2))
    else:
        raise RuntimeError(f"unsupported locked compiler identity: {compiler}")
    if expected["compiler_version"] not in compiler_line:
        raise RuntimeError(
            f"compiler version mismatch for {args.target}: expected {expected['compiler_version']}, got {compiler_line}"
        )

    host = f"{platform.system()} {platform.machine()}"
    print(f"verified ADB helper toolchain {expected['identifier']} on {host}; image={image_os}/{image_version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
