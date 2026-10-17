from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).parents[2]
ROOT_CMAKE = ROOT / "CMakeLists.txt"
LOCK = ROOT / "3rdparty" / "libimobiledevice" / "libimobiledevice.lock.json"
PATCH = (
    ROOT
    / "3rdparty"
    / "libimobiledevice"
    / "patches"
    / "0001-fix-ostrace-live-packet-leak.patch"
)
THIRD_PARTY_CMAKE = ROOT / "3rdparty" / "CMakeLists.txt"
PREFETCH_CMAKE = ROOT / "cmake" / "prefetch_cpm" / "CMakeLists.txt"
MAC_PACKAGE_ACTION = ROOT / ".github" / "actions" / "agent-package-mac" / "action.yml"
APP_CMAKE = ROOT / "src" / "app" / "CMakeLists.txt"
IOS_LIVE_SERVICES = ROOT / "src" / "ui" / "src" / "iosliveservices.cpp"
IOS_NATIVE_STREAM = ROOT / "src" / "livecapture" / "src" / "iosnativestream.cpp"
IOS_NATIVE_HEADER = ROOT / "src" / "livecapture" / "include" / "iosnativestream.h"
CI_BUILD_WORKFLOW = ROOT / ".github" / "workflows" / "ci-build.yml"
BUILD_SCRIPT = ROOT / "scripts" / "build_ios_native_stack.py"
VERIFY_SCRIPT = ROOT / "scripts" / "verify_ios_native_stack.py"
LEGAL_SCRIPT = ROOT / "scripts" / "build_ios_native_legal_assets.py"
SUPERBUILD = ROOT / "packaging" / "ios-native" / "superbuild" / "CMakeLists.txt"

_BUILD_SPEC = importlib.util.spec_from_file_location("build_ios_native_stack", BUILD_SCRIPT)
assert _BUILD_SPEC is not None and _BUILD_SPEC.loader is not None
BUILD_MODULE = importlib.util.module_from_spec(_BUILD_SPEC)
_BUILD_SPEC.loader.exec_module(BUILD_MODULE)
sys.path.insert(0, str(ROOT / "scripts"))
_LEGAL_SPEC = importlib.util.spec_from_file_location("build_ios_native_legal_assets", LEGAL_SCRIPT)
assert _LEGAL_SPEC is not None and _LEGAL_SPEC.loader is not None
LEGAL_MODULE = importlib.util.module_from_spec(_LEGAL_SPEC)
_LEGAL_SPEC.loader.exec_module(LEGAL_MODULE)
_VERIFY_SPEC = importlib.util.spec_from_file_location("verify_ios_native_stack", VERIFY_SCRIPT)
assert _VERIFY_SPEC is not None and _VERIFY_SPEC.loader is not None
VERIFY_MODULE = importlib.util.module_from_spec(_VERIFY_SPEC)
_VERIFY_SPEC.loader.exec_module(VERIFY_MODULE)
_CI_SPEC = importlib.util.spec_from_file_location(
    "ios_native_ci_quality", ROOT / "scripts" / "lint_ci_quality.py"
)
assert _CI_SPEC is not None and _CI_SPEC.loader is not None
CI_MODULE = importlib.util.module_from_spec(_CI_SPEC)
_CI_SPEC.loader.exec_module(CI_MODULE)

IOS_STARTUP_STEP = "Execute pinned os_trace startup C contract"
IOS_ARCHIVE_ROOT = "${{ github.workspace }}/ios-native-sources"
IOS_STARTUP_COMMAND = (
    'python3 "${{ github.workspace }}/tests/scripts/test_libimobiledevice_ostrace_startup.py"'
)
IOS_NATIVE_JOBS = (
    "BuildIosNativeX64",
    "BuildIosNativeArm64",
)

# Native jobs run on ordinary events and isolated dependency dispatches, but
# never become environment producer work or replace the ordinary application gate.
NATIVE_JOB_GUARD = (
    "${{ ((github.event_name != 'workflow_dispatch' || "
    "(inputs.environment-mode == 'off' && inputs.dependency-mode == 'off')) || "
    "(github.event_name == 'workflow_dispatch' && "
    "(inputs.dependency-mode == 'qualify' || inputs.dependency-mode == 'publish'))) "
    "&& !contains(github.event.head_commit.message, '[skip ci]') }}"
)


def ios_startup_ci_issues(text: str) -> list[str]:
    """Require the small, unconditional C gate in both native source consumers.

    Deliberately accept only the existing job condition and shared-step shape:
    unknown conditions must not silently narrow event coverage. Concrete-leg
    identity is enforced by the workflow-shape contract, not duplicated here.
    Shell bodies are checked as complete commands, not script-name substrings.
    """
    triggers = CI_MODULE.workflow_trigger_mapping(text)
    jobs = CI_MODULE.workflow_mapping_block(text.splitlines(), "jobs", 0)
    if triggers is None or jobs is None:
        return ["malformed workflow triggers/jobs"]
    if not {"pull_request", "push", "workflow_dispatch"} <= triggers.keys():
        return ["missing native CI event coverage"]
    issues = []
    for name in IOS_NATIVE_JOBS:
        job = jobs.get(name, (None, []))[1]
        direct = CI_MODULE.workflow_mapping_block(job, name, 2)
        if direct is None:
            issues.append(f"{name}: missing or malformed native job")
            continue
        if (
            direct.get("if", (None,))[0] != NATIVE_JOB_GUARD
            or "continue-on-error" in direct
        ):
            issues.append(f"{name}: native event gate changed")
        steps_value = direct.get("steps", (None,))[0]
        if name == "BuildIosNativeArm64":
            if steps_value != "*ios_native_steps":
                issues.append(f"{name}: must inherit the checked native steps")
                continue
            job = jobs.get("BuildIosNativeX64", (None, []))[1]
        elif steps_value != "&ios_native_steps":
            issues.append(f"{name}: missing native steps anchor")
        steps = CI_MODULE.workflow_step_blocks(job)
        parsed = [CI_MODULE.workflow_step_fields(step) for step in steps]
        matches = [i for i, (fields, _) in enumerate(parsed)
                   if fields.get("name") == IOS_STARTUP_STEP]
        if len(matches) != 1:
            issues.append(f"{name}: required actual-C startup step missing or duplicated")
            continue
        index = matches[0]
        fields, children = parsed[index]
        # Normalize the list item's first key to a mapping to reject duplicate
        # direct keys and malformed child mappings using the existing parser.
        normalized = ["      gate:", steps[index][0].replace("- ", "  ", 1)]
        normalized.extend(steps[index][1:])
        direct_step = CI_MODULE.workflow_mapping_block(normalized, "gate", 6)
        environment = CI_MODULE.workflow_mapping_block(steps[index], "env", 8)
        commands = [line.strip() for line in fields.get("run", "").splitlines()
                    if line.strip() and not line.lstrip().startswith("#")]
        if (
            direct_step is None
            or set(fields) != {"name", "shell", "env", "run"}
            or fields.get("shell") != "bash"
            or environment is None
            or children.get("env") != {"KLOGG_IOS_NATIVE_ARCHIVE_ROOT": IOS_ARCHIVE_ROOT}
            or commands != ["set -euo pipefail", IOS_STARTUP_COMMAND]
        ):
            issues.append(f"{name}: startup must execute fail-closed with explicit archives")
        previous, inputs = parsed[index - 1] if index else ({}, {})
        if (
            not previous.get("uses", "").startswith("actions/download-artifact@")
            or "if" in previous or "continue-on-error" in previous
            or inputs.get("with", {}).get("name") != "ios-native-source-cache"
            or inputs.get("with", {}).get("path") != IOS_ARCHIVE_ROOT
        ):
            issues.append(f"{name}: startup must immediately follow native archive download")
    return issues

EXPECTED_SOURCES = {
    "openssl": {
        "version": "3.5.7",
        "commit": "8cf17aaeb4599f8af87fefd810b5b5fee90fe69e",
        "archive_sha256": "a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8",
        "archive_url": "https://www.openssl.org/source/openssl-3.5.7.tar.gz",
    },
    "curl": {
        "version": "8.21.0",
        "commit": "68720b4837284335b2d63cb358f8f6ce65f5bc55",
        "archive_sha256": "aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6",
        "archive_url": "https://curl.se/download/curl-8.21.0.tar.xz",
    },
    "libplist": {
        "version": "2.7.0",
        "commit": "cf5897a71ea412ea2aeb1e2f6b5ea74d4fabfd8c",
    },
    "libtatsu": {
        "version": "1.0.5",
        "commit": "42329cb756682535c7c0f087987b78d1dd5b16c8",
        "archive_sha256": "83f25dec2d4a3982d6d0b9cb0d9a7c891bc00f3c232b29fb518ad43b16c14bb3",
    },
    "libimobiledevice-glue": {
        "version": "1.3.2",
        "commit": "aef2bf0f5bfe961ad83d224166462d87b1df2b00",
    },
    "libusbmuxd": {
        "version": "2.1.1",
        "commit": "adf9c22b9010490e4b55eaeb14731991db1c172c",
    },
    "libimobiledevice": {
        "version": "1.4.0",
        "commit": "149f7623c672c1fa73122c7119a12bfc0012f2ac",
    },
}
EXPECTED_BUILD_ORDER = list(EXPECTED_SOURCES)
EXPECTED_LIBIMOBILEDEVICE_PATCH_CHAIN = [
    "patches/0001-fix-ostrace-live-packet-leak.patch",
    "patches/0002-interrupt-live-capture-receive.patch",
    "patches/0003-passive-lockdown-handshake.patch",
    "patches/0004-syslog-terminal-callback.patch",
    "patches/0005-ostrace-record-type-callback.patch",
]
EXPECTED_LIBIMOBILEDEVICE_FINAL_TREE_SHA256 = (
    "dc0ef5f499295cab2678810ac6cea0ea85be00e554a6395a81a36b6373bdab15"
)
EXPECTED_THIN_ARTIFACTS = {
    "x86_64": "15.0",
    "arm64": "14.0",
}
UPSTREAM_LEAK_FIX_COMMIT = "5ca453f9e3950b1f24b51e4cdf255236e34254c4"
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")


def required_text(path: pathlib.Path) -> str:
    if not path.is_file():
        raise AssertionError(f"required iOS native release contract file is missing: {path}")
    return path.read_text(encoding="utf-8")


def required_json(path: pathlib.Path) -> dict:
    try:
        value = json.loads(required_text(path))
    except json.JSONDecodeError as error:
        raise AssertionError(f"{path} is not valid JSON: {error}") from error
    if not isinstance(value, dict):
        raise AssertionError(f"{path} must contain a JSON object")
    return value


def cmake_calls(source: str, command: str) -> list[str]:
    calls = []
    lowered = source.lower()
    needle = f"{command.lower()}("
    position = 0
    while True:
        start = lowered.find(needle, position)
        if start < 0:
            return calls
        body_start = start + len(needle)
        depth = 1
        quote = None
        escaped = False
        cursor = body_start
        while cursor < len(source) and depth:
            character = source[cursor]
            if quote is not None:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    quote = None
            elif character in ('"', "'"):
                quote = character
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
            cursor += 1
        if depth:
            raise AssertionError(f"unterminated {command} call")
        calls.append(source[body_start : cursor - 1])
        position = cursor


def cmake_tokens(body: str) -> list[str]:
    uncommented = "\n".join(line.split("#", 1)[0] for line in body.splitlines())
    return shlex.split(uncommented, posix=True)


def cpm_packages(path: pathlib.Path) -> dict[str, list[str]]:
    packages = {}
    for body in cmake_calls(required_text(path), "cpmaddpackage"):
        tokens = cmake_tokens(body)
        try:
            name = tokens[tokens.index("NAME") + 1]
        except (ValueError, IndexError):
            continue
        packages[name.lower()] = tokens
    return packages


class IosStartupCiContractTest(unittest.TestCase):
    download = (
        "      - uses: actions/download-artifact@"
        + CI_MODULE.REVIEWED_ACTION_REVISIONS["actions/download-artifact"]
        + "\n        with:\n"
        "          name: ios-native-source-cache\n"
        "          path: ${{ github.workspace }}/ios-native-sources\n"
    )
    startup = (
        f"      - name: {IOS_STARTUP_STEP}\n"
        "        shell: bash\n"
        "        env:\n"
        "          KLOGG_IOS_NATIVE_ARCHIVE_ROOT: ${{ github.workspace }}/ios-native-sources\n"
        "        run: |\n"
        "          set -euo pipefail\n"
        f"          {IOS_STARTUP_COMMAND}\n"
    )

    def fixture(self, steps: str | None = None) -> str:
        return (
            "on:\n  pull_request:\n  push:\n  workflow_dispatch:\n"
            "jobs:\n"
            "  BuildIosNativeX64:\n"
            f'    if: {NATIVE_JOB_GUARD}\n'
            "    runs-on: macos-26-intel\n"
            "    steps: &ios_native_steps\n"
            + (self.download + self.startup if steps is None else steps)
            + "  BuildIosNativeArm64:\n"
            f'    if: {NATIVE_JOB_GUARD}\n'
            "    runs-on: macos-26\n"
            "    steps: *ios_native_steps\n"
        )

    def test_real_workflow_requires_actual_c_after_download_on_both_native_legs(self):
        workflow = required_text(CI_BUILD_WORKFLOW)
        self.assertEqual(ios_startup_ci_issues(workflow), [])

    def test_exact_good_and_adjacent_non_native_jobs_are_valid(self):
        self.assertEqual(ios_startup_ci_issues(self.fixture()), [])
        adjacent = (
            "  OrdinaryPythonDiscovery:\n"
            "    if: ${{ github.event_name == 'pull_request' }}\n"
            "    steps:\n      - run: python3 -m unittest discover -s tests/scripts\n"
            "        continue-on-error: true\n"
        )
        self.assertEqual(ios_startup_ci_issues(self.fixture() + adjacent), [])
        comment = "          # python3 is required even on a cold cache\n"
        commented = self.fixture().replace("          set -euo pipefail\n", comment + "          set -euo pipefail\n")
        self.assertEqual(ios_startup_ci_issues(commented), [])

    def test_removed_moved_and_soft_skipped_invocations_fail(self):
        mutations = {
            "missing": self.download,
            "before-download": self.startup + self.download,
            "after-other-step": self.download + "      - run: true\n" + self.startup,
            "duplicated": self.download + self.startup + self.startup,
            "step-if": self.download + self.startup.replace("        shell:", "        if: false\n        shell:"),
            "continue": self.download + self.startup.replace("        shell:", "        continue-on-error: true\n        shell:"),
            "secret-gate": self.download + self.startup.replace("        shell:", "        if: ${{ env.RELEASE_SECRET != '' }}\n        shell:"),
            "wrong-root": self.download + self.startup.replace("/ios-native-sources", "/missing"),
            "implicit-cache": self.download + self.startup.replace("        env:\n          KLOGG_IOS_NATIVE_ARCHIVE_ROOT: ${{ github.workspace }}/ios-native-sources\n", ""),
            "unquoted-path": self.download + self.startup.replace(IOS_STARTUP_COMMAND, IOS_STARTUP_COMMAND.replace('"', '')),
            "ignored-failure": self.download + self.startup.replace(IOS_STARTUP_COMMAND, IOS_STARTUP_COMMAND + " || true"),
            "early-exit": self.download + self.startup.replace(IOS_STARTUP_COMMAND, "exit 0\n          " + IOS_STARTUP_COMMAND),
            "download-skipped": self.download.replace("        with:", "        if: false\n        with:") + self.startup,
        }
        for name, steps in mutations.items():
            with self.subTest(mutation=name):
                self.assertTrue(ios_startup_ci_issues(self.fixture(steps)))

    def test_comments_strings_and_malformed_structure_cannot_spoof_execution(self):
        good = self.fixture()
        mutations = {
            "comment": good.replace(IOS_STARTUP_COMMAND, "# " + IOS_STARTUP_COMMAND),
            "string": good.replace(IOS_STARTUP_COMMAND, "echo '" + IOS_STARTUP_COMMAND + "'"),
            "dead-branch": good.replace(IOS_STARTUP_COMMAND, "if false; then " + IOS_STARTUP_COMMAND + "; fi"),
            "duplicate-run": good.replace("        run: |", "        run: exit 0\n        run: |"),
            "duplicate-env": good.replace("        env:", "        env: {}\n        env:"),
            "duplicate-root": good.replace("        run: |", "          KLOGG_IOS_NATIVE_ARCHIVE_ROOT: /missing\n        run: |"),
            "malformed-env": good.replace("        env:", "        env: ["),
            "malformed-jobs": good.replace("jobs:", "jobs: ["),
            "unknown-alias": good.replace("*ios_native_steps", "*missing"),
            "unknown-condition": good.replace("!contains(github.event.head_commit.message, '[skip ci]')", "fromJSON(inputs.native)"),
            "job-soft-failure": good.replace(
                "    runs-on: macos-26-intel\n",
                "    continue-on-error: true\n    runs-on: macos-26-intel\n",
                1,
            ),
            "duplicate-job": good + "  BuildIosNativeX64:\n    steps: []\n",
        }
        for name, text in mutations.items():
            with self.subTest(mutation=name):
                self.assertTrue(ios_startup_ci_issues(text))

    def test_real_tree_mutations_cannot_move_gate_to_an_unrelated_job(self):
        original = required_text(CI_BUILD_WORKFLOW)
        # Project the pending stable id in memory so this mutation test exercises
        # gate placement during RED instead of stopping at an unrelated KeyError.
        # Explicit fixture insertion also makes this mutation test useful in RED,
        # before the production step exists. Never change the on-disk workflow.
        blocks = CI_MODULE.workflow_job_blocks(original)
        self.assertIn("BuildIosNativeX64", blocks)
        block = blocks["BuildIosNativeX64"]
        steps = CI_MODULE.workflow_step_blocks(block)
        native_step = next((step for step in steps if
                            CI_MODULE.workflow_step_fields(step)[0].get("name") == IOS_STARTUP_STEP), None)
        if native_step is not None:
            original = original.replace("\n".join(native_step), "", 1)
        self.assertTrue(ios_startup_ci_issues(original))
        text = original + "\n  Unrelated:\n    steps:\n" + self.startup
        self.assertTrue(ios_startup_ci_issues(text))
        download = next(step for step in steps if
                        CI_MODULE.workflow_step_fields(step)[1].get("with", {}).get("name") == "ios-native-source-cache")
        download_text = "\n".join(download)
        wired = original.replace(download_text, download_text + "\n" + self.startup, 1)
        self.assertEqual(ios_startup_ci_issues(wired), [])

    def test_each_event_and_dispatch_mode_inherits_the_same_required_gate(self):
        actual = required_text(CI_BUILD_WORKFLOW)
        triggers = CI_MODULE.workflow_trigger_mapping(actual)
        self.assertIsNotNone(triggers)
        for event in ("pull_request", "push", "workflow_dispatch", "schedule"):
            for mode in (("validation", "release") if event == "workflow_dispatch" else (None,)):
                with self.subTest(event=event, mode=mode):
                    # No evaluator is needed: the rule accepts only the existing
                    # event-neutral job guard and forbids every step-level guard.
                    fixture = actual
                    if event != "schedule":
                        self.assertIn(event, triggers)
                    elif event not in triggers:
                        # CI Build has no schedule today; exercise an added
                        # schedule without making it a production requirement.
                        fixture = actual.replace("on:\n", "on:\n  schedule:\n    - cron: '7 9 * * *'\n", 1)
                    self.assertEqual(ios_startup_ci_issues(fixture), [])
                    guard = f"        if: ${{{{ github.event_name == '{event}'"
                    if mode:
                        guard += f" && inputs.qualification-mode == '{mode}'"
                    guard += " }}\n"
                    marker = f"      - name: {IOS_STARTUP_STEP}\n"
                    narrowed = fixture.replace(marker, marker + guard, 1)
                    self.assertTrue(ios_startup_ci_issues(narrowed))


class IosNativeReleaseContractTest(unittest.TestCase):
    def lock(self) -> dict:
        return required_json(LOCK)

    def sources(self) -> dict[str, dict]:
        sources = self.lock().get("sources")
        self.assertIsInstance(sources, list, "lock.sources must be an array")
        by_id = {source.get("id"): source for source in sources if isinstance(source, dict)}
        self.assertEqual(
            sorted(by_id),
            sorted(EXPECTED_BUILD_ORDER),
            "the lock must describe the complete source-built native dependency closure",
        )
        return by_id

    @contextlib.contextmanager
    def patched_stack_inspection(self):
        with contextlib.ExitStack() as patches:
            patches.enter_context(
                mock.patch.object(
                    VERIFY_MODULE,
                    "install_name",
                    side_effect=lambda path: f"@rpath/{path.name}",
                )
            )
            patches.enter_context(
                mock.patch.object(
                    VERIFY_MODULE, "rpaths", return_value=["@loader_path"]
                )
            )
            patches.enter_context(
                mock.patch.object(
                    VERIFY_MODULE, "architectures", return_value=["arm64"]
                )
            )
            patches.enter_context(
                mock.patch.object(
                    VERIFY_MODULE, "deployment_target", return_value="14.0"
                )
            )
            patches.enter_context(
                mock.patch.object(VERIFY_MODULE, "dependencies", return_value=[])
            )
            exported_symbols = patches.enter_context(
                mock.patch.object(
                    VERIFY_MODULE,
                    "exported_symbols",
                    return_value={"required_symbol"},
                )
            )
            yield exported_symbols

    def test_startup_burst_consumer_waits_on_notifications_without_drain_polling(self):
        source = (ROOT / "tests/unit/ios_native_stream_worker_test.cpp").read_text()
        startup = source.split('TEST_CASE( "native synchronous startup bursts', 1)[1].split(
            'TEST_CASE( "native terminal paths', 1)[0]
        self.assertIn("std::condition_variable drainChanged", startup)
        self.assertIn("callbacks.bytesAvailable =", startup)
        self.assertLess(startup.index("callbacks.bytesAvailable ="), startup.index("worker.start()"))
        self.assertIn("drainChanged.wait_until", startup)
        self.assertIn("drainRequested || startReturned.load()", startup)
        self.assertGreaterEqual(startup.count("drainChanged.notify_all()"), 2)
        self.assertIn("std::chrono::steady_clock::now() + 2s", startup)
        self.assertNotIn("const bool completed = waitForNativeCondition", startup)

    def test_catalog_dispatch_retains_its_executor_through_service_shutdown(self):
        source = required_text(IOS_LIVE_SERVICES)
        self.assertIn(
            "std::shared_ptr<BoundedSerialExecutor> catalogExecutor_", source
        )
        self.assertIn("[ executor = catalogExecutor_ ]", source)
        self.assertNotIn("[ executor = catalogExecutor_.get() ]", source)
        self.assertIn("catalogExecutor_->shutdownAsync();", source)

    def test_stop_acknowledgement_requires_cleanup_dispatch(self):
        source = required_text(IOS_NATIVE_STREAM)
        stop = re.search(
            r"void IosNativeStreamWorker::stop\([^)]*\) noexcept\s*\{(.*?)\n\}",
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(stop)
        body = stop.group(1)
        self.assertIn("state->scheduleCleanup()", body)
        self.assertNotIn("publishStopped", body)

    def test_legacy_syslog_production_callback_has_no_test_operation_counters(self):
        source = required_text(IOS_NATIVE_STREAM)
        header = required_text(IOS_NATIVE_HEADER)
        forbidden = (
            "legacySyslogOperationsForTesting",
            "syslogCallbackEntriesForTesting",
            "syslogAssemblyLocksForTesting",
            "syslogRecordBufferGrowthsForTesting",
            "syslogCompletionBufferAllocationsForTesting",
            "syslogCompletionCopiedBytesForTesting",
        )
        for token in forbidden:
            self.assertNotIn(token, source)
            self.assertNotIn(token, header)

    def generate_legal_fixture(
        self, root: pathlib.Path, version: str, base_url: str,
        architecture: str = "arm64",
    ) -> tuple[pathlib.Path, pathlib.Path]:
        stack = root / "stack"
        output = root / "legal"
        (stack / "lib").mkdir(exist_ok=True, parents=True)
        dylib = stack / "lib" / "libfixture.dylib"
        dylib.write_bytes(f"native binary {architecture}".encode())
        build = stack / "ios-native-build-receipt.json"
        if not build.exists():
            build.write_text(json.dumps({
                "schema_version": 1, "receipt_kind": "ios-native-build",
                "lock_sha256": LEGAL_MODULE.sha256(LOCK), "architecture": architecture,
                "deployment_target": EXPECTED_THIN_ARTIFACTS[architecture],
                "native_qualified": True, "qualification": "native",
                "dylibs": [{"name": dylib.name, "sha256": LEGAL_MODULE.sha256(dylib)}],
            }) + "\n", encoding="utf-8")
        argv = [str(LEGAL_SCRIPT), "--lock", str(LOCK), "--archive-root", str(root),
                "--repository-root", str(ROOT), "--stack-root", str(stack),
                "--version", version, "--base-url", base_url, "--output", str(output)]

        def archive_fixture(path, *_args):
            path.write_bytes(b"fixed source closure")

        with mock.patch.object(sys, "argv", argv), mock.patch.object(
            LEGAL_MODULE, "write_deterministic_source_archive", side_effect=archive_fixture
        ), mock.patch.object(LEGAL_MODULE, "extract_legal_file", return_value=b"license\n"):
            self.assertEqual(LEGAL_MODULE.main(), 0)
        return stack, output

    def test_version_and_url_rebuild_only_overlay_without_mutating_binary_or_legal_core(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, output = self.generate_legal_fixture(root, "26.09.27", "https://example.org/one")
            core_names = ("ios-native-corresponding-source.tar.gz",
                          "ios-native-source-set-receipt.json", "ios-native-source-receipt.json",
                          "ios-native-sbom.spdx.json", "ios-native-legal-receipt.json",
                          "NOTICE-ios-native.txt")
            original = {name: (output / name).read_bytes() for name in core_names}
            original_build = (stack / "ios-native-build-receipt.json").read_bytes()
            original_overlay = (output / "ios-native-source-offer.txt").read_bytes()
            self.generate_legal_fixture(root, "26.09.28", "https://example.org/two")
            self.assertEqual(original, {name: (output / name).read_bytes() for name in core_names})
            self.assertEqual(original_build, (stack / "ios-native-build-receipt.json").read_bytes())
            self.assertNotEqual(original_overlay, (output / "ios-native-source-offer.txt").read_bytes())
            self.assertTrue((output / "ios-native-overlay-receipt.json").is_file())
            self.assertIn("26.09.28", (output / "ios-native-source-offer.txt").read_text())

    def legal_mode_argv(self, core: pathlib.Path, stack: pathlib.Path, output: pathlib.Path,
                        mode: str = "--overlay-only"):
        return [str(LEGAL_SCRIPT), "--lock", str(LOCK), "--core-root", str(core),
                "--stack-root", str(stack), mode, "--version", "26.10.15",
                "--base-url", "https://example.org/new", "--output", str(output),
                "--source-set-sha256", LEGAL_MODULE.sha256(core / "ios-native-source-set-receipt.json"),
                "--legal-receipt-sha256", LEGAL_MODULE.sha256(core / "ios-native-legal-receipt.json"),
                "--build-receipt-sha256", LEGAL_MODULE.sha256(stack / "ios-native-build-receipt.json")]

    def tree_bytes(self, root: pathlib.Path):
        return {path.relative_to(root).as_posix(): path.read_bytes()
                for path in root.rglob("*") if path.is_file()}

    @unittest.skipIf(os.name == "nt", "requires POSIX dylib aliases and artifact modes")
    def test_assembled_stack_preserves_originals_and_matches_complete_cold_output(self):
        for architecture in EXPECTED_THIN_ARTIFACTS:
            with self.subTest(architecture=architecture), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                stack, original = self.generate_legal_fixture(
                    root, "26.10.15", "https://example.org/new", architecture)
                header = stack / "include" / "fixture" / "api.h"
                header.parent.mkdir(parents=True)
                header.write_bytes(b"original installed header\n")
                (stack / "include" / "empty").mkdir()
                (stack / "lib" / "libfixture.dylib").chmod(0o755)
                alias = stack / "lib" / "libfixture-alias.dylib"
                alias.symlink_to("libfixture.dylib")
                stack_before, original_before = self.tree_bytes(stack), self.tree_bytes(original)
                core = root / "core"
                argv = [str(LEGAL_SCRIPT), "--lock", str(LOCK), "--stack-root", str(stack),
                        "--archive-root", str(root), "--repository-root", str(ROOT),
                        "--core-only", "--output", str(core)]
                with mock.patch.object(sys, "argv", argv), mock.patch.object(
                    LEGAL_MODULE, "write_deterministic_source_archive",
                    side_effect=lambda path, *_args: path.write_bytes(b"fixed source closure")
                ), mock.patch.object(LEGAL_MODULE, "extract_legal_file", return_value=b"license\n"):
                    self.assertEqual(LEGAL_MODULE.main(), 0)
                core_before = self.tree_bytes(core)
                output = root / "assembled"
                argv = self.legal_mode_argv(core, stack, output, "--assemble-stack")
                error = io.StringIO()
                with mock.patch.object(sys, "argv", argv), mock.patch.object(
                    LEGAL_MODULE, "write_deterministic_source_archive",
                    side_effect=AssertionError("assembly must not rebuild corresponding source")
                ), mock.patch.object(LEGAL_MODULE, "extract_legal_file",
                                     side_effect=AssertionError("assembly must not acquire sources")), \
                        mock.patch.object(subprocess, "run", side_effect=AssertionError("no native tools")), \
                        contextlib.redirect_stderr(error):
                    try:
                        result = LEGAL_MODULE.main()
                    except SystemExit as exit_error:
                        result = exit_error.code
                self.assertEqual(result, 0, error.getvalue())
                self.assertEqual(self.tree_bytes(output), {**stack_before, **original_before})
                self.assertTrue((output / alias.relative_to(stack)).is_symlink())
                self.assertEqual(os.readlink(output / alias.relative_to(stack)), os.readlink(alias))
                self.assertTrue((output / "include" / "empty").is_dir())
                self.assertEqual((output / "lib" / "libfixture.dylib").stat().st_mode & 0o777, 0o755)
                self.assertEqual((self.tree_bytes(stack), self.tree_bytes(original),
                                  self.tree_bytes(core)),
                                 (stack_before, original_before, core_before))

    @unittest.skipIf(os.name == "nt", "requires POSIX dylib aliases and artifact modes")
    def test_full_release_assembly_reuses_original_native_and_legal_bytes(self):
        for architecture in EXPECTED_THIN_ARTIFACTS:
            for version in ("26.10.15", "26.10.16"):
                with self.subTest(architecture=architecture, version=version), \
                        tempfile.TemporaryDirectory() as temporary:
                    root = pathlib.Path(temporary)
                    stack, legal = self.generate_legal_fixture(
                        root, "26.10.15", "https://example.org/new", architecture)
                    header = stack / "include" / "fixture" / "api.h"
                    header.parent.mkdir(parents=True)
                    header.write_bytes(b"authenticated original header\n")
                    header.chmod(0o640)
                    (stack / "include" / "empty").mkdir(mode=0o750)
                    (stack / "lib" / "libfixture.dylib").chmod(0o755)
                    (stack / "lib" / "libfixture-alias.dylib").symlink_to("libfixture.dylib")
                    native_before = self.tree_bytes(stack)
                    full = root / "full"
                    shutil.copytree(stack, full, symlinks=True)
                    shutil.copytree(legal, full, dirs_exist_ok=True)
                    receipt_alias = f"ios-native-{architecture}-build-receipt.json"
                    shutil.copy2(full / "ios-native-build-receipt.json", full / receipt_alias)
                    native_before[receipt_alias] = (full / receipt_alias).read_bytes()
                    (full / "SHA256SUMS").write_text("".join(
                        f"{LEGAL_MODULE.sha256(path)}  {path.relative_to(full).as_posix()}\n"
                        for path in sorted(full.rglob("*"))
                        if path.is_file() and not path.is_symlink()))
                    before = self.tree_bytes(full)
                    _, expected = self.generate_legal_fixture(
                        root / "expected", version, "https://example.org/new", architecture)
                    args = argparse.Namespace(
                        lock=LOCK, stack_root=full, core_root=full, output=root / "assembled",
                        source_set_sha256=LEGAL_MODULE.sha256(full / "ios-native-source-set-receipt.json"),
                        legal_receipt_sha256=LEGAL_MODULE.sha256(full / "ios-native-legal-receipt.json"),
                        build_receipt_sha256=LEGAL_MODULE.sha256(full / "ios-native-build-receipt.json"))
                    with mock.patch.object(LEGAL_MODULE, "write_deterministic_source_archive",
                                           side_effect=AssertionError("no source acquisition")), \
                            mock.patch.object(subprocess, "run",
                                              side_effect=AssertionError("no native tools")):
                        LEGAL_MODULE.assemble_original_stack(
                            args, json.loads(LOCK.read_bytes()), version,
                            "https://example.org/new", full_release=True)
                    self.assertEqual(self.tree_bytes(args.output),
                                     {**native_before, **self.tree_bytes(expected)})
                    self.assertEqual(self.tree_bytes(full), before)
                    self.assertEqual((args.output / header.relative_to(stack)).stat().st_mode & 0o777,
                                     0o640)
                    self.assertEqual((args.output / "include/empty").stat().st_mode & 0o777, 0o750)
                    self.assertEqual(os.readlink(args.output / "lib/libfixture-alias.dylib"),
                                     "libfixture.dylib")

    @unittest.skipIf(os.name == "nt", "requires POSIX original stack assembly")
    def test_full_release_assembly_rejects_extra_inventory_and_changed_original_overlay(self):
        for mutation in ("foreign-entry", "changed-overlay", "wrong-receipt-alias"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                stack, legal = self.generate_legal_fixture(root, "26.10.15", "https://example.org/new")
                (stack / "include").mkdir()
                full = root / "full"
                shutil.copytree(stack, full)
                shutil.copytree(legal, full, dirs_exist_ok=True)
                receipt_alias = full / "ios-native-arm64-build-receipt.json"
                shutil.copy2(full / "ios-native-build-receipt.json", receipt_alias)
                if mutation == "wrong-receipt-alias":
                    receipt_alias.write_bytes(b"foreign receipt")
                (full / "SHA256SUMS").write_text("fixture envelope\n")
                args = argparse.Namespace(
                    lock=LOCK, stack_root=full, core_root=full, output=root / "assembled",
                    source_set_sha256=LEGAL_MODULE.sha256(full / "ios-native-source-set-receipt.json"),
                    legal_receipt_sha256=LEGAL_MODULE.sha256(full / "ios-native-legal-receipt.json"),
                    build_receipt_sha256=LEGAL_MODULE.sha256(full / "ios-native-build-receipt.json"))
                if mutation == "foreign-entry":
                    (full / "licenses/unlisted").write_bytes(b"foreign input")
                original_copy = LEGAL_MODULE.copy_checked_files

                def copy_then_mutate(source, output, *args, **kwargs):
                    original_copy(source, output, *args, **kwargs)
                    if mutation == "changed-overlay" and source == full and output == root / "assembled":
                        (full / "ios-native-source-offer.txt").write_bytes(b"changed input")

                message = {"foreign-entry": "unexpected original native stack entry",
                           "changed-overlay": "full legal entry changed",
                           "wrong-receipt-alias": "receipt alias differs"}[mutation]
                with mock.patch.object(LEGAL_MODULE, "copy_checked_files", side_effect=copy_then_mutate):
                    with self.assertRaisesRegex(RuntimeError, message):
                        LEGAL_MODULE.assemble_original_stack(
                            args, json.loads(LOCK.read_bytes()), "26.10.16",
                            "https://example.org/new", full_release=True)
                if mutation == "foreign-entry":
                    self.assertFalse(args.output.exists())
                    self.assertEqual((full / "licenses/unlisted").read_bytes(), b"foreign input")
                elif mutation == "changed-overlay":
                    self.assertTrue(args.output.is_dir())
                    self.assertEqual((full / "ios-native-source-offer.txt").read_bytes(), b"changed input")
                else:
                    self.assertFalse(args.output.exists())
                    self.assertEqual(receipt_alias.read_bytes(), b"foreign receipt")

    def test_ordinary_ios_builder_assembles_pinned_core_without_changing_artifact_layout(self):
        text = CI_BUILD_WORKFLOW.read_text()
        jobs = CI_MODULE.workflow_mapping_block(text.splitlines(), "jobs", 0)
        self.assertIsNotNone(jobs)
        steps = [CI_MODULE.workflow_step_fields(step)[0]
                 for step in CI_MODULE.workflow_step_blocks(jobs["BuildIosNativeX64"][1])]
        build = [step for step in steps
                 if step.get("name") == "Build disconnected thin iOS native stack"]
        self.assertEqual(len(build), 1)
        run = build[0]["run"]
        commands = [shlex.split(line.strip()) for line in run.replace("\\\n", " ").splitlines()
                    if line.strip().startswith("python3 ")]
        legal = [command for command in commands
                 if command[1] == "scripts/build_ios_native_legal_assets.py"]
        self.assertEqual(len(legal), 2, "ordinary builder must split immutable core and final assembly")
        core, assembled = legal
        native = next(command for command in commands
                      if command[1] == "scripts/build_ios_native_stack.py")
        def argument(command, flag):
            return command[command.index(flag) + 1]

        self.assertIn("--core-only", core)
        self.assertIn("--assemble-stack", assembled)
        self.assertEqual(argument(native, "--artifact-root"), "$original_root")
        self.assertEqual(argument(core, "--stack-root"), "$original_root")
        self.assertEqual(argument(core, "--output"), "$core_root")
        self.assertEqual(argument(assembled, "--stack-root"), "$original_root")
        self.assertEqual(argument(assembled, "--core-root"), "$core_root")
        self.assertEqual(argument(assembled, "--output"), "$stack_root")
        for flag in ("--source-set-sha256", "--legal-receipt-sha256", "--build-receipt-sha256"):
            self.assertIn(flag, assembled)
        self.assertNotIn("--archive-root", assembled)
        self.assertNotIn("--repository-root", assembled)
        self.assertIn('stack_root="${{ runner.temp }}/ios-native-stack"', run)
        self.assertIn('core_root="${{ runner.temp }}/ios-native-legal-core"', run)
        self.assertIn('original_root="${{ runner.temp }}/ios-native-original"', run)
        self.assertEqual(ios_startup_ci_issues(text), [])

    @unittest.skipIf(os.name == "nt", "requires POSIX native artifact permission modes")
    def test_assembled_stack_rejects_unreceipted_libraries_and_special_root_modes(self):
        for invalid in ("unreceipted-library", "special-root-mode"):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
                (stack / "include").mkdir()
                if invalid == "unreceipted-library":
                    (stack / "lib" / "foreign.dylib").write_bytes(b"not in original build receipt")
                else:
                    stack.chmod(0o2755)
                    self.assertTrue(stack.stat().st_mode & 0o2000)
                before = self.tree_bytes(stack), self.tree_bytes(core)
                output = root / "assembled"
                argv = self.legal_mode_argv(core, stack, output, "--assemble-stack")
                with mock.patch.object(sys, "argv", argv), self.assertRaises(RuntimeError):
                    LEGAL_MODULE.main()
                self.assertFalse(output.exists())
                self.assertEqual((self.tree_bytes(stack), self.tree_bytes(core)), before)

    @unittest.skipIf(os.name == "nt", "requires POSIX native stack assembly semantics")
    def test_assembled_stack_rejects_and_preserves_foreign_output_entries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            (stack / "include").mkdir()
            output = root / "assembled"
            before = self.tree_bytes(stack), self.tree_bytes(core)
            argv = self.legal_mode_argv(core, stack, output, "--assemble-stack")
            copy = LEGAL_MODULE.copy_checked_files

            def install_foreign_entry(source, destination, files, created=None, **kwargs):
                if source == stack:
                    (destination / "foreign.txt").write_bytes(b"foreign output must survive")
                return copy(source, destination, files, created, **kwargs)

            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                LEGAL_MODULE, "copy_checked_files", side_effect=install_foreign_entry
            ), self.assertRaisesRegex(RuntimeError, "inventory"):
                LEGAL_MODULE.main()
            self.assertEqual((output / "foreign.txt").read_bytes(), b"foreign output must survive")
            self.assertEqual((self.tree_bytes(stack), self.tree_bytes(core)), before)

    @unittest.skipIf(os.name == "nt", "requires POSIX native dylib alias validation")
    def test_assembly_rejects_existing_output_and_escaping_alias_without_repair(self):
        for invalid in ("existing-output", "escaping-alias"):
            with self.subTest(invalid=invalid), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
                (stack / "include").mkdir()
                output = root / "assembled"
                witness = root / "witness"
                witness.write_bytes(b"outside immutable inputs")
                if invalid == "existing-output":
                    output.mkdir()
                    (output / "keep").write_bytes(b"foreign existing output")
                else:
                    (stack / "lib" / "escape.dylib").symlink_to("../../witness")
                before = self.tree_bytes(stack), self.tree_bytes(core)
                argv = self.legal_mode_argv(core, stack, output, "--assemble-stack")
                with mock.patch.object(sys, "argv", argv), self.assertRaises(RuntimeError):
                    LEGAL_MODULE.main()
                self.assertEqual((self.tree_bytes(stack), self.tree_bytes(core)), before)
                self.assertEqual(witness.read_bytes(), b"outside immutable inputs")
                if invalid == "existing-output":
                    self.assertEqual(self.tree_bytes(output), {"keep": b"foreign existing output"})
                else:
                    self.assertFalse(output.exists())
                    self.assertEqual(os.readlink(stack / "lib" / "escape.dylib"), "../../witness")

    @unittest.skipIf(os.name == "nt", "requires POSIX native stack assembly semantics")
    def test_assembly_rejects_header_mutation_and_retains_partial_output_without_repair(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            header = stack / "include" / "api.h"
            header.parent.mkdir()
            header.write_bytes(b"original installed header")
            core_before = self.tree_bytes(core)
            output = root / "assembled"
            argv = self.legal_mode_argv(core, stack, output, "--assemble-stack")
            copy = LEGAL_MODULE.copy_checked_files

            def mutate_before_copy(source, destination, files, created=None, **kwargs):
                if source == stack:
                    header.write_bytes(b"changed before copy")
                    (destination / "foreign.txt").write_bytes(b"foreign partial output")
                return copy(source, destination, files, created, **kwargs)

            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                LEGAL_MODULE, "copy_checked_files", side_effect=mutate_before_copy
            ), self.assertRaisesRegex(RuntimeError, "changed while copying"):
                LEGAL_MODULE.main()
            self.assertEqual(header.read_bytes(), b"changed before copy")
            self.assertEqual(self.tree_bytes(core), core_before)
            self.assertEqual((output / "foreign.txt").read_bytes(), b"foreign partial output")
            self.assertFalse((output / "SHA256SUMS").exists())

    @unittest.skipIf(os.name == "nt", "requires POSIX output metadata and symlink semantics")
    def test_assembly_does_not_chmod_a_foreign_file_substituted_after_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            (stack / "include").mkdir()
            (stack / "lib" / "libfixture.dylib").chmod(0o755)
            witness = root / "witness"
            witness.write_bytes(b"foreign file")
            witness.chmod(0o600)
            output = root / "assembled"
            argv = self.legal_mode_argv(core, stack, output, "--assemble-stack")
            copy = LEGAL_MODULE.copy_checked_files

            def replace_after_copy(source, destination, files, created=None, **kwargs):
                result = copy(source, destination, files, created, **kwargs)
                if destination == output and source != stack:
                    copied = output / "lib" / "libfixture.dylib"
                    copied.unlink()
                    copied.symlink_to(witness)
                return result

            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                LEGAL_MODULE, "copy_checked_files", side_effect=replace_after_copy
            ), self.assertRaises(RuntimeError):
                LEGAL_MODULE.main()
            self.assertEqual(witness.read_bytes(), b"foreign file")
            self.assertEqual(witness.stat().st_mode & 0o777, 0o600)

    @unittest.skipIf(os.name == "nt", "requires POSIX directory handles and symlink semantics")
    def test_assembly_keeps_directory_metadata_bound_to_its_created_handle(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            (stack / "include").mkdir()
            witness = root / "foreign-directory"
            witness.mkdir(mode=0o750)
            output = root / "assembled"
            argv = self.legal_mode_argv(core, stack, output, "--assemble-stack")
            inventory = LEGAL_MODULE.native_stack_inventory
            observations = 0

            def replace_before_metadata(source, receipt, *, legal_files=None, receipt_alias=None):
                nonlocal observations
                result = inventory(source, receipt, legal_files=legal_files, receipt_alias=receipt_alias)
                observations += 1
                if observations == 2:
                    copied = output / "include"
                    copied.rmdir()
                    copied.symlink_to(witness, target_is_directory=True)
                return result

            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                LEGAL_MODULE, "native_stack_inventory", side_effect=replace_before_metadata
            ), self.assertRaisesRegex(RuntimeError, "directory identity changed"):
                LEGAL_MODULE.main()
            self.assertEqual(observations, 2)
            self.assertEqual(witness.stat().st_mode & 0o777, 0o750)
            self.assertTrue((output / "include").is_symlink())

    @unittest.skipIf(os.name == "nt", "requires POSIX held-directory and alias semantics")
    def test_assembly_never_writes_through_a_replaced_output_parent(self):
        for closure in ("lib", "licenses"):
            with self.subTest(closure=closure), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
                (stack / "include").mkdir()
                witness = root / "foreign-directory"
                witness.mkdir()
                output = root / "assembled"
                before = self.tree_bytes(stack), self.tree_bytes(core)
                argv = self.legal_mode_argv(core, stack, output, "--assemble-stack")
                copy = LEGAL_MODULE.copy_checked_files

                def replace_parent(source, destination, files, created=None, **kwargs):
                    if destination == output and (source == stack) == (closure == "lib"):
                        parent = output / closure
                        if parent.exists():
                            parent.rmdir()
                        parent.symlink_to(witness, target_is_directory=True)
                    return copy(source, destination, files, created, **kwargs)

                with mock.patch.object(sys, "argv", argv), mock.patch.object(
                    LEGAL_MODULE, "copy_checked_files", side_effect=replace_parent
                ), self.assertRaises((RuntimeError, OSError)):
                    LEGAL_MODULE.main()
                self.assertEqual(list(witness.iterdir()), [])
                self.assertEqual((self.tree_bytes(stack), self.tree_bytes(core)), before)

    @unittest.skipIf(os.name == "nt", "requires POSIX native file-mode validation")
    def test_assembly_rejects_output_mode_changed_after_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            (stack / "include").mkdir()
            (stack / "lib" / "libfixture.dylib").chmod(0o755)
            output = root / "assembled"
            argv = self.legal_mode_argv(core, stack, output, "--assemble-stack")
            copy = LEGAL_MODULE.copy_checked_files

            def change_mode(source, destination, files, created=None, **kwargs):
                result = copy(source, destination, files, created, **kwargs)
                if destination == output and source != stack:
                    (output / "lib" / "libfixture.dylib").chmod(0o600)
                return result

            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                LEGAL_MODULE, "copy_checked_files", side_effect=change_mode
            ), self.assertRaisesRegex(RuntimeError, "mode changed"):
                LEGAL_MODULE.main()
            self.assertEqual((stack / "lib" / "libfixture.dylib").stat().st_mode & 0o777, 0o755)

    def test_overlay_only_rejects_original_build_bound_to_a_different_source_core(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            source_path = core / "ios-native-source-set-receipt.json"
            build_path = stack / "ios-native-build-receipt.json"
            legal_path = core / "ios-native-legal-receipt.json"
            original_source_hash = LEGAL_MODULE.sha256(source_path)
            build = json.loads(build_path.read_text())
            build["source_set_receipt_sha256"] = original_source_hash
            build_path.write_text(json.dumps(build), encoding="utf-8")
            # Model a newly generated reproduction archive under the unchanged
            # dependency lock, not a new build or a rewritten build authority.
            archive = core / "ios-native-corresponding-source.tar.gz"
            archive.write_bytes(archive.read_bytes() + b"new reproduction scripts")
            source = json.loads(source_path.read_text())
            source["archive"]["sha256"] = LEGAL_MODULE.sha256(archive)
            source_path.write_text(json.dumps(source), encoding="utf-8")
            (core / "ios-native-source-receipt.json").write_bytes(source_path.read_bytes())
            legal = json.loads(legal_path.read_text())
            legal["source_set_receipt_sha256"] = LEGAL_MODULE.sha256(source_path)
            legal["build_receipt_sha256"] = LEGAL_MODULE.sha256(build_path)
            legal_path.write_text(json.dumps(legal), encoding="utf-8")
            LEGAL_MODULE.write_overlay(core, source, "26.09.27", "https://example.org/old")
            self.assertNotEqual(LEGAL_MODULE.sha256(source_path), original_source_hash)
            ordinary = [str(VERIFY_SCRIPT), "--lock", str(LOCK), "--stack-root", str(stack),
                        "--architecture", "arm64", "--asset-scope", "release",
                        "--source-assets-root", str(core), "--source-receipt", str(source_path),
                        "--legal-receipt", str(legal_path), "--sbom", str(core / "ios-native-sbom.spdx.json")]
            error = io.StringIO()
            with mock.patch.object(sys, "argv", ordinary), mock.patch.object(
                VERIFY_MODULE, "verify_stack", return_value=build["dylibs"]
            ), contextlib.redirect_stderr(error):
                self.assertEqual(VERIFY_MODULE.main(), 1)
            self.assertIn("stale source-set receipt binding", error.getvalue())
            output = root / "output"
            argv = self.legal_mode_argv(core, stack, output)
            before = self.tree_bytes(core), self.tree_bytes(stack)
            with mock.patch.object(sys, "argv", argv), self.assertRaisesRegex(
                RuntimeError, "stale source-set receipt binding"
            ):
                LEGAL_MODULE.main()
            self.assertFalse(output.exists())
            self.assertEqual((self.tree_bytes(core), self.tree_bytes(stack)), before)

    def test_overlay_only_accepts_absent_null_or_matching_original_build_source_binding(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            build_path = stack / "ios-native-build-receipt.json"
            legal_path = core / "ios-native-legal-receipt.json"
            original_build = json.loads(build_path.read_text())
            source_hash = LEGAL_MODULE.sha256(core / "ios-native-source-set-receipt.json")
            for label, binding in (("absent", None), ("null", None), ("matching", source_hash)):
                build = dict(original_build)
                if label != "absent":
                    build["source_set_receipt_sha256"] = binding
                build_path.write_text(json.dumps(build), encoding="utf-8")
                legal = json.loads(legal_path.read_text())
                legal["build_receipt_sha256"] = LEGAL_MODULE.sha256(build_path)
                legal_path.write_text(json.dumps(legal), encoding="utf-8")
                output = root / label
                argv = self.legal_mode_argv(core, stack, output)
                before = self.tree_bytes(core), self.tree_bytes(stack)
                with self.subTest(binding=label), mock.patch.object(sys, "argv", argv):
                    self.assertEqual(LEGAL_MODULE.main(), 0)
                self.assertEqual((self.tree_bytes(core), self.tree_bytes(stack)), before)
                self.assertEqual((output / legal_path.name).read_bytes(), legal_path.read_bytes())

    def test_core_only_and_pinned_overlay_only_reuse_immutable_bytes_without_rebuilding(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, original = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            immutable = self.tree_bytes(original)
            for name in ("ios-native-source-offer.txt", "ios-native-lgpl-replacement.txt",
                         "ios-native-overlay-receipt.json"):
                immutable.pop(name)
            core = root / "core"
            argv = [str(LEGAL_SCRIPT), "--lock", str(LOCK), "--stack-root", str(stack),
                    "--archive-root", str(root), "--repository-root", str(ROOT),
                    "--core-only", "--output", str(core)]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                LEGAL_MODULE, "write_deterministic_source_archive",
                side_effect=lambda path, *_args: path.write_bytes(b"fixed source closure")
            ), mock.patch.object(LEGAL_MODULE, "extract_legal_file", return_value=b"license\n"):
                self.assertEqual(LEGAL_MODULE.main(), 0)
            self.assertEqual(self.tree_bytes(core), immutable)
            core_before, stack_before = self.tree_bytes(core), self.tree_bytes(stack)
            for version, url in (("26.10.15", "https://example.org/new"),
                                 ("26.10.16", "https://example.org/other")):
                output = root / version
                if version == "26.10.16":
                    output.mkdir()
                argv = self.legal_mode_argv(core, stack, output)
                argv[argv.index("--version") + 1] = version
                argv[argv.index("--base-url") + 1] = url
                with mock.patch.object(sys, "argv", argv), mock.patch.object(
                    LEGAL_MODULE, "write_deterministic_source_archive",
                    side_effect=AssertionError("overlay must not regenerate source")
                ), mock.patch.object(LEGAL_MODULE, "extract_legal_file",
                                     side_effect=AssertionError("overlay must not reopen upstream sources")), \
                        mock.patch.object(subprocess, "run", side_effect=AssertionError("no native tools")):
                    self.assertEqual(LEGAL_MODULE.main(), 0)
                materialized = self.tree_bytes(output)
                self.assertEqual({name: materialized[name] for name in immutable}, immutable)
                self.assertIn(f"klogg-v{version}", (output / "ios-native-source-offer.txt").read_text())
                self.assertIn(url, (output / "ios-native-source-offer.txt").read_text())
                hashes = VERIFY_MODULE.verify_legal_assets(
                    LOCK, "arm64", output / "ios-native-source-set-receipt.json",
                    output / "ios-native-legal-receipt.json", output / "ios-native-sbom.spdx.json",
                    source_assets_root=output, build_receipt_path=stack / "ios-native-build-receipt.json")
                self.assertEqual(hashes["source_set_receipt_sha256"], LEGAL_MODULE.sha256(
                    core / "ios-native-source-set-receipt.json"))
            self.assertEqual(self.tree_bytes(core), core_before)
            self.assertEqual(self.tree_bytes(stack), stack_before)

    def test_overlay_only_rejects_unpinned_mutated_or_overlapping_inputs_before_writes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            output = root / "output"
            good = self.legal_mode_argv(core, stack, output)
            for flag in ("--source-set-sha256", "--legal-receipt-sha256", "--build-receipt-sha256"):
                for replacement in (None, "0" * 64, "a" * 64):
                    argv = list(good)
                    index = argv.index(flag)
                    if replacement is None:
                        del argv[index:index + 2]
                    else:
                        argv[index + 1] = replacement
                    with self.subTest(flag=flag, replacement=replacement), \
                            mock.patch.object(sys, "argv", argv), contextlib.redirect_stderr(io.StringIO()), \
                            self.assertRaises((RuntimeError, SystemExit)):
                        LEGAL_MODULE.main()
                    self.assertFalse(output.exists())
            for path in (core / "ios-native-corresponding-source.tar.gz",
                         core / "NOTICE-ios-native.txt", core / "ios-native-sbom.spdx.json",
                         core / "ios-native-source-set-receipt.json", core / "ios-native-legal-receipt.json",
                         stack / "ios-native-build-receipt.json", stack / "lib/libfixture.dylib",
                         next((core / "licenses").iterdir())):
                before = path.read_bytes()
                path.write_bytes(before + b"tampered")
                changed_core, changed_stack = self.tree_bytes(core), self.tree_bytes(stack)
                try:
                    with self.subTest(path=path.name), mock.patch.object(sys, "argv", good), \
                            self.assertRaises(RuntimeError):
                        LEGAL_MODULE.main()
                    self.assertFalse(output.exists())
                    self.assertEqual(self.tree_bytes(core), changed_core)
                    self.assertEqual(self.tree_bytes(stack), changed_stack)
                finally:
                    path.write_bytes(before)
            for destination in (core, stack, core / "nested", root):
                argv = self.legal_mode_argv(core, stack, destination)
                core_before, stack_before = self.tree_bytes(core), self.tree_bytes(stack)
                with self.subTest(destination=destination), mock.patch.object(sys, "argv", argv), \
                        self.assertRaises(RuntimeError):
                    LEGAL_MODULE.main()
                self.assertEqual(self.tree_bytes(core), core_before)
                self.assertEqual(self.tree_bytes(stack), stack_before)
                self.assertFalse((core / "nested").exists())
            output.mkdir()
            (output / "keep").write_bytes(b"existing output")
            with mock.patch.object(sys, "argv", good), self.assertRaises(RuntimeError):
                LEGAL_MODULE.main()
            self.assertEqual(self.tree_bytes(output), {"keep": b"existing output"})

    def test_overlay_only_rechecks_complete_output_and_cleans_copy_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            output = root / "output"
            argv = self.legal_mode_argv(core, stack, output)
            core_before, stack_before = self.tree_bytes(core), self.tree_bytes(stack)
            digest = LEGAL_MODULE.sha256
            changed = False

            def change_earlier_copy(path):
                nonlocal changed
                observed = digest(path)
                earlier = output / "ios-native-source-set-receipt.json"
                if not changed and output in path.parents and path != earlier and earlier.exists():
                    earlier.write_bytes(earlier.read_bytes() + b"changed during another copy")
                    changed = True
                return observed

            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                LEGAL_MODULE, "sha256", side_effect=change_earlier_copy
            ), self.assertRaises(RuntimeError):
                LEGAL_MODULE.main()
            self.assertFalse(output.exists())
            self.assertEqual(self.tree_bytes(core), core_before)
            self.assertEqual(self.tree_bytes(stack), stack_before)

    def test_overlay_only_rejects_source_changed_after_validation_without_repairing_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            output = root / "output"
            argv = self.legal_mode_argv(core, stack, output)
            source_archive = core / "ios-native-corresponding-source.tar.gz"
            changed = source_archive.read_bytes() + b"changed after validation"
            copy = LEGAL_MODULE.copy_checked_files

            def change_before_copy(source, target, files, created=None):
                if source == core:
                    source_archive.write_bytes(changed)
                copy(source, target, files, created)

            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                LEGAL_MODULE, "copy_checked_files", side_effect=change_before_copy
            ), self.assertRaises(RuntimeError):
                LEGAL_MODULE.main()
            self.assertFalse(output.exists())
            self.assertEqual(source_archive.read_bytes(), changed)

    def test_new_legal_modes_retain_parser_receipt_and_output_guards(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, core = self.generate_legal_fixture(root, "26.09.27", "https://example.org/old")
            output = root / "output"
            overlay = self.legal_mode_argv(core, stack, output)
            core_only = [str(LEGAL_SCRIPT), "--lock", str(LOCK), "--stack-root", str(stack),
                         "--archive-root", str(root), "--repository-root", str(ROOT),
                         "--core-only", "--output", str(output)]
            for argv in (overlay + ["--core-only"], overlay + ["--archive-root", str(root)],
                         core_only + ["--version", "26.10.15"], core_only + ["--core-root", str(core)]):
                with self.subTest(argv=argv), mock.patch.object(sys, "argv", argv), \
                        contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    LEGAL_MODULE.main()
                self.assertFalse(output.exists())
            build = stack / "ios-native-build-receipt.json"
            original = build.read_bytes()
            for changes in ({"schema_version": True}, {"native_qualified": False},
                            {"architecture": "x86_64"}, {"deployment_target": "1.0"}):
                document = json.loads(original)
                document.update(changes)
                build.write_text(json.dumps(document), encoding="utf-8")
                try:
                    for argv in (core_only, self.legal_mode_argv(core, stack, output)):
                        with self.subTest(changes=changes, mode=argv), mock.patch.object(sys, "argv", argv), \
                                self.assertRaises(RuntimeError):
                            LEGAL_MODULE.main()
                        self.assertFalse(output.exists())
                finally:
                    build.write_bytes(original)
            witness = root / "empty-witness"
            witness.mkdir()
            output.symlink_to(witness, target_is_directory=True)
            with mock.patch.object(sys, "argv", overlay), self.assertRaises(RuntimeError):
                LEGAL_MODULE.main()
            self.assertTrue(output.is_symlink())
            self.assertEqual(list(witness.iterdir()), [])
            output.unlink()
            core_before, stack_before = self.tree_bytes(core), self.tree_bytes(stack)
            copy = LEGAL_MODULE.copy_checked_files

            def fail_partial_copy(source, target, files, created=None):
                if target == output:
                    name = next(iter(files))
                    copy(source, target, {name: files[name]}, created)
                    raise OSError("injected copy failure")
                copy(source, target, files, created)

            with mock.patch.object(sys, "argv", overlay), mock.patch.object(
                LEGAL_MODULE, "copy_checked_files", side_effect=fail_partial_copy
            ), self.assertRaises(OSError):
                LEGAL_MODULE.main()
            self.assertFalse(output.exists())
            self.assertEqual(self.tree_bytes(core), core_before)
            self.assertEqual(self.tree_bytes(stack), stack_before)

    def test_architectures_share_core_but_keep_distinct_dylib_bound_sboms(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            _, arm = self.generate_legal_fixture(root / "arm", "26.09.27", "https://example.org/repo")
            _, x64 = self.generate_legal_fixture(
                root / "x64", "26.09.27", "https://example.org/repo", "x86_64"
            )
            core_name = "ios-native-source-set-receipt.json"
            self.assertEqual((arm / core_name).read_bytes(), (x64 / core_name).read_bytes())
            # The versioned source offer is shared across architectures; only
            # legal qualification and the dylib-hash-bearing SBOM vary.
            overlay_name = "ios-native-overlay-receipt.json"
            self.assertEqual((arm / overlay_name).read_bytes(), (x64 / overlay_name).read_bytes())
            arm_sbom = required_json(arm / "ios-native-sbom.spdx.json")
            x64_sbom = required_json(x64 / "ios-native-sbom.spdx.json")
            self.assertNotEqual(arm_sbom["files"], x64_sbom["files"])
            for output, architecture in ((arm, "arm64"), (x64, "x86_64")):
                legal = required_json(output / "ios-native-legal-receipt.json")
                self.assertEqual(legal["architecture"], architecture)
                self.assertEqual(legal["sbom"]["sha256"], VERIFY_MODULE.sha256(output / "ios-native-sbom.spdx.json"))

    def test_overlay_and_qualification_fail_closed_on_wrong_core_and_tampering(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, output = self.generate_legal_fixture(root, "26.09.27", "https://example.org/repo")
            core = output / "ios-native-source-set-receipt.json"
            legal = output / "ios-native-legal-receipt.json"
            sbom = output / "ios-native-sbom.spdx.json"
            build = stack / "ios-native-build-receipt.json"
            overlay = output / "ios-native-overlay-receipt.json"

            def verify():
                return VERIFY_MODULE.verify_legal_assets(
                    LOCK, "arm64", core, legal, sbom, source_assets_root=output,
                    build_receipt_path=build,
                )

            hashes = verify()
            self.assertEqual(hashes["source_set_receipt_sha256"], VERIFY_MODULE.sha256(core))
            self.assertEqual(hashes["overlay_receipt_sha256"], VERIFY_MODULE.sha256(overlay))
            self.assertNotIn("legal_receipt_sha256", required_json(overlay))
            for file, mutate in (
                (overlay, lambda doc: doc.update(source_set_receipt_sha256="0" * 64)),
                (overlay, lambda doc: doc.update(schema_version=True)),
                (overlay, lambda doc: doc.update(base_url="https://example.org/other")),
                (overlay, lambda doc: doc.update(version=[])),
                (overlay, lambda doc: doc.update(assets=[])),
                (overlay, lambda doc: doc["assets"].append(doc["assets"][0])),
                (overlay, lambda doc: doc["assets"][0].update(file_name="../offer.txt")),
                (legal, lambda doc: doc.update(build_receipt_sha256="0" * 64)),
                (legal, lambda doc: doc.update(source_set_receipt_sha256="0" * 64)),
            ):
                with self.subTest(file=file.name, mutate=mutate):
                    original = file.read_bytes()
                    try:
                        document = json.loads(original)
                        mutate(document)
                        file.write_text(json.dumps(document), encoding="utf-8")
                        with self.assertRaises(VERIFY_MODULE.VerificationError):
                            verify()
                    finally:
                        file.write_bytes(original)
            with mock.patch.object(VERIFY_MODULE, "sha256", side_effect=lambda path:
                "0" * 64 if path == build else LEGAL_MODULE.sha256(path)):
                with self.assertRaisesRegex(VERIFY_MODULE.VerificationError, "build binding"):
                    verify()
            renamed = overlay.with_suffix(".missing")
            overlay.rename(renamed)
            try:
                with self.assertRaises(VERIFY_MODULE.VerificationError):
                    verify()
            finally:
                renamed.rename(overlay)
            offer = output / "ios-native-source-offer.txt"
            offer.write_text(offer.read_text().replace("26.09.27", "26.09.29"), encoding="utf-8")
            with self.assertRaises(VERIFY_MODULE.VerificationError):
                verify()

    def test_package_receipt_binds_binary_core_qualification_overlay_and_final_closure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            stack, generated = self.generate_legal_fixture(root, "26.09.27", "https://example.org/repo")
            output = root / "package-support"
            shutil.copytree(generated, output, ignore=shutil.ignore_patterns(
                "ios-native-corresponding-source.tar.gz"
            ))
            build = stack / "ios-native-build-receipt.json"
            source = output / "ios-native-source-set-receipt.json"
            legal = output / "ios-native-legal-receipt.json"
            sbom = output / "ios-native-sbom.spdx.json"
            overlay = output / "ios-native-overlay-receipt.json"
            package = output / "ios-native-package-receipt.json"
            app = root / "klogg-app"
            app.write_bytes(b"final signed app")
            dylib = stack / "lib" / "libfixture.dylib"
            evidence = [{"name": dylib.name, "sha256": VERIFY_MODULE.sha256(dylib)}]
            argv = [str(VERIFY_SCRIPT), "--lock", str(LOCK), "--stack-root", str(stack),
                    "--architecture", "arm64", "--source-receipt", str(source),
                    "--legal-receipt", str(legal), "--sbom", str(sbom),
                    "--app-executable", str(app), "--asset-scope", "package"]
            with mock.patch.object(VERIFY_MODULE, "verify_stack", return_value=evidence), mock.patch.object(
                VERIFY_MODULE, "verify_application_rpath", return_value={"sha256": VERIFY_MODULE.sha256(app)}
            ):
                with mock.patch.object(sys, "argv", argv + ["--package-receipt", str(package)]):
                    self.assertEqual(VERIFY_MODULE.main(), 0)
                result = required_json(package)
                for key, path in (
                    ("build_receipt_sha256", build), ("source_set_receipt_sha256", source),
                    ("legal_receipt_sha256", legal), ("overlay_receipt_sha256", overlay),
                    ("sbom_sha256", sbom),
                ):
                    self.assertEqual(result[key], VERIFY_MODULE.sha256(path))
                with mock.patch.object(sys, "argv", argv + ["--verify-package-receipt", str(package),
                                                        "--unsigned-package-stage"]):
                    self.assertEqual(VERIFY_MODULE.main(), 0)
                result["overlay_receipt_sha256"] = "0" * 64
                package.write_text(json.dumps(result), encoding="utf-8")
                with mock.patch.object(sys, "argv", argv + ["--verify-package-receipt", str(package),
                                                        "--unsigned-package-stage"]):
                    self.assertNotEqual(VERIFY_MODULE.main(), 0)

    def test_legal_source_archive_rejects_traversal_member_names(self):
        code = """
import io
import tarfile
from build_ios_native_legal_assets import LegalAssetError, add_bytes
for name in ("../victim", "..\\\\victim", "/tmp/victim", "patches/../../victim"):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w") as archive:
        try:
            add_bytes(archive, name, b"data")
        except LegalAssetError:
            pass
        else:
            raise SystemExit("accepted " + name)
"""
        result = subprocess.run(
            [sys.executable, "-c", code],
            cwd=ROOT / "scripts",
            capture_output=True,
            text=True,
            timeout=10,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_lock_schema_is_extended_for_release_and_artifact_evidence(self):
        self.assertEqual(type(self.lock().get("schema_version")), int)
        self.assertEqual(self.lock().get("schema_version"), 2)

    def test_verifier_rejects_boolean_lock_build_and_package_schema_versions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            lock_path = root / "lock.json"
            receipt_path = root / "build.json"
            lock_path.write_text('{"schema_version": true}\n', encoding="utf-8")
            receipt_path.write_text("{}\n", encoding="utf-8")
            argv = [
                str(VERIFY_SCRIPT),
                "--lock",
                str(lock_path),
                "--stack-root",
                str(root),
                "--architecture",
                "arm64",
                "--receipt",
                str(receipt_path),
            ]
            with contextlib.ExitStack() as patches:
                patches.enter_context(mock.patch.object(sys, "argv", argv))
                patches.enter_context(
                    mock.patch.object(VERIFY_MODULE, "validate_build_receipt")
                )
                patches.enter_context(
                    mock.patch.object(VERIFY_MODULE, "verify_stack", return_value=[])
                )
                self.assertNotEqual(VERIFY_MODULE.main(), 0)

        valid_build = {
            "schema_version": 1,
            "receipt_kind": "ios-native-build",
            "lock_sha256": VERIFY_MODULE.sha256(LOCK),
            "architecture": "arm64",
            "deployment_target": "14.0",
            "native_qualified": True,
            "qualification": "native",
        }
        invalid_build = dict(valid_build, schema_version=True)
        with self.assertRaisesRegex(VERIFY_MODULE.VerificationError, "schema"):
            VERIFY_MODULE.validate_build_receipt(
                self.lock(), LOCK, invalid_build, "arm64"
            )

        invalid_package = {
            "schema_version": True,
            "receipt_kind": "ios-native-package",
            "lock_sha256": "a" * 64,
            "architecture": "arm64",
            "build_receipt_sha256": "b" * 64,
            "dylibs": [],
        }
        with self.assertRaisesRegex(VERIFY_MODULE.VerificationError, "schema"):
            VERIFY_MODULE.verify_package_receipt(
                invalid_package,
                "a" * 64,
                "arm64",
                "b" * 64,
                [],
                {},
            )

    def test_verifier_rejects_boolean_source_and_legal_receipt_schema_versions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            lock_path = root / "lock.json"
            lock_path.write_text('{"schema_version": 2}\n', encoding="utf-8")
            lock_hash = VERIFY_MODULE.sha256(lock_path)

            def write_asset(name: str, content: bytes) -> pathlib.Path:
                path = root / name
                path.write_bytes(content)
                return path

            source_offer = write_asset("source-offer.txt", b"source offer\n")
            replacement = write_asset("replacement.txt", b"replacement\n")
            notice = write_asset("NOTICE.txt", b"notice\n")
            license_file = write_asset("COPYING", b"license\n")
            sbom_path = root / "sbom.json"
            sbom_path.write_text('{"spdxVersion": "SPDX-2.3"}\n', encoding="utf-8")

            def bound(path: pathlib.Path) -> dict:
                return {"path": path.name, "sha256": VERIFY_MODULE.sha256(path)}

            source_document = {
                "schema_version": 1,
                "receipt_kind": "component-source-set",
                "component": "ios-native",
                "lock_sha256": lock_hash,
                "source_identity": {
                    "manifest_or_closure_sha256": "a" * 64,
                    "tree_hash_algorithm": "sha256",
                    "final_tree_sha256": "b" * 64,
                },
                "patch_chain_sha256": "c" * 64,
                "package_support_assets": [
                    {
                        "kind": "source-offer",
                        "file_name": source_offer.name,
                        "sha256": VERIFY_MODULE.sha256(source_offer),
                    },
                    {
                        "kind": "replacement-guide",
                        "file_name": replacement.name,
                        "sha256": VERIFY_MODULE.sha256(replacement),
                    },
                    {
                        "kind": "notices",
                        "file_name": notice.name,
                        "sha256": VERIFY_MODULE.sha256(notice),
                    },
                    {
                        "kind": "license:fixture",
                        "file_name": license_file.name,
                        "sha256": VERIFY_MODULE.sha256(license_file),
                    },
                ],
                "distribution": {
                    "package_required": False,
                    "release_required": True,
                },
                "archive": {"file_name": "source.tar.gz", "sha256": "d" * 64},
            }
            legal_document = {
                "schema_version": 1,
                "receipt_kind": "legal",
                "lock_sha256": lock_hash,
                "architecture": "arm64",
                "notice": bound(notice),
                "replacement_guide": bound(replacement),
                "license_files": [bound(license_file)],
                "sbom": bound(sbom_path),
            }
            source_path = root / "source.json"
            legal_path = root / "legal.json"

            for label in ("source", "legal"):
                with self.subTest(label=label):
                    source_document["schema_version"] = (
                        True if label == "source" else 1
                    )
                    legal_document["schema_version"] = (
                        True if label == "legal" else 1
                    )
                    source_path.write_text(
                        json.dumps(source_document), encoding="utf-8"
                    )
                    legal_path.write_text(
                        json.dumps(legal_document), encoding="utf-8"
                    )
                    with self.assertRaisesRegex(
                        VERIFY_MODULE.VerificationError, "schema"
                    ):
                        VERIFY_MODULE.verify_legal_assets(
                            lock_path,
                            "arm64",
                            source_path,
                            legal_path,
                            sbom_path,
                            asset_scope="package",
                            source_assets_root=root,
                        )

            source_document["schema_version"] = 1
            legal_document["schema_version"] = 1
            source_document["distribution"] = {
                "package_required": 0,
                "release_required": 1,
            }
            source_path.write_text(json.dumps(source_document), encoding="utf-8")
            legal_path.write_text(json.dumps(legal_document), encoding="utf-8")
            with self.assertRaisesRegex(VERIFY_MODULE.VerificationError, "distribution"):
                VERIFY_MODULE.verify_legal_assets(
                    lock_path,
                    "arm64",
                    source_path,
                    legal_path,
                    sbom_path,
                    asset_scope="package",
                    source_assets_root=root,
                )
            source_document["distribution"] = {
                "package_required": False, "release_required": True,
            }
            source_path.write_text(json.dumps(source_document), encoding="utf-8")
            self.assertEqual(
                VERIFY_MODULE.verify_legal_assets(
                    lock_path, "arm64", source_path, legal_path, sbom_path,
                    asset_scope="package", source_assets_root=root,
                )["source_set_receipt_sha256"],
                VERIFY_MODULE.sha256(source_path),
                "existing packaged iOS legal layout must remain verifiable",
            )

    def test_complete_native_stack_has_exact_versions_and_immutable_sources(self):
        sources = self.sources()
        for dependency, expected in EXPECTED_SOURCES.items():
            source = sources[dependency]
            self.assertEqual(source.get("version"), expected["version"], dependency)
            self.assertEqual(source.get("commit"), expected["commit"], dependency)
            if "archive_sha256" in expected:
                self.assertEqual(
                    source.get("archive_sha256"), expected["archive_sha256"], dependency
                )
            self.assertRegex(str(source.get("commit", "")), HEX40, dependency)
            self.assertRegex(str(source.get("archive_sha256", "")), HEX64, dependency)
            self.assertTrue(str(source.get("archive_url", "")).startswith("https://"))
            if dependency == "openssl":
                self.assertEqual(source.get("archive_url"), expected["archive_url"])
                self.assertEqual(source.get("release_tag"), "openssl-3.5.7")
                self.assertIn(source["commit"], str(source.get("commit_url", "")))
                self.assertEqual(source.get("support_track"), "LTS")
            elif dependency == "curl":
                self.assertEqual(source.get("archive_url"), expected["archive_url"])
                self.assertEqual(source.get("release_tag"), "curl-8_21_0")
                self.assertIn(source["commit"], str(source.get("commit_url", "")))
            else:
                self.assertIn(
                    source["commit"],
                    source["archive_url"],
                    f"{dependency} archive URL must identify the immutable locked revision",
                )
            self.assertTrue(source.get("archive_file"), dependency)
            self.assertTrue(source.get("source_built"), dependency)
            self.assertFalse(source.get("allow_system_substitution", True), dependency)

        self.assertTrue(
            sources["openssl"].get("selection_rationale"),
            "the supported OpenSSL 3.5.7 LTS source selection must record its rationale",
        )

    def test_build_and_prefetch_manifests_cover_the_same_locked_stack(self):
        sources = self.sources()
        build_packages = cpm_packages(THIRD_PARTY_CMAKE)
        prefetch_packages = cpm_packages(PREFETCH_CMAKE)

        for dependency, source in sources.items():
            key = dependency.lower()
            self.assertIn(key, build_packages, f"build manifest omits {dependency}")
            self.assertIn(key, prefetch_packages, f"prefetch manifest omits {dependency}")
            for package_set, label in ((build_packages, "build"), (prefetch_packages, "prefetch")):
                tokens = package_set[key]
                self.assertIn("GIT_TAG", tokens, f"{label} {dependency} is not commit pinned")
                self.assertEqual(tokens[tokens.index("GIT_TAG") + 1], source["commit"])

        build_script = required_text(BUILD_SCRIPT)
        for dependency in EXPECTED_BUILD_ORDER:
            self.assertIn(dependency, build_script)
        self.assertIn("MACOSX_DEPLOYMENT_TARGET", build_script)
        self.assertIn("x86_64", build_script)
        self.assertIn("arm64", build_script)
        self.assertNotRegex(build_script, r"\bbrew\s+(install|link|--prefix)\b")
        self.assertNotRegex(build_script, r"\bpkg-config\b.*(?:/opt/homebrew|/usr/local)")

    def test_catalog_stays_non_handshaking_and_stream_preflights_pair_record(self):
        adapter = required_text(ROOT / "src/livecapture/src/iosnativeadapter.cpp")
        api = required_text(ROOT / "src/livecapture/include/iosnativeapi.h")
        catalog = required_text(ROOT / "src/livecapture/src/iosdevicecatalog.cpp")
        stream = required_text(ROOT / "src/livecapture/src/iosnativestream.cpp")

        for source in (adapter, api, catalog, stream):
            self.assertNotIn("lockdownd_client_new_with_handshake", source)
            self.assertNotIn("lockdownClientNewWithHandshake", source)
        self.assertIn('"lockdownd_client_new"', adapter)
        self.assertIn('"lockdownd_client_new_with_existing_pair"', adapter)
        self.assertIn("lockdownClientNew", api)

        self.assertIn('"usbmuxd_read_pair_record"', adapter)
        self.assertIn("readPairRecord", api)
        self.assertIn("lockdownClientNewWithExistingPair", api)
        self.assertLess(
            stream.index("api.readPairRecord("),
            stream.index("api.lockdownClientNewWithExistingPair("),
            "the worker must preflight before the adapter revalidates and starts a passive session",
        )
        self.assertNotIn("api.pairDevice(", stream)
        self.assertNotIn("api.unpairDevice(", stream)

    def test_release_policy_forbids_runtime_host_and_apple_private_fallbacks(self):
        policy = self.lock().get("release_policy")
        self.assertIsInstance(policy, dict)
        required_false = (
            "allow_runtime_download",
            "allow_homebrew_runtime",
            "allow_system_dependency_substitution",
            "allow_usbmuxd_daemon_dependency",
            "allow_mobiledevice_framework",
            "allow_floating_revisions",
        )
        for field in required_false:
            self.assertIs(policy.get(field), False, field)
        self.assertIs(policy.get("source_build_required"), True)
        self.assertIs(policy.get("application_configure_disconnected"), True)
        self.assertIs(policy.get("fail_closed_on_missing_native_stack"), True)
        self.assertEqual(policy.get("required_architectures"), ["x86_64", "arm64"])

        combined = "\n".join(
            (
                required_text(THIRD_PARTY_CMAKE),
                required_text(BUILD_SCRIPT),
                required_text(MAC_PACKAGE_ACTION),
            )
        )
        self.assertNotIn("MobileDevice.framework", combined)
        self.assertNotRegex(combined, r"\b(?:brew|port)\s+install\b")
        self.assertNotRegex(combined, r"(?:launchctl|usbmuxd\s+(?:--daemon|-f|-v))")

    def test_leak_patch_is_mandatory_and_bound_to_source_clean_and_patched_trees(self):
        document = self.lock()
        patches = document.get("patches")
        self.assertIsInstance(patches, list)
        matching = [
            item
            for item in patches
            if item.get("upstream_commit") == UPSTREAM_LEAK_FIX_COMMIT
        ]
        self.assertEqual(len(matching), 1, "the 5ca453f9 leak fix must be mandatory")
        patch = matching[0]
        self.assertIs(patch.get("required"), True)
        self.assertEqual(patch.get("source_id"), "libimobiledevice")
        self.assertEqual(
            patch.get("path"), "patches/0001-fix-ostrace-live-packet-leak.patch"
        )
        for field in ("source_archive_sha256", "clean_tree_sha256", "sha256", "patched_tree_sha256"):
            self.assertRegex(str(patch.get(field, "")), HEX64, field)
        self.assertEqual(
            patch.get("source_archive_sha256"),
            self.sources()["libimobiledevice"]["archive_sha256"],
        )
        self.assertEqual(hashlib.sha256(PATCH.read_bytes()).hexdigest(), patch["sha256"])
        self.assertNotEqual(patch["clean_tree_sha256"], patch["patched_tree_sha256"])

        series = [item for item in patches if item.get("source_id") == "libimobiledevice"]
        self.assertGreaterEqual(len(series), 2)
        for previous, current in zip(series, series[1:]):
            self.assertEqual(
                previous["patched_tree_sha256"],
                current["clean_tree_sha256"],
                "each native patch must be based on the exact preceding intermediate tree",
            )
        final_patch = series[-1]

        build = required_text(THIRD_PARTY_CMAKE)
        build_verification = build + "\n" + required_text(BUILD_SCRIPT)
        for item in series:
            self.assertIn(pathlib.Path(item["path"]).name, build_verification)
        self.assertIn(
            patch["clean_tree_sha256"],
            build_verification,
            "build must verify the locked original clean tree",
        )
        self.assertIn(
            final_patch["patched_tree_sha256"],
            build_verification,
            "build must verify the final tree produced by the complete patch series",
        )

    def test_release_source_receipts_require_the_complete_ordered_libimobiledevice_patch_chain(self):
        patches = self.lock().get("patches")
        self.assertIsInstance(patches, list)
        actual = [
            patch.get("path")
            for patch in patches
            if isinstance(patch, dict) and patch.get("source_id") == "libimobiledevice"
        ]
        self.assertEqual(
            actual,
            EXPECTED_LIBIMOBILEDEVICE_PATCH_CHAIN,
            "release/source receipts must bind the typed-record ABI patch after the existing runtime series",
        )
        self.assertEqual(
            patches[-1].get("patched_tree_sha256"),
            EXPECTED_LIBIMOBILEDEVICE_FINAL_TREE_SHA256,
            "the lock must name the tree produced by applying the complete patch chain to the pinned archive",
        )

        build_inputs = "\n".join(
            (required_text(THIRD_PARTY_CMAKE), required_text(BUILD_SCRIPT))
        )
        for patch_path in EXPECTED_LIBIMOBILEDEVICE_PATCH_CHAIN:
            self.assertIn(pathlib.Path(patch_path).name, build_inputs)

    def test_dynamic_ostrace_error_callback_restores_the_application_context(self):
        adapter = required_text(ROOT / "src/livecapture/src/iosnativeadapter.cpp")
        self.assertIn("void relayOsTraceError(", adapter)
        self.assertIn(
            "relay->errorCallback( error, relay->context )",
            adapter,
        )
        self.assertRegex(
            adapter,
            r"symbols\.osTraceStartWithRecordType\(\s*client, nullptr, "
            r"&relayOsTraceRecord, &relayOsTraceError, relayContext\s*\)",
        )
        self.assertNotRegex(
            adapter,
            r"symbols\.osTraceStartWithRecordType\(\s*client, nullptr, "
            r"&relayOsTraceRecord, errorCallback, relayContext\s*\)",
        )

    def test_pinned_typed_ostrace_symbol_is_required_by_the_artifact_verifier(self):
        symbol = "ostrace_start_activity_with_record_type_and_error"
        contract = self.lock().get("artifact_contract")
        self.assertIsInstance(contract, dict)
        required = contract.get("required_exported_symbols")
        self.assertIsInstance(required, dict)
        self.assertEqual(
            required.get("libimobiledevice-1.0.dylib"),
            [
                "lockdownd_client_new_with_existing_pair",
                symbol,
                "syslog_relay_start_capture_raw_with_error",
            ],
        )

        verifier = required_text(VERIFY_SCRIPT)
        for token in ("nm", "required_exported_symbols", "missing required exported symbols"):
            self.assertIn(token, verifier)

    def test_exported_symbol_verification_normalizes_macho_names_and_fails_closed(self):
        dylib = pathlib.Path("libimobiledevice-1.0.dylib")
        with mock.patch.object(
            VERIFY_MODULE,
            "run",
            return_value=(
                "0000000000001000 T _ostrace_start_activity_with_record_type_and_error\n"
                "0000000000002000 T _lockdownd_client_new_with_existing_pair\n"
            ),
        ):
            self.assertEqual(
                VERIFY_MODULE.exported_symbols(dylib),
                {
                    "ostrace_start_activity_with_record_type_and_error",
                    "lockdownd_client_new_with_existing_pair",
                },
            )
            VERIFY_MODULE.verify_required_exported_symbols(
                dylib, ["ostrace_start_activity_with_record_type_and_error"]
            )
            with self.assertRaisesRegex(
                VERIFY_MODULE.VerificationError, "missing required exported symbols"
            ):
                VERIFY_MODULE.verify_required_exported_symbols(
                    dylib, ["syslog_relay_start_capture_raw_with_error"]
                )

    def test_exported_symbol_lock_contract_rejects_duplicates_and_unknown_dylibs(self):
        with self.assertRaises(VERIFY_MODULE.VerificationError):
            VERIFY_MODULE.locked_required_exported_symbols(
                {"required_exported_symbols": {"other.dylib": ["symbol"]}},
                ["libimobiledevice-1.0.dylib"],
            )
        with self.assertRaises(VERIFY_MODULE.VerificationError):
            VERIFY_MODULE.locked_required_exported_symbols(
                {
                    "required_exported_symbols": {
                        "libimobiledevice-1.0.dylib": ["symbol", "symbol"]
                    }
                },
                ["libimobiledevice-1.0.dylib"],
            )

    def test_builder_alias_layout_is_verified_and_alias_symbols_bind_to_the_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            prefix = root / "prefix"
            (prefix / "include").mkdir(parents=True)
            prefix_lib = prefix / "lib"
            prefix_lib.mkdir()
            physical = prefix_lib / "librequired.1.dylib"
            physical.write_bytes(b"required dylib fixture")
            try:
                (prefix_lib / "librequired.dylib").symlink_to(physical.name)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"filesystem symlinks are unavailable: {error}")

            stack_root = root / "artifact"
            identities = BUILD_MODULE.stage_artifact(prefix, stack_root, [physical])
            staged_physical = stack_root / "lib" / physical.name
            staged_alias = stack_root / "lib" / "librequired.dylib"
            self.assertTrue(staged_alias.is_symlink())
            self.assertEqual(staged_alias.resolve(), staged_physical.resolve())

            contract = {
                "thin_artifacts": {"arm64": {"deployment_target": "14.0"}},
                "allowed_dylib_rpaths": ["@loader_path"],
                "allowed_system_dependencies": ["/usr/lib/libSystem.B.dylib"],
                "required_dylibs": [staged_alias.name],
                "required_exported_symbols": {
                    staged_alias.name: ["required_symbol"]
                },
                "forbidden_dynamic_references": [],
            }
            with self.patched_stack_inspection() as exported_symbols:
                evidence = VERIFY_MODULE.verify_stack(
                    {"artifact_contract": contract},
                    stack_root,
                    "arm64",
                    {"dylibs": identities},
                )

            self.assertEqual([item["name"] for item in evidence], [physical.name])
            exported_symbols.assert_called_once_with(staged_physical)

    def test_verifier_rejects_regular_dylibs_outside_the_locked_physical_closure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            prefix = root / "prefix"
            (prefix / "include").mkdir(parents=True)
            prefix_lib = prefix / "lib"
            prefix_lib.mkdir()
            required = prefix_lib / "librequired.1.dylib"
            extra = prefix_lib / "libextra.1.dylib"
            required.write_bytes(b"required dylib fixture")
            extra.write_bytes(b"extra dylib fixture")
            try:
                (prefix_lib / "librequired.dylib").symlink_to(required.name)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"filesystem symlinks are unavailable: {error}")

            stack_root = root / "artifact"
            identities = BUILD_MODULE.stage_artifact(
                prefix, stack_root, [required, extra]
            )
            contract = {
                "thin_artifacts": {"arm64": {"deployment_target": "14.0"}},
                "allowed_dylib_rpaths": ["@loader_path"],
                "allowed_system_dependencies": ["/usr/lib/libSystem.B.dylib"],
                "required_dylibs": ["librequired.dylib"],
                "required_exported_symbols": {
                    "librequired.dylib": ["required_symbol"]
                },
                "forbidden_dynamic_references": [],
            }
            with self.patched_stack_inspection():
                with self.assertRaisesRegex(
                    VERIFY_MODULE.VerificationError, r"unrequired|physical closure"
                ):
                    VERIFY_MODULE.verify_stack(
                        {"artifact_contract": contract},
                        stack_root,
                        "arm64",
                        {"dylibs": identities},
                    )

    def test_verifier_rejects_dangling_chained_and_escaping_dylib_aliases(self):
        for scenario in ("dangling", "chain", "escape"):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                libdir = root / "lib"
                libdir.mkdir()
                physical = libdir / "librequired.1.dylib"
                physical.write_bytes(b"required dylib fixture")
                alias = libdir / "librequired.dylib"
                outside = root / "outside.dylib"
                outside.write_bytes(b"outside dylib fixture")
                try:
                    if scenario == "dangling":
                        alias.symlink_to("missing.dylib")
                    elif scenario == "chain":
                        intermediate = libdir / "libintermediate.dylib"
                        intermediate.symlink_to(physical.name)
                        alias.symlink_to(intermediate.name)
                    else:
                        alias.symlink_to(pathlib.Path("..") / outside.name)
                except (NotImplementedError, OSError) as error:
                    self.skipTest(f"filesystem symlinks are unavailable: {error}")

                contract = {
                    "thin_artifacts": {"arm64": {"deployment_target": "14.0"}},
                    "allowed_dylib_rpaths": ["@loader_path"],
                    "allowed_system_dependencies": ["/usr/lib/libSystem.B.dylib"],
                    "required_dylibs": [alias.name],
                    "required_exported_symbols": {alias.name: ["required_symbol"]},
                    "forbidden_dynamic_references": [],
                }
                receipt = {
                    "dylibs": [
                        {"name": physical.name, "sha256": VERIFY_MODULE.sha256(physical)}
                    ]
                }
                with self.assertRaisesRegex(
                    VERIFY_MODULE.VerificationError,
                    r"alias|chain|dangling|escape|regular target",
                ):
                    VERIFY_MODULE.verify_stack(
                        {"artifact_contract": contract}, root, "arm64", receipt
                    )

    def test_verifier_rejects_aliases_redirected_to_a_different_required_library(self):
        with tempfile.TemporaryDirectory() as temporary:
            libdir = pathlib.Path(temporary) / "lib"
            libdir.mkdir()
            other_library = libdir / "libother.1.dylib"
            other_library.write_bytes(b"other required dylib fixture")
            alias = libdir / "librequired.dylib"
            try:
                alias.symlink_to(other_library.name)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"filesystem symlinks are unavailable: {error}")

            with self.assertRaisesRegex(
                VERIFY_MODULE.VerificationError, r"alias.*name|library identity"
            ):
                VERIFY_MODULE.direct_dylib_target(alias, libdir, alias.name)

    def test_verifier_enforces_macho_closure_install_names_architectures_and_targets(self):
        verifier = required_text(VERIFY_SCRIPT)
        for token in (
            "otool",
            "-L",
            "-D",
            "@rpath",
            "install_name",
            "lipo",
            "x86_64",
            "arm64",
            "vtool",
            "deployment_target",
            "LC_BUILD_VERSION",
            "sha256",
        ):
            self.assertIn(token, verifier)
        for forbidden in (
            "/opt/homebrew",
            "/usr/local/opt",
            "MobileDevice.framework",
            "/System/Library/PrivateFrameworks",
        ):
            self.assertIn(
                forbidden,
                verifier,
                f"verifier must reject dynamic closure reference {forbidden}",
            )
        self.assertRegex(verifier, r"(?:usbmuxd|daemon)")
        self.assertRegex(verifier, r"(?:missing|not found).*(?:fail|error|raise)|raise.*missing")

        artifact_contract = self.lock().get("artifact_contract")
        self.assertIsInstance(artifact_contract, dict)
        self.assertEqual(artifact_contract.get("dylib_install_name_prefix"), "@rpath/")
        self.assertEqual(
            artifact_contract.get("required_architectures"), ["x86_64", "arm64"]
        )
        thin = artifact_contract.get("thin_artifacts")
        self.assertEqual(sorted(thin), sorted(EXPECTED_THIN_ARTIFACTS))
        for architecture, expected_target in EXPECTED_THIN_ARTIFACTS.items():
            receipt = thin[architecture]
            self.assertEqual(receipt.get("architecture"), architecture)
            self.assertEqual(receipt.get("deployment_target"), expected_target)
            self.assertTrue(receipt.get("receipt_file"))

    def test_legal_sbom_source_offer_and_receipts_bind_every_shipped_dylib(self):
        sources = self.sources()
        for dependency, source in sources.items():
            legal = source.get("legal")
            self.assertIsInstance(legal, dict, dependency)
            self.assertTrue(legal.get("spdx_license"), dependency)
            self.assertTrue(legal.get("license_files"), dependency)
            self.assertTrue(legal.get("notice_label"), dependency)
            self.assertTrue(legal.get("source_offer_label"), dependency)

        legal_script = required_text(LEGAL_SCRIPT)
        for token in (
            "SPDX",
            "source",
            "license",
            "notice",
            "sbom",
            "receipt",
            "sha256",
            "package",
        ):
            self.assertIn(token.lower(), legal_script.lower())
        for dependency in sources:
            self.assertIn(dependency, legal_script)

        receipts = self.lock().get("receipts")
        self.assertIsInstance(receipts, dict)
        self.assertEqual(
            sorted(receipts),
            sorted(("build", "source", "legal", "sbom", "package")),
        )
        for kind, path in receipts.items():
            self.assertTrue(path, kind)

        package_action = required_text(MAC_PACKAGE_ACTION)
        for path in receipts.values():
            self.assertIn(path, package_action)
        self.assertIn(VERIFY_SCRIPT.relative_to(ROOT).as_posix(), package_action)
        self.assertIn(LOCK.relative_to(ROOT).as_posix(), package_action)

    def test_source_set_receipt_is_architecture_independent_and_hash_bound(self):
        legal = required_text(LEGAL_SCRIPT)
        verifier = required_text(VERIFY_SCRIPT)
        build = required_text(BUILD_SCRIPT)
        receipt_name = "ios-native-source-set-receipt.json"

        self.assertIn(receipt_name, legal)
        self.assertIn('"receipt_kind": "component-source-set"', legal)
        self.assertIn('"component": "ios-native"', legal)
        source_set_start = legal.index('"receipt_kind": "component-source-set"')
        source_set_end = legal.find("write_json", source_set_start)
        self.assertGreater(source_set_end, source_set_start)
        source_set_block = legal[source_set_start:source_set_end]
        self.assertNotIn("architecture", source_set_block)
        self.assertNotIn("deployment_target", source_set_block)
        for token in (
            "lock_sha256",
            "archive",
            "source_identity",
            "tree_hash_algorithm",
            "final_tree_sha256",
            "package_support_assets",
            "package_required",
            "release_required",
        ):
            self.assertIn(token, source_set_block)

        self.assertIn("source_set_receipt_sha256", legal + "\n" + build)
        package_start = verifier.index("package = {")
        package_end = verifier.index("args.package_receipt.write_text", package_start)
        self.assertIn(
            "source_set_receipt_sha256",
            verifier[package_start:package_end],
            "iOS package receipts must bind the architecture-independent source set",
        )

    def test_package_and_release_scopes_resolve_ios_source_archive_separately(self):
        verifier = required_text(VERIFY_SCRIPT)
        self.assertNotRegex(
            verifier,
            re.compile(
                r"bound_asset\(source_receipt_path\.parent,\s*"
                r"source\.get\(\"source_offer\"\),\s*\"corresponding source\"\)"
            ),
            "package verification must not unconditionally resolve the release-only source archive beside the package receipt",
        )
        for token in (
            "--asset-scope",
            "--source-assets-root",
            "package",
            "release",
            "must not contain the corresponding source archive",
        ):
            self.assertIn(token, verifier)

    def test_release_scope_requires_external_source_assets_without_package_receipt_operation(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = pathlib.Path(tempdir)
            lock = root / "lock.json"
            receipt = root / "ios-native-build-receipt.json"
            lock.write_text("{}\n", encoding="utf-8")
            receipt.write_text("{}\n", encoding="utf-8")
            argv = [
                str(VERIFY_SCRIPT),
                "--lock",
                str(lock),
                "--stack-root",
                str(root),
                "--architecture",
                "arm64",
                "--receipt",
                str(receipt),
                "--asset-scope",
                "release",
                "--source-assets-root",
                str(root / "external-sources"),
                "--source-receipt",
                str(root / "source-set.json"),
                "--legal-receipt",
                str(root / "legal.json"),
                "--sbom",
                str(root / "sbom.json"),
            ]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                VERIFY_MODULE, "validate_build_receipt"
            ), mock.patch.object(
                VERIFY_MODULE, "verify_stack", return_value=[]
            ), mock.patch.object(
                VERIFY_MODULE,
                "verify_legal_assets",
                side_effect=VERIFY_MODULE.VerificationError(
                    "missing corresponding source archive"
                ),
            ):
                self.assertNotEqual(
                    VERIFY_MODULE.main(),
                    0,
                    "release scope must verify its external archive even without package receipt creation/verification",
                )

    def test_native_stack_smoke_receives_qt_path_without_packaging_rpath(self):
        app_cmake = required_text(APP_CMAKE)
        native_rpath_block = app_cmake.split("if(KLOGG_ENABLE_IOS_NATIVE_STACK)", 1)[
            1
        ].split("endif()", 1)[0]
        self.assertIn("SKIP_BUILD_RPATH ON", native_rpath_block)
        self.assertIn(
            'KLOGG_QT_BUILD_RPATH "${${KLOGG_QT_PACKAGE_DIR_VARIABLE}}/../.." ABSOLUTE',
            native_rpath_block,
        )
        self.assertIn(
            'KLOGG_QT_BUILD_RPATH "${KLOGG_QT_BUILD_RPATH}"', native_rpath_block
        )
        self.assertIn(
            '"LINKER:-rpath,@executable_path/../Frameworks"', native_rpath_block
        )
        self.assertIn(
            '"LINKER:-rpath,@executable_path/../Frameworks/ios-native/lib"',
            native_rpath_block,
        )
        root_cmake = required_text(ROOT_CMAKE)
        self.assertIn("DYLD_FRAMEWORK_PATH=${KLOGG_SMOKE_QT_RPATH}", root_cmake)
        self.assertIn("DYLD_LIBRARY_PATH=${KLOGG_SMOKE_QT_RPATH}", root_cmake)

    def test_enabled_raw_apps_stage_the_private_ios_runtime_closure(self):
        app_cmake = required_text(APP_CMAKE)
        native_block = app_cmake.split("if(KLOGG_ENABLE_IOS_NATIVE_STACK)", 1)[
            1
        ].split("\n  endif()", 1)[0]
        third_party_cmake = required_text(THIRD_PARTY_CMAKE)
        self.assertIn("KLOGG_IOS_NATIVE_RUNTIME_DYLIBS", third_party_cmake)
        for direct_alias in (
            "libcrypto.dylib",
            "libssl.dylib",
            "libcurl.dylib",
            "libtatsu.dylib",
        ):
            self.assertIn(direct_alias, third_party_cmake)
        self.assertIn("get_target_property(", native_block)
        self.assertIn("native_dylibs klogg_ios_native_stack", native_block)
        self.assertIn("KLOGG_IOS_NATIVE_RUNTIME_DYLIBS", native_block)
        self.assertNotIn("file(GLOB native_dylibs", native_block)
        self.assertIn("function(klogg_stage_ios_native_stack", native_block)
        self.assertIn("${CMAKE_RUNTIME_OUTPUT_DIRECTORY}", native_block)
        self.assertIn("GENERATOR_IS_MULTI_CONFIG", native_block)
        self.assertIn("$<CONFIG>", native_block)
        self.assertIn("Frameworks/ios-native/lib", native_block)
        self.assertIn("${KLOGG_IOS_NATIVE_STACK_ROOT}/lib", third_party_cmake)
        self.assertIn("add_custom_target", native_block)
        self.assertIn("COMMAND /bin/cp -a", native_block)
        self.assertIn("COMMAND_EXPAND_LISTS", native_block)
        self.assertIn("add_dependencies(${target}", native_block)
        self.assertNotIn("POST_BUILD", native_block)
        self.assertNotIn("copy_directory", native_block)
        self.assertIn("klogg_stage_ios_native_stack(klogg)", native_block)
        self.assertIn("klogg_stage_ios_native_stack(klogg_portable)", native_block)
        self.assertNotIn(
            'copy_directory "${KLOGG_IOS_NATIVE_STACK_ROOT}/lib" '
            '"$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks"',
            native_block,
        )

    def test_ios_build_receipt_records_actual_ninja_version_for_core_qualification(self):
        builder = required_text(ROOT / "scripts/build_ios_native_stack.py")
        catalog = json.loads(required_text(ROOT / "ci/dependencies/catalog.json"))
        for architecture in ("x86_64", "arm64"):
            self.assertEqual(catalog["targets"]["ios-" + architecture]["toolchain"]["ninja"],
                             "1.12.1")
        self.assertIn('"ninja": run(["ninja", "--version"]).strip()', builder)

    def test_ios_native_jobs_check_selected_xcode_before_source_build(self):
        workflow = required_text(CI_BUILD_WORKFLOW)
        jobs = CI_MODULE.workflow_job_steps(workflow)
        steps = [CI_MODULE.workflow_step_fields(step)[0]
                 for step in jobs["BuildIosNativeX64"]]
        names = [step.get("name", "") for step in steps]
        selection = "Verify pinned iOS producer toolchain"
        self.assertEqual(names.count(selection), 1)
        self.assertLess(names.index("Select verified Xcode 26.6"), names.index("Install iOS native source-build tools"))
        self.assertLess(names.index("Install pinned CMake and Ninja"), names.index(selection))
        self.assertLess(names.index(selection),
                        names.index("Build disconnected thin iOS native stack"))
        preflight = steps[names.index(selection)]
        self.assertEqual(preflight.get("shell"), "bash")
        body = CI_MODULE.active_script_content(preflight.get("run", ""))
        self.assertNotIn("Xcode_16.4.app", body)
        self.assertIn("scripts/ci_dependency_toolchain.py", body)
        self.assertIn("ios-${KLOGG_IOS_ARCHITECTURE}", body)
        select = CI_MODULE.active_script_content(steps[names.index("Select verified Xcode 26.6")].get("run", ""))
        self.assertIn("/Applications/Xcode_26.6.app/Contents/Developer", select)
        self.assertIn("17F113", select)
        self.assertIn("$GITHUB_ENV", select)
        arm = CI_MODULE.workflow_job_blocks(workflow)["BuildIosNativeArm64"]
        self.assertTrue(any("steps: *ios_native_steps" in line for line in arm))

    def test_ios_native_homebrew_bootstrap_cleans_aws_formula_and_tap_before_install(self):
        workflow = required_text(CI_BUILD_WORKFLOW)
        jobs = CI_MODULE.workflow_job_steps(workflow)
        self.assertIn("BuildIosNativeX64", jobs)
        install_steps = [
            fields
            for step in jobs["BuildIosNativeX64"]
            for fields, _ in [CI_MODULE.workflow_step_fields(step)]
            if fields.get("name") == "Install iOS native source-build tools"
        ]
        self.assertEqual(len(install_steps), 1)
        install_step = CI_MODULE.active_script_content(
            install_steps[0].get("run", "")
        )
        uninstall = "brew uninstall --ignore-dependencies aws-sam-cli"
        untap = "brew untap aws/tap"
        install = "brew install autoconf automake libtool pkg-config"
        failures = []
        tool_actions = [
            children.get("with", {})
            for step in jobs["BuildIosNativeX64"]
            for fields, children in [CI_MODULE.workflow_step_fields(step)]
            if fields.get("uses") == "lukka/get-cmake@fffaaafeea488556c2c12dad60690008bc1caacb"
        ]
        self.assertEqual(
            tool_actions, [{"cmakeVersion": "3.31.6", "ninjaVersion": "1.12.1"}]
        )
        self.assertNotIn("brew install autoconf automake libtool pkg-config cmake ninja", install_step)
        for marker in (
            "HOMEBREW_NO_AUTO_UPDATE=1",
            "HOMEBREW_NO_INSTALL_CLEANUP=1",
            uninstall,
            untap,
            install,
        ):
            if marker not in install_step:
                failures.append(f"missing {marker}")
        if not failures:
            if not (
                install_step.index(uninstall)
                < install_step.index(untap)
                < install_step.index(install)
            ):
                failures.append(
                    "aws-sam-cli must be uninstalled before aws/tap is untapped "
                    "and before project build tools are installed"
                )
            cleanup = install_step[
                install_step.index(uninstall) : install_step.index(install)
            ]
            if "|| true" not in cleanup and "brew list --formula" not in cleanup:
                failures.append(
                    "shared AWS Homebrew cleanup must tolerate an absent formula and tap"
                )
        self.assertEqual(failures, [], "\n".join(failures))

    def test_ci_produces_both_thin_stacks_and_mac_consumes_the_bound_artifact(self):
        workflow = required_text(CI_BUILD_WORKFLOW)
        blocks = CI_MODULE.workflow_job_blocks(workflow)
        self.assertTrue(
            {"BuildIosNativeX64", "BuildIosNativeArm64"} <= set(blocks),
            sorted(blocks),
        )
        needs = CI_MODULE.workflow_job_needs(workflow)
        self.assertIn("BuildIosNativeX64", needs.get("MacPackages", set()))
        self.assertIn("BuildIosNativeArm64", needs.get("MacArmPackages", set()))
        for token in (
            "prefetch_ios_native_sources.py",
            "build_ios_native_stack.py",
            "build_ios_native_legal_assets.py",
            "verify_ios_native_stack.py",
            "ios-native-x86_64",
            "ios-native-arm64",
            "ios-native-build-receipt.json",
            "ios-native-source-receipt.json",
            "ios-native-legal-receipt.json",
            "ios-native-sbom.spdx.json",
            "SHA256SUMS",
            "KLOGG_IOS_NATIVE_STACK_ROOT",
            "KLOGG_ENABLE_IOS_NATIVE_STACK=ON",
            "FETCHCONTENT_FULLY_DISCONNECTED=ON",
        ):
            self.assertIn(token, workflow)
        for job, runner, architecture, artifact in (
            (
                "BuildIosNativeX64",
                "macos-26-intel",
                "x86_64",
                "ios-native-x86_64",
            ),
            ("BuildIosNativeArm64", "macos-26", "arm64", "ios-native-arm64"),
        ):
            with self.subTest(job=job):
                job_block = blocks[job]
                block = CI_MODULE.active_script_content("\n".join(job_block))
                self.assertEqual(
                    CI_MODULE.workflow_job_direct_value(job_block, "runs-on"), runner
                )
                environment = CI_MODULE.workflow_mapping_block(job_block, "env", 4)
                self.assertIsNotNone(environment)
                assert environment is not None
                self.assertEqual(environment["KLOGG_IOS_ARCHITECTURE"][0], architecture)
                self.assertEqual(environment["KLOGG_IOS_ARTIFACT"][0], artifact)
                self.assertIn(artifact, block)
                self.assertNotRegex(
                    block, r"\$\{\{[^}]*\bmatrix\.artifact\b"
                )
                self.assertNotIn("matrix.architecture", block)
        artifact_records = CI_MODULE.workflow_artifact_records(workflow)
        for job, artifact in (
            ("MacPackages", "ios-native-x86_64"),
            ("MacArmPackages", "ios-native-arm64"),
        ):
            with self.subTest(consumer=job):
                block = CI_MODULE.active_script_content("\n".join(blocks[job]))
                self.assertIn(artifact, block)
                self.assertNotRegex(
                    block,
                    r"matrix\.config\.(?:ios_native_artifact|arch)\b",
                )
                downloads = {
                    (name, condition)
                    for kind, name, condition in artifact_records.get(job, [])
                    if kind == "downloads"
                }
                self.assertTrue(
                    (artifact, None) in downloads
                    or (
                        "${{ env.KLOGG_IOS_NATIVE_ARTIFACT }}",
                        "${{ env.klogg_package_enabled == 'true' }}",
                    )
                    in downloads,
                    downloads,
                )
        self.assertIn("tar -czf", workflow)
        self.assertIn("python3 scripts/extract_verified_tar.py", workflow)
        self.assertNotIn('tar -xzf "$archive"', workflow)
        self.assertIn("prefetch_artifacts/ios-native-archive", workflow)
        producer = "\n".join(blocks["BuildIosNativeX64"])
        for marker in (
            "id: upload_ios_stack",
            "continue-on-error: true",
            "steps.upload_ios_stack.outcome == 'failure'",
            "overwrite: true",
        ):
            self.assertIn(marker, producer)
        self.assertEqual(producer.count("continue-on-error: true"), 1)

    def test_commit_archives_receive_locked_autotools_tarball_versions(self):
        build = required_text(BUILD_SCRIPT)
        self.assertIn(".tarball-version", build)
        self.assertIn('record["version"]', build)
        self.assertLess(build.index("patched_tree_sha256"), build.index(".tarball-version"))

    def test_disconnected_superbuild_has_no_download_or_update_boundary(self):
        superbuild = required_text(SUPERBUILD)
        self.assertNotRegex(superbuild, r"https?://|\bGIT_REPOSITORY\b|\bURL\b")
        external_projects = cmake_calls(superbuild, "ExternalProject_Add")
        self.assertGreaterEqual(len(external_projects), 2)
        for project in external_projects:
            tokens = cmake_tokens(project)
            self.assertIn("DOWNLOAD_COMMAND", tokens)
            self.assertEqual(tokens[tokens.index("DOWNLOAD_COMMAND") + 1], "")
            self.assertIn("UPDATE_COMMAND", tokens)
            self.assertEqual(tokens[tokens.index("UPDATE_COMMAND") + 1], "")
            self.assertIn("PATCH_COMMAND", tokens)
            self.assertEqual(tokens[tokens.index("PATCH_COMMAND") + 1], "")

    def test_source_extraction_rejects_special_and_duplicate_members(self):
        def archive_with(members):
            temporary = tempfile.NamedTemporaryFile(suffix=".tar.gz", delete=False)
            temporary.close()
            path = pathlib.Path(temporary.name)
            with tarfile.open(path, "w:gz") as archive:
                root = tarfile.TarInfo("source")
                root.type = tarfile.DIRTYPE
                archive.addfile(root)
                for member in members:
                    archive.addfile(member, io.BytesIO(b"payload") if member.isfile() else None)
            self.addCleanup(path.unlink, missing_ok=True)
            return path

        fifo = tarfile.TarInfo("source/pipe")
        fifo.type = tarfile.FIFOTYPE
        duplicate_a = tarfile.TarInfo("source/repeated")
        duplicate_a.size = len(b"payload")
        duplicate_b = tarfile.TarInfo("source/repeated")
        duplicate_b.size = len(b"payload")

        with tempfile.TemporaryDirectory() as output:
            with self.assertRaises(BUILD_MODULE.BuildError):
                BUILD_MODULE.safe_extract(
                    archive_with([fifo]), pathlib.Path(output) / "special"
                )
            with self.assertRaises(BUILD_MODULE.BuildError):
                BUILD_MODULE.safe_extract(
                    archive_with([duplicate_a, duplicate_b]), pathlib.Path(output) / "duplicate"
                )

    def test_legal_assets_bind_build_receipt_sbom_and_lgpl_replacement_guide(self):
        legal = required_text(LEGAL_SCRIPT)
        for token in (
            "lock_sha256",
            "receipt_kind",
            "architecture",
            "deployment_target",
            "licenseInfoInFiles",
            "copyrightText",
            "relationships",
            "ios-native-lgpl-replacement.txt",
        ):
            self.assertIn(token, legal)
        action = required_text(MAC_PACKAGE_ACTION)
        self.assertIn("ios-native-lgpl-replacement.txt", action)
        self.assertIn("codesign --force --deep --sign -", legal)

    def test_external_ios_source_offer_is_actionable_and_archive_rebuild_paths_match(self):
        legal = required_text(LEGAL_SCRIPT)
        for token in (
            "--version",
            "--base-url",
            "not included in the installer",
            'f"{base_url}/releases"',
            "shasum -a 256",
            "validated_relative_path(",
            'patch.get("path"), "locked patch"',
            'f"3rdparty/libimobiledevice/{patch_path}"',
        ):
            self.assertIn(token, legal)
        self.assertIn('f"{base_url}/releases/tag/continuous"', legal)
        self.assertNotIn('source_asset_url(base_url, "continuous"', legal)
        self.assertIn("rolling and mutable", legal)
        self.assertNotIn(
            "The accompanying ios-native-corresponding-source.tar.gz",
            legal,
        )

    def test_macos_root_closure_check_targets_ios_specific_dylibs_only(self):
        action = required_text(MAC_PACKAGE_ACTION)
        start = action.index('if find "$app_frameworks"')
        end = action.index('case "${KLOGG_ARCH}"', start)
        check = action[start:end]
        for name in (
            "libimobiledevice",
            "libplist",
            "libusbmuxd",
            "libimobiledevice-glue",
            "libtatsu",
        ):
            self.assertIn(name, check)
        for qt_runtime_dependency in ("libssl", "libcrypto", "libcurl"):
            self.assertNotIn(qt_runtime_dependency, check)

    def test_staged_bundle_is_reverified_after_macdeployqt_before_signing(self):
        action = required_text(MAC_PACKAGE_ACTION)
        deploy = action.index("Mac deploy Qt")
        staged_verify = action.index("Verify staged iOS native bundle")
        signing = action.index("Sign and verify macOS application bundle")
        self.assertLess(deploy, staged_verify)
        self.assertLess(staged_verify, signing)
        staged_section = action[staged_verify:signing]
        self.assertIn("--app-executable", staged_section)
        self.assertIn("Contents/Frameworks/ios-native", staged_section)
        verifier = required_text(VERIFY_SCRIPT)
        for token in ("--app-executable", "LC_RPATH", "otool", "-l"):
            self.assertIn(token, verifier)

    def test_verifier_rejects_hostile_rpaths_unlocked_system_imports_and_unqualified_receipts(self):
        contract = self.lock().get("artifact_contract", {})
        self.assertEqual(contract.get("allowed_dylib_rpaths"), ["@loader_path"])
        allowed_system = contract.get("allowed_system_dependencies")
        self.assertIsInstance(allowed_system, list)
        self.assertIn("/usr/lib/libSystem.B.dylib", allowed_system)

        verifier = required_text(VERIFY_SCRIPT)
        for token in (
            "LC_RPATH",
            "allowed_dylib_rpaths",
            "allowed_system_dependencies",
            "native_qualified",
            "qualification",
            "receipt_kind",
        ):
            self.assertIn(token, verifier)

    def test_unsigned_validation_receipts_do_not_require_code_signatures(self):
        action = required_text(MAC_PACKAGE_ACTION)
        validation = action.split("- name: Validate unsigned macOS disk image", 1)[1].split(
            "- name: Sign and verify macOS disk image", 1
        )[0]
        self.assertIn("--unsigned-package-stage", validation)
        verifier = required_text(VERIFY_SCRIPT)
        self.assertIn("--unsigned-package-stage", verifier)
        self.assertIn(
            "args.verify_package_receipt is not None and not args.unsigned_package_stage",
            verifier,
        )

    def test_package_receipt_binds_legal_source_sbom_and_final_signed_closure(self):
        verifier = required_text(VERIFY_SCRIPT)
        for token in (
            "source_receipt",
            "legal_receipt",
            "sbom",
            "source_receipt_sha256",
            "legal_receipt_sha256",
            "sbom_sha256",
        ):
            self.assertIn(token, verifier)

        action = required_text(MAC_PACKAGE_ACTION)
        sign_position = action.index("Sign and verify macOS application bundle")
        final_verify_positions = [
            match.start()
            for match in re.finditer("verify_ios_native_stack.py", action)
            if match.start() > sign_position
        ]
        self.assertTrue(
            final_verify_positions,
            "the signed app-bundled closure must be reverified after application signing",
        )
        signed_verification = action[final_verify_positions[-1] :]
        self.assertIn("--source-receipt", signed_verification)
        self.assertIn("--legal-receipt", signed_verification)
        self.assertIn("--sbom", signed_verification)
        self.assertIn("--verify-package-receipt", signed_verification)

    def test_release_secrets_are_transported_through_step_environment(self):
        action = required_text(MAC_PACKAGE_ACTION)
        for unsafe in (
            'require_input "p12-password" "${{ inputs.p12-password }}"',
            '--password "${{ inputs.notarization-password }}"',
            'echo "KLOGG_CODESIGN=${{ inputs.codesign-identity }}"',
        ):
            self.assertNotIn(unsafe, action)
        for marker in (
            "KLOGG_P12_PASSWORD: ${{ inputs.p12-password }}",
            "KLOGG_NOTARIZATION_PASSWORD: ${{ inputs.notarization-password }}",
            'printf \'KLOGG_CODESIGN=%s\\n\'',
        ):
            self.assertIn(marker, action)

    def test_release_consumption_is_disconnected_and_fails_closed(self):
        action = required_text(MAC_PACKAGE_ACTION)
        self.assertTrue(
            "FETCHCONTENT_FULLY_DISCONNECTED=ON" in action,
            "macOS release consumption must configure with FetchContent fully disconnected",
        )
        self.assertTrue(
            "KLOGG_IOS_NATIVE_STACK_ROOT" in action,
            "macOS release consumption must require an explicit prefetched native stack root",
        )
        self.assertTrue(
            "verify_ios_native_stack.py" in action,
            "macOS packaging must invoke the fail-closed native stack verifier",
        )
        self.assertNotRegex(action, r"\|\|\s*(?:true|echo .*Warning)")

        build = required_text(THIRD_PARTY_CMAKE)
        self.assertIn("KLOGG_IOS_NATIVE_STACK_ROOT", build)
        self.assertRegex(build, r"(?:FATAL_ERROR|SEND_ERROR).*(?:iOS|IOS).*native")
        self.assertNotRegex(build, r"find_(?:package|library)\([^\n]*(?:usbmuxd|plist|imobiledevice)")


if __name__ == "__main__":
    unittest.main()
