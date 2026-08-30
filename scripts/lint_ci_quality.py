#!/usr/bin/env python3
"""Reject CI patterns that silently weaken quality gates."""

from __future__ import annotations

import fnmatch
import math
import re
import shlex
import sys
from pathlib import Path


CHECKOUT_SHA_RE = re.compile(r"actions/checkout@[0-9a-f]{40}")
REMOTE_ACTION_SHA_RE = re.compile(r"[^\s@]+@[0-9a-f]{40}")
CODEQL_ACTION_SHA_RE = re.compile(
    r"github/codeql-action/(?P<action>init|analyze)@(?P<sha>[0-9a-f]{40})"
)
CODEQL_CURRENT_SHA = "cdf488f595d80d6e07e03d4674febd5ab45fa938"
REVIEWED_ACTION_REVISIONS = {
    "actions/attest-build-provenance": "4d101475d8b20a2381f78447822ac1eab6504dd8",
    "actions/cache": "55cc8345863c7cc4c66a329aec7e433d2d1c52a9",
    "actions/checkout": "3d3c42e5aac5ba805825da76410c181273ba90b1",
    "actions/download-artifact": "3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c",
    "actions/setup-python": "5fda3b95a4ea91299a34e894583c3862153e4b97",
    "actions/upload-artifact": "043fb46d1a93c77aae656e7c1c64a875d1fc6a0a",
    "apple-actions/import-codesign-certs": "5142e029c445c10ffc7149d172e540235a065466",
    "dawidd6/action-download-artifact": "d63b86af1b34672e53c440b1b83979861906bad7",
    "docker/build-push-action": "53b7df96c91f9c12dcc8a07bcb9ccacbed38856a",
    "docker/setup-buildx-action": "bb05f3f5519dd87d3ba754cc423b652a5edd6d2c",
    "github/codeql-action": "cdf488f595d80d6e07e03d4674febd5ab45fa938",
    "ilshidur/action-discord": "d2594079a10f1d6739ee50a2471f0ca57418b554",
    "joncloud/makensis-action": "971ef20f43e4f9f3af2c7f276cb7348d033da1cd",
    "jurplel/install-qt-action": "48d3ad6db93f3627c8ee7a0454bc6f3744f7e730",
    "lukka/get-cmake": "fffaaafeea488556c2c12dad60690008bc1caacb",
    "mathrix-education/setup-sentry-cli": "ff4fff773f5e628fa214ebd19e46fb7289454ee2",
    "msys2/setup-msys2": "66cd2cce69caa17b53920067426061ca1de3a884",
    "softprops/action-gh-release": "efb35369e0ad2afab669f228072c1b0d510eae64",
}

KNOWN_NODE20_ACTION_REFS = {
    "actions/checkout@11d5960a326750d5838078e36cf38b85af677262",
    "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
    "actions/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093",
    "github/codeql-action/init@4187e74d05793876e9989daffde9c3e66b4acd07",
    "github/codeql-action/analyze@4187e74d05793876e9989daffde9c3e66b4acd07",
    "ilammy/msvc-dev-cmd@0b201ec74fa43914dc39ae48a89fd1d8cb592756",
}
LIST_ITEM_RE = re.compile(r"^(?P<indent>\s*)-(?:\s+|$)")
KEY_VALUE_RE = re.compile(r"^(?P<indent>\s*)(?:-\s*)?(?P<key>[\w-]+):\s*(?P<value>.*)$")
BROAD_SUPPRESSION_RE = re.compile(
    r"^(?:race|leak):.*\*(?:Qt|tbb|mimalloc|glib)\*", re.MULTILINE
)
BROAD_CPPCHECK_IDS = (
    "functionStatic",
    "noExplicitConstructor",
    "returnByReference",
    "constParameterPointer",
    "constParameterReference",
    "constVariable",
    "shadowFunction",
    "variableScope",
    "unreadVariable",
    "assignBoolToFloat",
    "useStlAlgorithm",
    "useInitializationList",
    "missingOverride",
)

CI_BUILD_JOB_NEEDS = {
    "ReleaseQualificationPreflight": set(),
    "SaveVersion": {"ReleaseQualificationPreflight"},
    "PrefetchCpmCache": {"ReleaseQualificationPreflight"},
    "PrefetchBoost": {"ReleaseQualificationPreflight"},
    "PrefetchOpenSsl": {"ReleaseQualificationPreflight"},
    "PrefetchLinuxDeployQt": {"ReleaseQualificationPreflight"},
    "PrefetchCmakeInstaller": {"ReleaseQualificationPreflight"},
    "PrefetchWindowsTools": {"ReleaseQualificationPreflight"},
    "PrefetchAdbHelperSources": {"ReleaseQualificationPreflight"},
    "PrefetchIosNativeSources": {"ReleaseQualificationPreflight"},
    "BuildAdbHelperLegalAssets": {
        "SaveVersion",
        "PrefetchAdbHelperSources",
    },
    "BuildAdbHelpers": {
        "PrefetchAdbHelperSources",
        "BuildAdbHelperLegalAssets",
    },
    "BuildAdbLinuxArm64": {
        "PrefetchAdbHelperSources",
        "BuildAdbHelperLegalAssets",
    },
    "BuildAdbWindowsX64": {
        "PrefetchAdbHelperSources",
        "BuildAdbHelperLegalAssets",
    },
    "BuildAdbMacX64": {
        "PrefetchAdbHelperSources",
        "BuildAdbHelperLegalAssets",
    },
    "BuildAdbMacArm64": {
        "PrefetchAdbHelperSources",
        "BuildAdbHelperLegalAssets",
    },
    "BuildIosNativeStacks": {
        "SaveVersion",
        "PrefetchIosNativeSources",
    },
    "BuildIosNativeArm64": {
        "SaveVersion",
        "PrefetchIosNativeSources",
    },
    "LinuxPackages": {
        "SaveVersion",
        "PrefetchCpmCache",
        "PrefetchLinuxDeployQt",
        "PrefetchCmakeInstaller",
        "BuildAdbHelpers",
    },
    "LinuxSanitizers": {
        "SaveVersion",
        "PrefetchCpmCache",
        "PrefetchCmakeInstaller",
    },
    "LinuxTsan": {
        "SaveVersion",
        "PrefetchCpmCache",
    },
    "MacPackages": {
        "SaveVersion",
        "PrefetchCpmCache",
        "PrefetchBoost",
        "BuildAdbMacX64",
        "BuildIosNativeStacks",
    },
    "MacArmPackages": {
        "SaveVersion",
        "PrefetchCpmCache",
        "PrefetchBoost",
        "BuildAdbMacArm64",
        "BuildIosNativeArm64",
    },
    "MacSanitizers": {
        "SaveVersion",
        "PrefetchCpmCache",
        "PrefetchBoost",
    },
    "WindowsPackages": {
        "SaveVersion",
        "PrefetchCpmCache",
        "PrefetchBoost",
        "PrefetchWindowsTools",
        "BuildAdbWindowsX64",
    },
    "WindowsX86": {
        "SaveVersion",
        "PrefetchCpmCache",
        "PrefetchBoost",
        "PrefetchOpenSsl",
        "PrefetchWindowsTools",
    },
    "WindowsAsan": {
        "SaveVersion",
        "PrefetchCpmCache",
        "PrefetchBoost",
        "PrefetchWindowsTools",
    },
    "ci-gate": {
        "BuildAdbHelpers",
        "BuildAdbLinuxArm64",
        "BuildAdbWindowsX64",
        "BuildAdbMacX64",
        "BuildAdbMacArm64",
        "BuildIosNativeStacks",
        "BuildIosNativeArm64",
        "LinuxPackages",
        "LinuxSanitizers",
        "LinuxTsan",
        "MacPackages",
        "MacArmPackages",
        "MacSanitizers",
        "WindowsPackages",
        "WindowsX86",
        "WindowsAsan",
    },
}

CI_BUILD_ARTIFACT_PRODUCERS = {
    "klogg_version": "SaveVersion",
    "cpm-cache": "PrefetchCpmCache",
    "boost-root": "PrefetchBoost",
    "openssl-archive": "PrefetchOpenSsl",
    "linuxdeployqt": "PrefetchLinuxDeployQt",
    "cmake-installer": "PrefetchCmakeInstaller",
    "msys2-tools": "PrefetchWindowsTools",
    "adb-helper-source-cache": "PrefetchAdbHelperSources",
    "adb-helper-legal-assets": "BuildAdbHelperLegalAssets",
    "ios-native-source-cache": "PrefetchIosNativeSources",
    "ios-native-source-assets": "BuildIosNativeArm64",
}

CI_BUILD_REQUIRED_ARTIFACT_CONSUMERS = {
    "cpm-cache": {
        "LinuxPackages",
        "LinuxSanitizers",
        "LinuxTsan",
        "MacPackages",
        "MacArmPackages",
        "MacSanitizers",
        "WindowsPackages",
        "WindowsX86",
        "WindowsAsan",
    },
    "boost-root": {
        "MacPackages",
        "MacArmPackages",
        "MacSanitizers",
        "WindowsPackages",
        "WindowsX86",
        "WindowsAsan",
    },
    "openssl-archive": {"WindowsX86"},
    "linuxdeployqt": {"LinuxPackages"},
    "cmake-installer": {"LinuxPackages", "LinuxSanitizers"},
    "msys2-tools": {"WindowsPackages", "WindowsX86", "WindowsAsan"},
    "adb-helper-source-cache": {
        "BuildAdbHelperLegalAssets",
        "BuildAdbHelpers",
        "BuildAdbLinuxArm64",
        "BuildAdbWindowsX64",
        "BuildAdbMacX64",
        "BuildAdbMacArm64",
    },
    "adb-helper-legal-assets": {
        "BuildAdbHelpers",
        "BuildAdbLinuxArm64",
        "BuildAdbWindowsX64",
        "BuildAdbMacX64",
        "BuildAdbMacArm64",
        "LinuxPackages",
        "MacPackages",
        "MacArmPackages",
        "WindowsPackages",
    },
    "ios-native-source-cache": {"BuildIosNativeStacks", "BuildIosNativeArm64"},
}

CI_BUILD_ARTIFACT_CONDITIONS = {
    ("BuildIosNativeArm64", "uploads", "ios-native-source-assets"):
        "${{ matrix.architecture == 'arm64' }}",
    ("LinuxPackages", "downloads", "linuxdeployqt"):
        "${{ matrix.config.os == 'ubuntu_appimage' }}",
    ("LinuxPackages", "downloads", "cmake-installer"):
        "${{ matrix.config.sanitizer != 'thread' }}",
    ("LinuxSanitizers", "downloads", "cmake-installer"):
        "${{ matrix.config.sanitizer != 'thread' }}",
    ("LinuxPackages", "downloads", "adb-helper-legal-assets"):
        "${{ matrix.config.package != false }}",
    ("MacPackages", "downloads", "adb-helper-legal-assets"):
        "${{ matrix.config.package != false }}",
    ("MacArmPackages", "downloads", "adb-helper-legal-assets"):
        "${{ matrix.config.package != false }}",
    ("WindowsX86", "downloads", "openssl-archive"):
        "${{ startswith(matrix.config.qt_version, '5') }}",
    ("WindowsPackages", "downloads", "adb-helper-legal-assets"):
        "${{ matrix.config.package != false }}",
}


def strip_yaml_comment(value: str) -> str:
    quote: str | None = None
    for index, char in enumerate(value):
        if char in "'\"":
            if quote is None:
                quote = char
            elif quote == char:
                quote = None
        elif char == "#" and quote is None and (index == 0 or value[index - 1].isspace()):
            return value[:index].rstrip()
    return value.strip()


def scalar(value: str) -> str:
    value = strip_yaml_comment(value).strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
        return value[1:-1]
    return value


def yaml_value(block: list[str], offset: int, value: str, indent: int) -> str:
    candidate = scalar(value)
    if candidate not in ("|", "|-", "|+", ">", ">-", ">+"):
        return candidate

    content: list[str] = []
    for following in block[offset + 1 :]:
        if following.strip():
            following_indent = len(following) - len(following.lstrip())
            if following_indent <= indent:
                break
            content.append(following.strip())
        else:
            content.append("")
    separator = " " if candidate.startswith(">") else "\n"
    return separator.join(content).strip()


def workflow_run_blocks(text: str) -> list[str]:
    lines = text.splitlines()
    blocks: list[str] = []
    for index, line in enumerate(lines):
        entry = KEY_VALUE_RE.match(line)
        if entry is None or entry.group("key") != "run":
            continue
        indent = len(entry.group("indent"))
        value = yaml_value(lines, index, entry.group("value"), indent)
        active_lines = []
        for run_line in value.splitlines():
            active = strip_yaml_comment(run_line)
            if active:
                active_lines.append(active)
        blocks.append("\n".join(active_lines))
    return blocks


def workflow_job_blocks(text: str) -> dict[str, list[str]]:
    lines = text.splitlines()
    jobs_index: int | None = None
    jobs_indent = 0
    for index, line in enumerate(lines):
        entry = KEY_VALUE_RE.match(line)
        if entry is not None and entry.group("key") == "jobs":
            jobs_index = index
            jobs_indent = len(entry.group("indent"))
            break
    if jobs_index is None:
        return {}

    job_starts: list[tuple[int, str, int]] = []
    job_indent: int | None = None
    for index in range(jobs_index + 1, len(lines)):
        line = lines[index]
        active = strip_yaml_comment(line)
        if not active:
            continue
        indent = len(line) - len(line.lstrip())
        if indent <= jobs_indent:
            break
        entry = KEY_VALUE_RE.match(line)
        if entry is None:
            continue
        if job_indent is None:
            job_indent = indent
        if indent == job_indent:
            job_starts.append((index, entry.group("key"), indent))

    blocks: dict[str, list[str]] = {}
    for offset, (start, name, _) in enumerate(job_starts):
        end = job_starts[offset + 1][0] if offset + 1 < len(job_starts) else len(lines)
        blocks[name] = lines[start:end]
    return blocks


def parse_inline_yaml_list(value: str) -> set[str] | None:
    candidate = scalar(value)
    if not candidate.startswith("[") or not candidate.endswith("]"):
        return None
    inner = candidate[1:-1].strip()
    if not inner:
        return set()
    return {scalar(item.strip()) for item in inner.split(",") if item.strip()}


def workflow_job_needs(text: str) -> dict[str, set[str]]:
    result: dict[str, set[str]] = {}
    for name, block in workflow_job_blocks(text).items():
        header = KEY_VALUE_RE.match(block[0])
        if header is None:
            continue
        job_indent = len(header.group("indent"))
        needs: set[str] = set()
        found = False
        for offset, line in enumerate(block[1:], start=1):
            entry = KEY_VALUE_RE.match(line)
            if entry is None or entry.group("key") != "needs":
                continue
            indent = len(entry.group("indent"))
            if indent != job_indent + 2 or found:
                continue
            found = True
            value = scalar(entry.group("value"))
            inline = parse_inline_yaml_list(value)
            if inline is not None:
                needs = inline
                continue
            if value:
                needs = {value}
                continue
            for following in block[offset + 1 :]:
                active = strip_yaml_comment(following)
                if not active:
                    continue
                following_indent = len(following) - len(following.lstrip())
                if following_indent <= indent:
                    break
                item = LIST_ITEM_RE.match(following)
                if item is None:
                    continue
                item_value = scalar(following[item.end() :])
                if item_value:
                    needs.add(item_value)
        result[name] = needs
    return result


def workflow_job_direct_value(block: list[str], key: str) -> str | None:
    if not block:
        return None
    header = KEY_VALUE_RE.match(block[0])
    if header is None:
        return None
    expected_indent = len(header.group("indent")) + 2
    for offset, line in enumerate(block[1:], start=1):
        entry = KEY_VALUE_RE.match(line)
        if (
            entry is not None
            and entry.group("key") == key
            and len(entry.group("indent")) == expected_indent
        ):
            return yaml_value(block, offset, entry.group("value"), expected_indent)
    return None


def workflow_job_ancestors(needs: dict[str, set[str]], job: str) -> set[str]:
    ancestors: set[str] = set()
    visiting: set[str] = set()

    def visit(current: str) -> None:
        if current in visiting:
            raise ValueError(f"workflow dependency cycle includes {current}")
        visiting.add(current)
        for dependency in needs.get(current, set()):
            if dependency not in needs:
                raise ValueError(f"workflow job {current} needs unknown job {dependency}")
            if dependency not in ancestors:
                ancestors.add(dependency)
                visit(dependency)
        visiting.remove(current)

    visit(job)
    return ancestors


def workflow_step_blocks(job_block: list[str]) -> list[list[str]]:
    if not job_block:
        return []
    header = KEY_VALUE_RE.match(job_block[0])
    if header is None:
        return []
    job_indent = len(header.group("indent"))
    steps_index: int | None = None
    steps_indent = 0
    for index, line in enumerate(job_block[1:], start=1):
        entry = KEY_VALUE_RE.match(line)
        if (
            entry is not None
            and entry.group("key") == "steps"
            and len(entry.group("indent")) == job_indent + 2
        ):
            steps_index = index
            steps_indent = len(entry.group("indent"))
            break
    if steps_index is None:
        return []

    step_starts: list[int] = []
    step_indent: int | None = None
    region_end = len(job_block)
    for index in range(steps_index + 1, len(job_block)):
        line = job_block[index]
        active = strip_yaml_comment(line)
        if not active:
            continue
        indent = len(line) - len(line.lstrip())
        if indent <= steps_indent:
            region_end = index
            break
        item = LIST_ITEM_RE.match(line)
        if item is None:
            continue
        if step_indent is None:
            step_indent = len(item.group("indent"))
        if len(item.group("indent")) == step_indent:
            step_starts.append(index)

    result: list[list[str]] = []
    for offset, start in enumerate(step_starts):
        end = step_starts[offset + 1] if offset + 1 < len(step_starts) else region_end
        result.append(job_block[start:end])
    return result


def workflow_job_matrix_values(block: list[str]) -> dict[str, set[str]]:
    if not block:
        return {}
    header = KEY_VALUE_RE.match(block[0])
    if header is None:
        return {}
    job_indent = len(header.group("indent"))
    matrix_index: int | None = None
    matrix_indent = 0
    for index, line in enumerate(block[1:], start=1):
        entry = KEY_VALUE_RE.match(line)
        if (
            entry is not None
            and entry.group("key") == "matrix"
            and len(entry.group("indent")) == job_indent + 4
        ):
            matrix_index = index
            matrix_indent = len(entry.group("indent"))
            break
    if matrix_index is None:
        return {}

    values: dict[str, set[str]] = {}
    for line in block[matrix_index + 1 :]:
        active = strip_yaml_comment(line)
        if not active:
            continue
        indent = len(line) - len(line.lstrip())
        if indent <= matrix_indent:
            break
        entry = KEY_VALUE_RE.match(line)
        if entry is None or entry.group("key") in {"config", "include", "exclude"}:
            continue
        value = scalar(entry.group("value")).lower()
        values.setdefault(entry.group("key"), set()).add(value)
    return values


def workflow_job_is_package_free(block: list[str]) -> bool:
    return workflow_job_matrix_values(block).get("package") == {"false"}


def artifact_condition_is_statically_false(
    condition: str | None, matrix_values: dict[str, set[str]]
) -> bool:
    if condition is None or "||" in condition:
        return False

    equality = re.compile(
        r"matrix\.(?:config\.)?(?P<key>[\w-]+)\s*(?P<operator>==|!=)\s*"
        r"(?P<value>'[^']*'|\"[^\"]*\"|true|false)"
    )
    for match in equality.finditer(condition):
        values = matrix_values.get(match.group("key"))
        if not values:
            continue
        expected = scalar(match.group("value")).lower()
        if match.group("operator") == "==" and all(
            value != expected for value in values
        ):
            return True
        if match.group("operator") == "!=" and all(
            value == expected for value in values
        ):
            return True

    startswith = re.compile(
        r"startswith\(matrix\.(?:config\.)?(?P<key>[\w-]+),\s*"
        r"(?P<prefix>'[^']*'|\"[^\"]*\")\)"
    )
    for match in startswith.finditer(condition):
        values = matrix_values.get(match.group("key"))
        prefix = scalar(match.group("prefix")).lower()
        if values and all(not value.startswith(prefix) for value in values):
            return True

    return False


def workflow_job_steps(
    text: str,
) -> dict[str, list[list[str]]]:
    job_blocks = workflow_job_blocks(text)
    anchors: dict[str, list[list[str]]] = {}
    direct_steps: dict[str, list[list[str]]] = {}
    aliases: dict[str, str] = {}

    for job, block in job_blocks.items():
        steps_value = workflow_job_direct_value(block, "steps")
        if steps_value is None:
            direct_steps[job] = []
            continue
        if steps_value.startswith("&"):
            anchor = steps_value[1:].strip()
            if not anchor or anchor in anchors:
                raise ValueError(f"workflow steps anchor is invalid or duplicated: {steps_value}")
            steps = workflow_step_blocks(block)
            anchors[anchor] = steps
            direct_steps[job] = steps
        elif steps_value.startswith("*"):
            alias = steps_value[1:].strip()
            if not alias:
                raise ValueError(f"workflow steps alias is invalid: {steps_value}")
            aliases[job] = alias
        else:
            direct_steps[job] = workflow_step_blocks(block)

    for job, alias in aliases.items():
        if alias not in anchors:
            raise ValueError(f"workflow job {job} uses unknown steps alias {alias}")
        direct_steps[job] = anchors[alias]

    return direct_steps


def workflow_artifact_records(
    text: str,
) -> dict[str, list[tuple[str, str, str | None]]]:
    records: dict[str, list[tuple[str, str, str | None]]] = {}
    job_blocks = workflow_job_blocks(text)
    for job, steps in workflow_job_steps(text).items():
        job_records: list[tuple[str, str, str | None]] = []
        matrix_values = workflow_job_matrix_values(job_blocks.get(job, []))
        for step in steps:
            item = LIST_ITEM_RE.match(step[0])
            if item is None:
                continue
            item_indent = len(item.group("indent"))
            action: str | None = None
            with_indent: int | None = None
            artifact_name: str | None = None
            condition: str | None = None
            continue_on_error: str | None = None
            for offset, step_line in enumerate(step):
                entry = KEY_VALUE_RE.match(step_line)
                if entry is None:
                    continue
                key = entry.group("key")
                indent = len(entry.group("indent"))
                if key == "uses" and indent in {item_indent, item_indent + 2}:
                    action = yaml_value(step, offset, entry.group("value"), indent)
                elif key == "if" and indent in {item_indent, item_indent + 2}:
                    condition = scalar(entry.group("value")).lower()
                elif key == "continue-on-error" and indent in {item_indent, item_indent + 2}:
                    continue_on_error = scalar(entry.group("value")).lower()
                elif key == "with" and indent == item_indent + 2:
                    inline = scalar(entry.group("value"))
                    with_indent = indent
                    if inline.startswith("{") and inline.endswith("}"):
                        for mapping_item in inline[1:-1].split(","):
                            inline_key, separator, inline_value = mapping_item.partition(":")
                            if separator and inline_key.strip() == "name":
                                artifact_name = scalar(inline_value.strip())
                elif with_indent is not None and indent <= with_indent:
                    with_indent = None
                elif key == "name" and with_indent is not None and indent == with_indent + 2:
                    artifact_name = scalar(entry.group("value"))
            if action is None or artifact_name is None:
                continue
            if continue_on_error not in {None, "false"}:
                continue
            if artifact_condition_is_statically_false(condition, matrix_values):
                continue
            if action.startswith("actions/upload-artifact@"):
                job_records.append(("uploads", artifact_name, condition))
            elif action.startswith("actions/download-artifact@"):
                job_records.append(("downloads", artifact_name, condition))
        records[job] = job_records
    return records


def workflow_artifact_actions(text: str) -> dict[str, dict[str, set[str]]]:
    actions: dict[str, dict[str, set[str]]] = {}
    disabled_conditions = {"false", "${{ false }}"}
    for job, records in workflow_artifact_records(text).items():
        uploads = {
            name
            for kind, name, condition in records
            if kind == "uploads" and condition not in disabled_conditions
        }
        downloads = {
            name
            for kind, name, condition in records
            if kind == "downloads" and condition not in disabled_conditions
        }
        actions[job] = {"uploads": uploads, "downloads": downloads}
    return actions


def ci_build_workflow_issues(text: str) -> list[str]:
    issues: list[str] = []
    needs = workflow_job_needs(text)
    unexpected_jobs = set(needs) - set(CI_BUILD_JOB_NEEDS)
    if unexpected_jobs:
        issues.append("CI build workflow contains unmodeled jobs outside the reviewed gate")
    for job, expected in CI_BUILD_JOB_NEEDS.items():
        if job not in needs:
            issues.append(f"CI build workflow must define job {job}")
            continue
        if needs[job] != expected:
            issues.append(
                f"CI build job {job} must need exactly {sorted(expected)}, got {sorted(needs[job])}"
            )

    if "PrefetchDeps" in needs:
        issues.append("CI build workflow must split the monolithic PrefetchDeps job")

    try:
        for job in needs:
            workflow_job_ancestors(needs, job)
    except ValueError as error:
        issues.append(str(error))
        return issues

    try:
        artifact_records = workflow_artifact_records(text)
        artifacts = workflow_artifact_actions(text)
    except ValueError as error:
        issues.append(str(error))
        return issues
    for artifact, producer in CI_BUILD_ARTIFACT_PRODUCERS.items():
        owners = {
            job
            for job, job_actions in artifacts.items()
            if artifact in job_actions["uploads"]
        }
        if owners != {producer}:
            issues.append(
                f"CI artifact {artifact} must be uploaded only by {producer}, got {sorted(owners)}"
            )

    for consumer, job_actions in artifacts.items():
        for artifact in job_actions["downloads"]:
            producer = CI_BUILD_ARTIFACT_PRODUCERS.get(artifact)
            if producer is None or producer not in needs or consumer not in needs:
                continue
            if producer not in workflow_job_ancestors(needs, consumer):
                issues.append(
                    f"CI artifact {artifact} producer {producer} must be an ancestor of {consumer}"
                )

    for artifact, consumers in CI_BUILD_REQUIRED_ARTIFACT_CONSUMERS.items():
        for consumer in consumers:
            if artifact not in artifacts.get(consumer, {}).get("downloads", set()):
                issues.append(f"CI job {consumer} must download artifact {artifact}")

    expected_steps = {
        (producer, "uploads", artifact)
        for artifact, producer in CI_BUILD_ARTIFACT_PRODUCERS.items()
    }
    expected_steps.update(
        (consumer, "downloads", artifact)
        for artifact, consumers in CI_BUILD_REQUIRED_ARTIFACT_CONSUMERS.items()
        for consumer in consumers
    )
    for job, kind, artifact in expected_steps:
        conditions = [
            condition
            for record_kind, record_artifact, condition in artifact_records.get(job, [])
            if record_kind == kind and record_artifact == artifact
        ]
        expected_condition = CI_BUILD_ARTIFACT_CONDITIONS.get((job, kind, artifact))
        if conditions != [expected_condition]:
            issues.append(
                f"CI artifact step {job} {kind} {artifact} must use condition "
                f"{expected_condition!r}, got {conditions!r}"
            )

    job_blocks = workflow_job_blocks(text)
    if workflow_job_direct_value(job_blocks.get("ci-gate", []), "if") != "always()":
        issues.append("CI gate must run with if: always()")
    for job in (
        "LinuxPackages",
        "LinuxSanitizers",
        "LinuxTsan",
        "MacPackages",
        "MacArmPackages",
        "MacSanitizers",
        "WindowsPackages",
        "WindowsX86",
        "WindowsAsan",
        "ci-gate",
    ):
        continue_on_error = workflow_job_direct_value(
            job_blocks.get(job, []), "continue-on-error"
        )
        if continue_on_error not in {None, "false"}:
            issues.append(f"CI build job {job} must not continue on error")

    adb_prefetch = job_blocks.get("PrefetchAdbHelperSources", [])
    adb_prefetch_active = "\n".join(
        active
        for line in adb_prefetch
        if (active := strip_yaml_comment(line))
    )
    if (
        "KLOGG_VERSION" in adb_prefetch_active
        or "build_adb_helper_legal_assets.py" in adb_prefetch_active
    ):
        issues.append("ADB source prefetch must not generate version-bound legal assets")

    ios_prefetch_count = sum(
        "prefetch_ios_native_sources.py" in run_block
        for block in job_blocks.values()
        for run_block in workflow_run_blocks("\n".join(block))
    )
    if ios_prefetch_count != 1:
        issues.append("iOS native sources must be prefetched exactly once")

    active_text = "\n".join(
        active for line in text.splitlines() if (active := strip_yaml_comment(line))
    )
    windows_active = "\n".join(
        active
        for line in job_blocks.get("WindowsPackages", [])
        if (active := strip_yaml_comment(line))
    )
    if "github.event_name" in windows_active and any(
        marker in windows_active
        for marker in (
            "adb-helper-",
            "agent-package-win",
            "Package tarball for upload",
            "upload-artifact@",
        )
    ):
        issues.append(
            "Windows package preparation and artifact upload must run on pull requests"
        )

    release_secret_re = re.compile(
        r"secrets\.(?:CODESIGN|NOTARIZATION|APPLE_DEVELOPER)"
    )
    release_mode_expression = (
        "qualification-mode: ${{ github.event_name == 'workflow_dispatch' && "
        "github.ref == 'refs/heads/master' && inputs.qualification-mode == "
        "'release' && 'release' || 'validation' }}"
    )
    active_lines = [
        active for line in text.splitlines() if (active := strip_yaml_comment(line))
    ]
    has_release_dispatch_input = all(
        marker in active_text
        for marker in (
            "workflow_dispatch:",
            "qualification-mode:",
            "type: choice",
            "- validation",
            "- release",
        )
    )
    unsafe_release_secret = False
    for index, line in enumerate(active_lines):
        if release_secret_re.search(line) is None:
            continue
        context = "\n".join(active_lines[max(0, index - 12) : index + 1])
        mac_qualification_input = (
            "uses: ./.github/actions/agent-package-mac" in context
            and release_mode_expression in context
        )
        release_preflight_input = (
            "name: Verify release qualification inputs" in context
            and "inputs.qualification-mode == 'release'" in context
        )
        if not (mac_qualification_input or release_preflight_input):
            unsafe_release_secret = True
            break
    if release_secret_re.search(active_text) and (
        not has_release_dispatch_input or unsafe_release_secret
    ):
        issues.append(
            "required CI validation must not require signing or notarization secrets"
        )

    if "CreatePreRelease" in job_blocks or "softprops/action-gh-release@" in active_text:
        issues.append("release publication must run outside the required CI workflow")

    mobile_jobs = {
        job
        for job in needs
        if job.startswith("BuildAdb") or job.startswith("BuildIos")
    }
    package_free_with_mobile = False
    for job, block in job_blocks.items():
        block_active = "\n".join(
            active for line in block if (active := strip_yaml_comment(line))
        )
        if "package: false" not in block_active:
            continue
        ancestors = workflow_job_ancestors(needs, job) if job in needs else set()
        if ancestors & mobile_jobs:
            package_free_with_mobile = True
            break
    if package_free_with_mobile:
        issues.append("package-free CI legs must not depend on mobile artifact producers")

    if any(
        "github.run_id" in line and "ccache" in line.lower()
        for line in active_lines
    ):
        issues.append("ccache keys must be bounded and must not use github.run_id")

    if ".ccache" in active_text and "actions/cache/save@" in active_text:
        ccache_limits = [
            int(value)
            for value in re.findall(r"ccache\s+--max-size=(\d+)M", active_text)
        ]
        if not ccache_limits or any(value > 250 for value in ccache_limits):
            issues.append("ccache entries must be capped at 250M")

    return issues


def has_open_shell_quote(value: str) -> bool:
    quote: str | None = None
    escaped = False
    for character in value:
        if escaped:
            escaped = False
            continue
        if quote == "'":
            if character == "'":
                quote = None
            continue
        if character == "\\":
            escaped = True
            continue
        if quote == '"':
            if character == '"':
                quote = None
            continue
        if character in "'\"":
            quote = character
    return quote is not None


def shell_commands(text: str) -> list[str]:
    commands: list[str] = []
    for block in workflow_run_blocks(text):
        continued: list[str] = []
        for line in block.splitlines():
            fragment = line.strip()
            line_continues = fragment.endswith("\\")
            if line_continues:
                fragment = fragment[:-1].rstrip()
            continued.append(fragment)
            command = " ".join(part for part in continued if part)
            if line_continues or has_open_shell_quote(command):
                continue
            if command:
                commands.append(command)
            continued = []
        if continued:
            commands.append(" ".join(part for part in continued if part))
    return commands


def shell_tokens(command: str) -> list[str]:
    try:
        return shlex.split(command)
    except ValueError:
        return []


def xargs_bash_script_index(tokens: list[str]) -> int | None:
    if not tokens or tokens[0] != "xargs":
        return None

    seen_null = False
    seen_max_args = False
    seen_parallel = False
    index = 1
    while index < len(tokens):
        token = tokens[index]
        if token == "-0" and not seen_null:
            seen_null = True
            index += 1
        elif token == "-n" and not seen_max_args and index + 1 < len(tokens):
            if tokens[index + 1] != "1":
                return None
            seen_max_args = True
            index += 2
        elif token == "-P" and not seen_parallel and index + 1 < len(tokens):
            seen_parallel = True
            index += 2
        elif token == "bash":
            if (
                not seen_null
                or not seen_max_args
                or index + 1 >= len(tokens)
                or tokens[index + 1] != "-c"
            ):
                return None
            return index + 2
        else:
            return None
    return None


def clang_tidy_consumes_xargs_argument(script: str) -> bool:
    tokens = shell_tokens(script)
    if not tokens or tokens[0] != "clang-tidy":
        return False
    for control in (";", "&&", "||", "\n"):
        if control in tokens:
            tokens = tokens[: tokens.index(control)]
    return bool(tokens) and tokens[-1] in {"$1", "${1}"}


def cmake_cache_assignments(tokens: list[str], name: str) -> list[str]:
    assignments: list[str] = []
    for index, token in enumerate(tokens):
        candidate = None
        if token == "-D" and index + 1 < len(tokens):
            candidate = tokens[index + 1]
        elif token.startswith("-D"):
            candidate = token[2:]
        if candidate is None:
            continue
        match = re.fullmatch(rf"{re.escape(name)}(?::[^=]+)?=(.*)", candidate)
        if match is not None:
            assignments.append(match.group(1).upper())
    return assignments


def cmake_unsets_cache_entry(tokens: list[str], name: str) -> bool:
    for index, token in enumerate(tokens):
        pattern = None
        if token == "-U" and index + 1 < len(tokens):
            pattern = tokens[index + 1]
        elif token.startswith("-U"):
            pattern = token[2:]
        if pattern and fnmatch.fnmatchcase(name, pattern):
            return True
    return False


def option_value(tokens: list[str], option: str) -> str | None:
    try:
        index = tokens.index(option)
    except ValueError:
        return None
    return tokens[index + 1] if index + 1 < len(tokens) else None


def unique_option_value(tokens: list[str], option: str) -> str | None:
    if tokens.count(option) != 1:
        return None
    return option_value(tokens, option)


def unique_cmake_option_value(tokens: list[str], option: str) -> str | None:
    values: list[str] = []
    for index, token in enumerate(tokens):
        if token == option and index + 1 < len(tokens):
            values.append(tokens[index + 1])
        elif token.startswith(option) and token != option:
            value = token[len(option) :]
            values.append(value[1:] if value.startswith("=") else value)
    return values[0] if len(values) == 1 else None


def yaml_scalar_body_lines(lines: list[str]) -> set[int]:
    body: set[int] = set()
    for index, line in enumerate(lines):
        entry = KEY_VALUE_RE.match(line)
        if entry is None or scalar(entry.group("value")) not in {"|", "|-", "|+", ">", ">-", ">+"}:
            continue
        indent = len(entry.group("indent"))
        for following in range(index + 1, len(lines)):
            candidate = lines[following]
            if not candidate.strip():
                body.add(following)
                continue
            candidate_indent = len(candidate) - len(candidate.lstrip())
            if candidate_indent <= indent:
                break
            body.add(following)
    return body


def workflow_action_refs(text: str) -> list[tuple[int, str]]:
    lines = text.splitlines()
    scalar_body = yaml_scalar_body_lines(lines)
    actions: list[tuple[int, str]] = []
    for index, line in enumerate(lines):
        if index in scalar_body:
            continue
        entry = KEY_VALUE_RE.match(line)
        if entry is None or entry.group("key") != "uses":
            continue
        value = yaml_value(lines, index, entry.group("value"), len(entry.group("indent")))
        value = value.strip()
        if value:
            actions.append((index + 1, value))
    return actions


def checkout_steps(text: str) -> list[tuple[int, str, str | None]]:
    lines = text.splitlines()
    checkouts: list[tuple[int, str, str | None]] = []
    for index, line in enumerate(lines):
        item = LIST_ITEM_RE.match(line)
        if item is None:
            continue
        item_indent = len(item.group("indent"))
        end = len(lines)
        for following in range(index + 1, len(lines)):
            next_item = LIST_ITEM_RE.match(lines[following])
            if next_item is not None and len(next_item.group("indent")) <= item_indent:
                end = following
                break

        block = lines[index:end]
        entries: list[tuple[int, str, str, int]] = []
        for offset, block_line in enumerate(block):
            entry = KEY_VALUE_RE.match(block_line)
            if entry is not None:
                entries.append(
                    (
                        offset,
                        entry.group("key"),
                        entry.group("value"),
                        len(entry.group("indent")),
                    )
                )

        uses: str | None = None
        uses_line = index + 1
        for offset, key, value, entry_indent in entries:
            if key == "uses":
                candidate = yaml_value(block, offset, value, entry_indent)
                if candidate.startswith("actions/checkout@"):
                    uses = candidate
                    uses_line = index + offset + 1
                    break
        if uses is None:
            continue

        credentials: str | None = None
        with_entries = [entry for entry in entries if entry[1] == "with"]
        if with_entries:
            with_offset, _, _, with_indent = with_entries[0]
            mapping_entries: list[tuple[str, str, int]] = []
            for offset in range(with_offset + 1, len(block)):
                block_line = block[offset]
                stripped = block_line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                indent = len(block_line) - len(block_line.lstrip())
                if indent <= with_indent:
                    break
                entry = KEY_VALUE_RE.match(block_line)
                if entry is not None:
                    mapping_entries.append(
                        (entry.group("key"), entry.group("value"), indent)
                    )
            if mapping_entries:
                child_indent = min(entry[2] for entry in mapping_entries)
                for key, value, indent in mapping_entries:
                    if key == "persist-credentials" and indent == child_indent:
                        credentials = scalar(value).lower()
                        break

        checkouts.append((uses_line, uses, credentials))
    return checkouts


def check_checkout_blocks(path: Path, text: str) -> list[str]:
    issues: list[str] = []
    for line, uses, credentials in checkout_steps(text):
        if CHECKOUT_SHA_RE.fullmatch(uses) is None:
            issues.append(f"{path}:{line}: actions/checkout must use a reviewed 40-char SHA")
        if credentials != "false":
            issues.append(
                f"{path}:{line}: checkout must set with.persist-credentials to false"
            )

    for line, uses in workflow_action_refs(text):
        if uses.startswith("./") or uses.startswith("docker://"):
            continue
        if uses in KNOWN_NODE20_ACTION_REFS:
            issues.append(f"{path}:{line}: remote action uses a known Node20 action generation")
            continue
        pinned = REMOTE_ACTION_SHA_RE.fullmatch(uses)
        if uses.startswith("actions/checkout@") and pinned is None:
            continue
        if pinned is None:
            issues.append(
                f"{path}:{line}: remote actions must use a reviewed 40-char SHA"
            )
            continue
        action, sha = uses.rsplit("@", 1)
        repository = "/".join(action.split("/")[:2]).lower()
        expected = REVIEWED_ACTION_REVISIONS.get(repository)
        if expected is None or sha != expected:
            issues.append(
                f"{path}:{line}: remote action SHA is not the reviewed revision"
            )
    return issues


def codeql_workflow_issues(text: str) -> list[str]:
    """Validate the fail-closed CodeQL job and its remote action revisions."""
    lines = text.splitlines()
    job_start: int | None = None
    job_indent = 0
    for index, line in enumerate(lines):
        match = re.match(r"^(?P<indent>\s*)analyze:\s*(?:#.*)?$", line)
        if match is not None:
            job_start = index
            job_indent = len(match.group("indent"))
            break

    if job_start is None:
        return ["CodeQL workflow must define the analyze job"]

    job_end = len(lines)
    for index in range(job_start + 1, len(lines)):
        line = lines[index]
        active = strip_yaml_comment(line)
        if not active:
            continue
        indent = len(line) - len(line.lstrip())
        if indent <= job_indent:
            job_end = index
            break

    job_lines = lines[job_start + 1 : job_end]
    direct_entries: list[tuple[str, str]] = []
    nested_entries: list[tuple[str, str]] = []
    child_indents = [
        len(line) - len(line.lstrip())
        for line in job_lines
        if strip_yaml_comment(line)
        and (len(line) - len(line.lstrip())) > job_indent
        and KEY_VALUE_RE.match(line) is not None
    ]
    direct_indent = min(child_indents) if child_indents else job_indent + 2

    for line in job_lines:
        entry = KEY_VALUE_RE.match(line)
        if entry is None:
            continue
        key = entry.group("key")
        value = scalar(entry.group("value"))
        indent = len(entry.group("indent"))
        nested_entries.append((key, value))
        if indent == direct_indent:
            direct_entries.append((key, value))

    issues: list[str] = []
    timeout_values = [value for key, value in direct_entries if key == "timeout-minutes"]
    if timeout_values != ["30"]:
        issues.append("CodeQL analyze job must set timeout-minutes: 30")

    if any(key == "continue-on-error" for key, _ in nested_entries):
        issues.append("CodeQL workflow must not use continue-on-error")

    jobs_position = next(
        (index for index, line in enumerate(lines) if re.match(r"^jobs:\s*(?:#.*)?$", line)),
        len(lines),
    )
    trigger_text = "\n".join(
        f"{' ' * (len(line) - len(line.lstrip()))}{active}"
        for line in lines[:jobs_position]
        if (active := strip_yaml_comment(line))
    )
    if not re.search(r"(?m)^\s{2}push:\s*$", trigger_text) or not re.search(
        r"(?m)^\s{4}branches:\s*\[\s*master\s*\]\s*$", trigger_text
    ):
        issues.append("CodeQL workflow must run on pushes to master")

    action_refs: dict[str, list[str]] = {"init": [], "analyze": []}
    for key, value in nested_entries:
        if key != "uses" or not value.startswith("github/codeql-action/"):
            continue
        action = value.partition("github/codeql-action/")[2].partition("@")[0]
        if action in action_refs:
            action_refs[action].append(value)

    pinned_shas: dict[str, str] = {}
    for action in ("init", "analyze"):
        refs = action_refs[action]
        if len(refs) != 1:
            issues.append(f"CodeQL workflow must use exactly one {action} action")
            continue
        match = CODEQL_ACTION_SHA_RE.fullmatch(refs[0])
        if match is None:
            issues.append(f"CodeQL {action} must use a reviewed 40-char SHA")
            continue
        pinned_shas[action] = match.group("sha")

    matching_codeql_revision = (
        len(pinned_shas) == 2 and pinned_shas["init"] == pinned_shas["analyze"]
    )
    if len(pinned_shas) == 2 and not matching_codeql_revision:
        issues.append("CodeQL init and analyze must use the same reviewed SHA")
    if (
        matching_codeql_revision
        and pinned_shas["init"] != CODEQL_CURRENT_SHA
    ):
        issues.append("CodeQL actions must use the current reviewed generation")

    active_text = "\n".join(
        active for line in text.splitlines() if (active := strip_yaml_comment(line))
    )
    init_position = active_text.find("github/codeql-action/init@")
    analyze_position = active_text.find("github/codeql-action/analyze@")
    build_mode_position = active_text.find("build-mode: manual")
    if not (
        init_position >= 0
        and build_mode_position > init_position
        and analyze_position > build_mode_position
    ):
        issues.append("CodeQL init must set build-mode: manual")

    cmake_configures = []
    for command in shell_commands(text):
        tokens = shell_tokens(command)
        if (
            tokens
            and tokens[0] == "cmake"
            and "--build" not in tokens
            and "-P" not in tokens
        ):
            cmake_configures.append(tokens)
    cpm_source_caches = [
        value
        for tokens in cmake_configures
        for value in cmake_cache_assignments(tokens, "CPM_SOURCE_CACHE")
    ]
    fully_disconnected = [
        value
        for tokens in cmake_configures
        for value in cmake_cache_assignments(
            tokens, "FETCHCONTENT_FULLY_DISCONNECTED"
        )
    ]
    if (
        cpm_source_caches != ["$GITHUB_WORKSPACE/CPM_CACHE"]
        or fully_disconnected != ["ON"]
    ):
        issues.append(
            "CodeQL configure must use the restored CPM source cache fully disconnected"
        )

    return issues


def agent_setup_cpm_issues(text: str) -> list[str]:
    """Keep cache-only workflows independent of cross-workflow warm-up races."""
    restore_marker = "name: Restore CPM cache"
    prefetch_marker = "uses: ./.github/actions/prefetch-cpm-cache"
    restore_position = text.find(restore_marker)
    prefetch_position = text.find(prefetch_marker)
    required_condition = "if: ${{ env.KLOGG_REQUIRE_PREFETCHED_CPM != 'ON' }}"

    if (
        restore_position < 0
        or prefetch_position < 0
        or restore_position >= prefetch_position
        or text.count(prefetch_marker) != 1
    ):
        return ["agent setup must populate the CPM source cache after restore"]

    prefetch_block_start = text.rfind("\n    - ", restore_position, prefetch_position)
    if prefetch_block_start < 0:
        prefetch_block_start = restore_position
    prefetch_block_end = text.find("\n    - ", prefetch_position)
    if prefetch_block_end < 0:
        prefetch_block_end = len(text)
    prefetch_block = text[prefetch_block_start:prefetch_block_end]
    if (
        required_condition not in prefetch_block
        or "KLOGG_WORKSPACE: ${{ github.workspace }}" not in prefetch_block
    ):
        return ["agent setup must populate the CPM source cache after restore"]

    return []


def has_unsupported_macos_lsan(text: str) -> bool:
    return re.search(r"ASAN_OPTIONS=.*detect_leaks=1", text) is not None


def windows_package_action_issues(text: str) -> list[str]:
    active = "\n".join(
        line for line in text.splitlines() if strip_yaml_comment(line)
    )
    if "github.event_name" in active:
        return ["Windows package composite must remain event-neutral"]
    return []


def static_analysis_workflow_issues(text: str) -> list[str]:
    commands = shell_commands(text)
    issues: list[str] = []

    expected_producer = [
        "python3",
        "scripts/first_party_compile_units.py",
        "$KLOGG_BUILD_ROOT/compile_commands.json",
        "$KLOGG_WORKSPACE/src",
        "--null",
        ">",
        "tidy_files.nul",
    ]
    tidy_scope_commands = [
        command for command in commands if shell_tokens(command) == expected_producer
    ]
    if len(tidy_scope_commands) != 1 or any(
        command.startswith("find src ") and "tidy_files.txt" in command
        for command in commands
    ):
        issues.append(
            "clang-tidy TU scope must come from the configured compile database"
        )

    strict_consumers = []
    strict_runners = []
    report_only_consumers = []
    for command in commands:
        tokens = shell_tokens(command)
        if (
            len(tokens) >= 2
            and tokens[:2]
            == ["python3", "scripts/run_changed_clang_tidy.py"]
            and unique_option_value(tokens, "--base")
            in {"${{ github.event.pull_request.base.sha }}", "$KLOGG_ANALYSIS_BASE"}
            and unique_option_value(tokens, "--build-dir") == "$KLOGG_BUILD_ROOT"
            and unique_option_value(tokens, "--clang-tidy-diff")
            == "$CLANG_TIDY_DIFF"
            and not any(control in tokens for control in ("&&", "||", ";"))
        ):
            strict_runners.append(command)

        script_index = xargs_bash_script_index(tokens)
        if (
            script_index is None
            or option_value(tokens, "<") != "tidy_files.nul"
            or tokens.count("<") != 1
            or tokens.count("-c") != 1
        ):
            continue
        if (
            script_index + 2 >= len(tokens)
            or tokens[script_index + 1] != "_"
            or tokens[script_index + 2] != "<"
        ):
            continue
        script = tokens[script_index].strip()
        if not script.startswith("clang-tidy ") or not clang_tidy_consumes_xargs_argument(
            script
        ):
            continue
        if (
            '--line-filter="$TIDY_LINE_FILTER"' in script
            and not any(control in script for control in (";", "&&", "||", "\n"))
            and not any(control in tokens for control in ("&&", "||", ";"))
        ):
            strict_consumers.append(command)
        if (
            "--line-filter" not in script
            and "--warnings-as-errors" not in script
            and not any(control in script for control in (";", "&&", "||", "\n"))
            and not any(control in tokens for control in ("&&", "||", ";"))
        ):
            report_only_consumers.append(command)

    if (
        len(strict_consumers) + len(strict_runners) != 1
        or len(report_only_consumers) != 1
        or any("tidy_files.txt" in command for command in commands)
    ):
        issues.append(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers"
        )

    cmake_commands = []
    for command in commands:
        tokens = shell_tokens(command)
        if tokens and tokens[0] == "cmake" and "--build" not in tokens:
            cmake_commands.append(tokens)
    sentry_configures = [
        tokens
        for tokens in cmake_commands
        if unique_cmake_option_value(tokens, "-S") == "$KLOGG_WORKSPACE"
        and unique_cmake_option_value(tokens, "-B") == "$KLOGG_BUILD_ROOT"
        and "-P" not in tokens
    ]
    sentry_assignments = [
        value
        for tokens in sentry_configures
        for value in cmake_cache_assignments(tokens, "KLOGG_USE_SENTRY")
    ]
    later_sentry_commands = [
        tokens
        for tokens in cmake_commands
        if tokens not in sentry_configures
        and unique_cmake_option_value(tokens, "-B") == "$KLOGG_BUILD_ROOT"
        and "-P" not in tokens
    ]
    sentry_unset = any(
        cmake_unsets_cache_entry(tokens, "KLOGG_USE_SENTRY")
        for tokens in sentry_configures
    )
    cmake_true_values = {"1", "ON", "TRUE", "YES", "Y"}
    if (
        len(sentry_configures) != 1
        or len(sentry_assignments) != 1
        or sentry_assignments[-1] not in cmake_true_values
        or later_sentry_commands
        or sentry_unset
    ):
        issues.append("static analysis must configure optional Sentry production code")

    vectorscan_assignments = [
        value
        for tokens in sentry_configures
        for value in cmake_cache_assignments(tokens, "KLOGG_USE_VECTORSCAN")
    ]
    vectorscan_unset = any(
        cmake_unsets_cache_entry(tokens, "KLOGG_USE_VECTORSCAN")
        for tokens in sentry_configures
    )
    if (
        len(vectorscan_assignments) != 1
        or vectorscan_assignments[-1] not in cmake_true_values
        or later_sentry_commands
        or vectorscan_unset
    ):
        issues.append(
            "static analysis must configure optional Vectorscan production code"
        )

    cpm_source_caches = [
        value
        for tokens in sentry_configures
        for value in cmake_cache_assignments(tokens, "CPM_SOURCE_CACHE")
    ]
    fully_disconnected = [
        value
        for tokens in sentry_configures
        for value in cmake_cache_assignments(
            tokens, "FETCHCONTENT_FULLY_DISCONNECTED"
        )
    ]
    if (
        cpm_source_caches != ["$KLOGG_WORKSPACE/CPM_CACHE"]
        or fully_disconnected != ["ON"]
    ):
        issues.append(
            "static analysis configure must use the prefetched CPM cache fully disconnected"
        )

    full_audit_isolated = all(
        marker in text
        for marker in (
            "group: ${{ github.workflow }}-${{ github.event_name }}-${{ github.event_name == 'workflow_dispatch' && github.run_id || github.ref }}",
            "cancel-in-progress: ${{ github.event_name != 'schedule' }}",
            'if [ "$KLOGG_ANALYSIS_MODE" = "full-report" ]; then',
            'pull_request) base_sha="$PR_BASE_SHA"',
            'push) base_sha="$PUSH_BASE_SHA"',
            'MANUAL_BASE_SHA: ${{ inputs.base-sha }}',
            'git fetch --no-tags origin "$base_sha"',
        )
    )
    if (
        report_only_consumers
        and "github.event_name" in text
        and "pull_request" in text
        and not full_audit_isolated
    ):
        issues.append("clang-tidy findings must fail both pull requests and master pushes")

    # A PR that deletes a src/ file must not feed the deleted path to
    # clang-tidy/cppcheck: both tools fail on the nonexistent file and the
    # gate breaks for an unrelated reason. Every changed-path discovery
    # (`git diff --name-only`) must therefore exclude deleted files.
    for command in commands:
        tokens = shell_tokens(command)
        if (
            "diff" in tokens
            and "--name-only" in tokens
            and "--diff-filter=d" not in tokens
        ):
            issues.append(
                "changed-path discovery must exclude deleted files (--diff-filter=d)"
            )
            break

    return issues


def coverage_build_targets(command: str) -> set[str]:
    tokens = shell_tokens(command)
    if not tokens or tokens[:2] != ["cmake", "--build"]:
        return set()
    for control in ("&&", "||", ";", "|"):
        if control in tokens:
            tokens = tokens[: tokens.index(control)]
    target_index = None
    for option in ("-t", "--target"):
        if option in tokens:
            target_index = tokens.index(option) + 1
            break
    if target_index is None:
        return set()
    targets = set()
    for token in tokens[target_index:]:
        if token.startswith("-"):
            break
        targets.add(token)
    return targets


def coverage_workflow_issues(text: str) -> list[str]:
    issues: list[str] = []
    commands = shell_commands(text)
    gcovr_commands = [command for command in commands if command.startswith("gcovr ")]
    gcovr_tokens = [shell_tokens(command) for command in gcovr_commands]
    if len(gcovr_commands) != 2 or any(
        unique_option_value(tokens, "--filter") != "^src/"
        for tokens in gcovr_tokens
    ):
        issues.append("gcovr must include root-relative src/ paths in every report pass")

    if (
        "EVENT_NAME: ${{ github.event_name }}" not in text
        or 'git fetch --no-tags origin "$base_sha"' not in text
        or 'git cat-file -e "$base_sha^{commit}"' not in text
        or "coverage ratchet base commit" not in text
        or "base_sha=$(git rev-parse HEAD^ 2>/dev/null || true)" in text.replace(
            "workflow_dispatch) base_sha=$(git rev-parse HEAD^ 2>/dev/null || true)",
            "",
        )
    ):
        issues.append("coverage ratchet must fail closed on the event base commit")

    if len(gcovr_commands) == 2:
        html_reports = []
        json_reports = []
        for command in gcovr_commands:
            tokens = shell_tokens(command)
            output = unique_option_value(tokens, "-o")
            if (
                "--html-details" in tokens
                and "--print-summary" in tokens
                and output == "coverage_report/index.html"
            ):
                html_reports.append(command)
            if "--json" in tokens and output == "coverage_report/coverage.json":
                json_reports.append(command)
        if len(html_reports) != 1 or len(json_reports) != 1:
            issues.append(
                "coverage must produce the authoritative HTML summary and JSON report"
            )

    build_targets = set()
    for command in commands:
        build_targets.update(coverage_build_targets(command))
    for target in ("klogg", "klogg_grep", "klogg_test_build"):
        if target not in build_targets:
            issues.append(f"coverage build must include target {target}")

    negative_hits_option = "--gcov-ignore-parse-errors"
    negative_hits_value = "negative_hits.warn_once_per_file"
    if len(gcovr_commands) != 2 or any(
        tokens.count(negative_hits_option) != 1
        or option_value(tokens, negative_hits_option) != negative_hits_value
        for tokens in gcovr_tokens
    ):
        issues.append(
            "every coverage report must narrowly handle GCC's negative branch-hit bug"
        )
    if any(
        negative_hits_option in tokens
        and (
            tokens.count(negative_hits_option) != 1
            or option_value(tokens, negative_hits_option) != negative_hits_value
        )
        for tokens in gcovr_tokens
    ):
        issues.append(
            "coverage may ignore only negative_hits.warn_once_per_file parse errors"
        )
    if any(
        any(control in tokens for control in ("&&", "||", ";"))
        for tokens in gcovr_tokens
    ):
        issues.append("coverage report generation must fail closed")
    return issues


def coverage_floor(path: Path) -> float:
    value = float(path.read_text().strip())
    if not math.isfinite(value) or value <= 0 or value > 100:
        raise ValueError
    return value


def cppcheck_suppression_issues(text: str) -> list[str]:
    issues: list[str] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        entry = line.strip()
        if not entry:
            continue
        parts = entry.rsplit(":", 2)
        if len(parts) != 3 or not parts[1] or not parts[2].isdigit():
            issues.append(
                f"tests/cppcheck_suppressions.txt:{line_number}: "
                "cppcheck baselines must be qualified by path and line"
            )
    return issues


def ci_manifests(root: Path) -> list[Path]:
    paths = set()
    for pattern in ("*.yml", "*.yaml"):
        paths.update((root / ".github" / "workflows").glob(pattern))
    for pattern in ("action.yml", "action.yaml"):
        paths.update((root / ".github" / "actions").glob(f"**/{pattern}"))
    return sorted(paths)


def check_repo(root: Path) -> list[str]:
    issues: list[str] = []
    workflows = root / ".github" / "workflows"
    for path in ci_manifests(root):
        issues.extend(check_checkout_blocks(path.relative_to(root), path.read_text()))

    for path in sorted((root / "tests" / "sanitizers").glob("*_suppressions.txt")):
        text = path.read_text()
        for match in BROAD_SUPPRESSION_RE.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            issues.append(
                f"{path.relative_to(root)}:{line}: broad sanitizer suppression can hide first-party defects"
            )

    coverage_text = (workflows / "coverage.yml").read_text()
    issues.extend(
        f".github/workflows/coverage.yml: {issue}"
        for issue in coverage_workflow_issues(coverage_text)
    )
    if "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a" not in coverage_text:
        issues.append(
            ".github/workflows/coverage.yml: coverage artifact upload must use a reviewed commit SHA"
        )

    codeql_text = (workflows / "codeql-analysis.yml").read_text()
    issues.extend(
        f".github/workflows/codeql-analysis.yml: {issue}"
        for issue in codeql_workflow_issues(codeql_text)
    )

    agent_setup_path = (
        root / ".github" / "actions" / "agent-setup" / "action.yml"
    )
    issues.extend(
        f".github/actions/agent-setup/action.yml: {issue}"
        for issue in agent_setup_cpm_issues(agent_setup_path.read_text())
    )
    windows_package_path = (
        root / ".github" / "actions" / "agent-package-win" / "action.yml"
    )
    issues.extend(
        f".github/actions/agent-package-win/action.yml: {issue}"
        for issue in windows_package_action_issues(windows_package_path.read_text())
    )

    static_text = (workflows / "static-analysis.yml").read_text()
    issues.extend(
        f".github/workflows/static-analysis.yml: {issue}"
        for issue in static_analysis_workflow_issues(static_text)
    )
    if (
        'python3 "$CLANG_TIDY_DIFF"' not in static_text
        and '--clang-tidy-diff "$CLANG_TIDY_DIFF"' not in static_text
    ):
        issues.append(
            ".github/workflows/static-analysis.yml: clang-tidy-diff must use the discovered Ubuntu tool path"
        )
    changed_tidy_runner = (root / "scripts" / "run_changed_clang_tidy.py").read_text()
    for extension in (".cxx", ".hh", ".hxx"):
        if extension not in changed_tidy_runner:
            issues.append(
                f"scripts/run_changed_clang_tidy.py: static analysis must include {extension} files"
            )
    if not re.search(
        r"git(?: -c core\.quotePath=false)? diff --name-only -z", static_text
    ):
        issues.append(
            ".github/workflows/static-analysis.yml: changed paths must use NUL-delimited Git output"
        )
    header_filter = (root / ".clang-tidy").read_text()
    if ".*/klogg/src/.*\\.(h|hh|hpp|hxx)$" not in header_filter:
        issues.append(
            ".clang-tidy: header filter must admit headers in new first-party modules"
        )
    for diagnostic_id in BROAD_CPPCHECK_IDS:
        if re.search(rf"--suppress={re.escape(diagnostic_id)}(?:\s|\\|$)", static_text):
            issues.append(
                f".github/workflows/static-analysis.yml: cppcheck suppression {diagnostic_id} must be location-qualified"
            )
    issues.extend(
        cppcheck_suppression_issues(
            (root / "tests" / "cppcheck_suppressions.txt").read_text()
        )
    )

    ci_text = (workflows / "ci-build.yml").read_text()
    issues.extend(
        f".github/workflows/ci-build.yml: {issue}"
        for issue in ci_build_workflow_issues(ci_text)
    )
    if not all(
        marker in ci_text
        for marker in (
            "github.event_name == 'workflow_dispatch' && github.run_id || github.ref",
            "inputs.qualification-mode == 'validation'",
        )
    ):
        issues.append(
            ".github/workflows/ci-build.yml: release qualification must use an isolated non-cancelable concurrency group"
        )
    if "detect_container_overflow=0" in ci_text:
        issues.append(
            ".github/workflows/ci-build.yml: container-overflow detection must not be disabled globally"
        )
    mac_section = ci_text.partition("  MacPackages:")[2].partition("  WindowsPackages:")[0]
    if has_unsupported_macos_lsan(mac_section):
        issues.append(
            ".github/workflows/ci-build.yml: Apple ASan does not support detect_leaks=1"
        )

    for name in ("coverage-line.txt", "coverage-branch.txt"):
        path = root / ".github" / "baselines" / name
        try:
            coverage_floor(path)
        except (OSError, ValueError):
            issues.append(
                f"{path.relative_to(root)}: coverage floor must be a finite positive percentage no greater than 100"
            )

    return issues


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    issues = check_repo(root)
    if issues:
        print("\n".join(issues))
        return 1
    print("OK: CI quality gates are not silently weakened.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
