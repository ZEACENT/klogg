#!/usr/bin/env python3
"""Check Linux release-package CI keeps runtime CPU compatibility covered."""

from __future__ import annotations

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github" / "workflows" / "ci-build.yml"
THIRDPARTY_CMAKE = ROOT / "3rdparty" / "CMakeLists.txt"
VECTORSCAN_FLAGS_PATCH = ROOT / "3rdparty" / "patches" / "fix_vectorscan_msvc_warning_flags.patch"


def linux_matrix_entries(workflow_text: str) -> list[dict[str, str]]:
    entries: list[dict[str, str]] = []
    current: dict[str, str] | None = None

    for raw_line in workflow_text.splitlines():
        stripped = raw_line.strip()

        if stripped.startswith("- os: "):
            if current and current.get("os", "").startswith("ubuntu_"):
                entries.append(current)
            current = {"os": stripped.split(":", 1)[1].strip()}
            continue

        if current is None:
            continue

        if stripped and not stripped.startswith("#") and ":" in stripped:
            key, value = stripped.split(":", 1)
            current[key.strip()] = value.strip().strip('"')

    if current and current.get("os", "").startswith("ubuntu_"):
        entries.append(current)

    return entries


def main() -> int:
    workflow_text = WORKFLOW.read_text(encoding="utf-8")
    thirdparty_cmake = THIRDPARTY_CMAKE.read_text(encoding="utf-8")
    vectorscan_patch = VECTORSCAN_FLAGS_PATCH.read_text(encoding="utf-8")
    failures: list[str] = []

    for entry in linux_matrix_entries(workflow_text):
        package_suffix = entry.get("package_suffix")
        if package_suffix not in {"deb", "AppImage"}:
            continue

        os_name = entry.get("os", "<unknown>")
        cmake_opts = entry.get("cmake_opts", "")
        if "-DKLOGG_GENERIC_CPU=ON" not in cmake_opts:
            failures.append(f"{os_name}: release packages must set -DKLOGG_GENERIC_CPU=ON")

        if package_suffix == "deb":
            check_command = entry.get("check_command", "")
            has_real_install = ("apt install" in check_command) or ("apt-get install" in check_command)
            if not has_real_install or "--dry-run" in check_command:
                failures.append(f"{os_name}: deb package check must perform a real install")
            if "klogg -v" not in check_command:
                failures.append(f"{os_name}: deb package check must execute klogg -v")

    if "if(KLOGG_GENERIC_CPU)" not in thirdparty_cmake:
        failures.append("Vectorscan build must branch on KLOGG_GENERIC_CPU")
    if "set(VECTORSCAN_BUILD_AVX2 OFF)" not in thirdparty_cmake:
        failures.append("KLOGG_GENERIC_CPU must disable Vectorscan AVX2")
    if "set(VECTORSCAN_BUILD_AVX512 OFF)" not in thirdparty_cmake:
        failures.append("KLOGG_GENERIC_CPU must disable Vectorscan AVX512")
    if "KLOGG_GENERIC_CPU" not in vectorscan_patch or "GNUCC_ARCH x86-64" not in vectorscan_patch:
        failures.append("Vectorscan patch must force x86-64 architecture for KLOGG_GENERIC_CPU")

    if failures:
        print("Linux package runtime lint failed:")
        for failure in failures:
            print(f" - {failure}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
