#!/usr/bin/env python3
"""Exact target inventory for seven source-built native CI dependency cores."""

from __future__ import annotations

from ci_dependency_artifact import validate_archive_name
from ci_environment_registry import DEPENDENCY_REGISTRY

PRODUCER_WORKFLOW = ".github/workflows/ci-dependencies.yml"
IOS_TOOLCHAIN = {
    "xcode": ["Xcode 16.4", "Build version 16F6"],
    "sdk_version": "15.5",
    "sdk_path": "/Applications/Xcode_16.4.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
    "clang": "Apple clang version 17.0.0 (clang-1700.0.13.5)",
    "cmake": "cmake version 3.31.6",
    "ninja": "1.12.1",
}
TARGETS = {
    "adb-linux-x86_64": ("adb-helper", "linux-x86_64", "ubuntu-24.04"),
    "adb-linux-arm64": ("adb-helper", "linux-arm64", "ubuntu-24.04-arm"),
    "adb-windows-x86_64": ("adb-helper", "windows-x86_64", "windows-2022"),
    "adb-macos-x86_64": ("adb-helper", "macos-x86_64", "macos-15-intel"),
    "adb-macos-arm64": ("adb-helper", "macos-arm64", "macos-15"),
    "ios-x86_64": ("ios-native", "x86_64", "macos-15-intel"),
    "ios-arm64": ("ios-native", "arm64", "macos-15"),
}


class DependencyCatalogError(ValueError):
    """The project has no reviewed policy for this dependency target."""


def validate_catalog(document: object) -> dict[str, dict]:
    if (
        not isinstance(document, dict)
        or set(document) != {
            "schema_version", "kind", "registry", "producer_workflow", "targets"
        }
        or type(document["schema_version"]) is not int
        or document["schema_version"] != 1
        or document["kind"] != "native-dependency-targets"
        or document["registry"] != DEPENDENCY_REGISTRY
        or document["producer_workflow"] != PRODUCER_WORKFLOW
    ):
        raise DependencyCatalogError("invalid native dependency catalog identity")
    targets = document["targets"]
    if not isinstance(targets, dict) or set(targets) != set(TARGETS):
        raise DependencyCatalogError("catalog must cover exactly seven native targets")
    for name, row in targets.items():
        component, target, runner = TARGETS[name]
        archive_name = f"{component}-{target}.tar.gz"
        fields = {"component", "target", "runner", "archive_name"}
        if component == "ios-native":
            fields.add("toolchain")
        if (
            not isinstance(row, dict)
            or set(row) != fields
            or row["component"] != component
            or row["target"] != target
            or row["runner"] != runner
            or row["archive_name"] != archive_name
            or (component == "ios-native" and row["toolchain"] != IOS_TOOLCHAIN)
        ):
            raise DependencyCatalogError(f"invalid native target policy: {name}")
        validate_archive_name(row["archive_name"])
    return targets
