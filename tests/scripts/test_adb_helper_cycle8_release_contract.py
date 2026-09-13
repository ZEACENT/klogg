from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
FIXTURE = ROOT / "tests" / "fixtures" / "adb_helper_cycle8_release_contract.json"
LOCK = ROOT / "packaging" / "adb" / "adb-helper.lock.json"
BUILD_SCRIPT = ROOT / "scripts" / "build_adb_helper.py"
VERIFY_SCRIPT = ROOT / "scripts" / "verify_adb_helper_artifact.py"
PREFETCH_SCRIPT = ROOT / "scripts" / "prefetch_adb_helper_sources.py"
SUPERBUILD = ROOT / "packaging" / "adb" / "superbuild" / "CMakeLists.txt"
WINDOWS_PATCHES = ROOT / "packaging" / "adb" / "patches"
BUILD_ACTION = ROOT / ".github" / "actions" / "build-adb-helper" / "action.yml"
MAC_PACKAGE = ROOT / ".github" / "actions" / "agent-package-mac" / "action.yml"
WIN_PACKAGE = ROOT / ".github" / "actions" / "agent-package-win" / "action.yml"
CI_BUILD = ROOT / ".github" / "workflows" / "ci-build.yml"
CI_RELEASE = ROOT / ".github" / "workflows" / "ci-release.yml"
GIT_ATTRIBUTES = ROOT / ".gitattributes"
CI_LINT = ROOT / "scripts" / "lint_ci_quality.py"
ADB_CACHE_KEY = (
    "adb-helper-sources-v2-${{ hashFiles('packaging/adb/adb-helper.lock.json', "
    "'scripts/prefetch_adb_helper_sources.py') }}"
)
ADB_CACHE_KEY_REFERENCE = "${{ steps.adb-cache-key.outputs.key }}"
ADB_CACHE_FALLBACK = "adb-helper-sources-v1-"
ADB_CACHE_MAX_BYTES = "536870912"

_CI_SPEC = importlib.util.spec_from_file_location("lint_ci_quality", CI_LINT)
assert _CI_SPEC is not None and _CI_SPEC.loader is not None
CI_MODULE = importlib.util.module_from_spec(_CI_SPEC)
_CI_SPEC.loader.exec_module(CI_MODULE)


def read_text(path: pathlib.Path) -> str:
    if not path.is_file():
        raise AssertionError(f"required cycle 8 contract input is missing: {path}")
    return path.read_text(encoding="utf-8")


def read_json(path: pathlib.Path) -> dict:
    document = json.loads(read_text(path))
    if not isinstance(document, dict):
        raise AssertionError(f"expected JSON object in {path}")
    return document


def records_by_id(records) -> dict:
    result = {}
    for record in records or []:
        if isinstance(record, dict) and isinstance(record.get("id"), str):
            result[record["id"]] = record
    return result


def active_lines(source: str) -> str:
    return "\n".join(
        line for line in source.splitlines() if line.strip() and not line.lstrip().startswith("#")
    )


def section(source: str, start: str, end: str) -> str:
    start_index = source.find(start)
    if start_index < 0:
        raise AssertionError(f"missing section start: {start}")
    end_index = source.find(end, start_index + len(start))
    if end_index < 0:
        raise AssertionError(f"missing section end after {start}: {end}")
    return source[start_index:end_index]


def version_tuple(value: str) -> tuple[int, ...]:
    if not isinstance(value, str) or re.fullmatch(r"\d+(?:\.\d+)+", value) is None:
        raise AssertionError(f"invalid dotted version: {value!r}")
    return tuple(int(component) for component in value.split("."))


def adb_cache_contract(workflow: str) -> tuple[list[str], str | None]:
    issues: list[str] = []
    blocks = CI_MODULE.workflow_job_blocks(workflow)
    job_block = blocks.get("PrefetchAdbHelperSources", [])
    job_env = CI_MODULE.workflow_mapping_block(job_block, "env", 4)
    configured_env = (
        {key: value for key, (value, _) in job_env.items()}
        if job_env is not None
        else {}
    )
    if configured_env.get("KLOGG_ADB_SOURCE_CACHE_MAX_BYTES") != ADB_CACHE_MAX_BYTES:
        issues.append("ADB source cache job must define the named byte limit")

    jobs = CI_MODULE.workflow_job_steps(workflow)
    steps = jobs.get("PrefetchAdbHelperSources", [])
    parsed = [CI_MODULE.workflow_step_fields(step) for step in steps]
    key_indexes = [
        index
        for index, (fields, _) in enumerate(parsed)
        if fields.get("id") == "adb-cache-key"
    ]
    restore_indexes = [
        index
        for index, (fields, _) in enumerate(parsed)
        if fields.get("uses", "").startswith("actions/cache/restore@")
    ]
    save_indexes = [
        index
        for index, (fields, _) in enumerate(parsed)
        if fields.get("uses", "").startswith("actions/cache/save@")
    ]
    upload_indexes = [
        index
        for index, (fields, _) in enumerate(parsed)
        if fields.get("uses", "").startswith("actions/upload-artifact@")
    ]
    prefetch_indexes = [
        index
        for index, (fields, _) in enumerate(parsed)
        if "python3 scripts/prefetch_adb_helper_sources.py" in fields.get("run", "")
    ]
    if not all(
        len(indexes) == 1
        for indexes in (
            key_indexes,
            restore_indexes,
            save_indexes,
            upload_indexes,
            prefetch_indexes,
        )
    ):
        issues.append("ADB source cache steps must be unique and structurally present")
        return issues, None

    key_index = key_indexes[0]
    restore_index = restore_indexes[0]
    save_index = save_indexes[0]
    upload_index = upload_indexes[0]
    prefetch_index = prefetch_indexes[0]
    key_fields, _ = parsed[key_index]
    restore_fields, restore_children = parsed[restore_index]
    prefetch_fields, prefetch_children = parsed[prefetch_index]
    save_fields, save_children = parsed[save_index]
    restore_with = restore_children.get("with", {})
    prefetch_env = prefetch_children.get("env", {})
    save_with = save_children.get("with", {})

    key_script = CI_MODULE.active_script_content(key_fields.get("run", ""))
    if ADB_CACHE_KEY not in key_script or "github.run_id" in key_script:
        issues.append("ADB source cache key step must define the exact v2 key")
    if restore_with.get("key") != ADB_CACHE_KEY_REFERENCE:
        issues.append("ADB source cache restore must reuse the named v2 key")
    if restore_with.get("restore-keys") != ADB_CACHE_FALLBACK:
        issues.append("ADB source cache restore must use only the controlled v1 fallback")
    if save_with.get("key") != ADB_CACHE_KEY_REFERENCE:
        issues.append("ADB source cache save must reuse the named v2 key")
    if (
        prefetch_env.get("KLOGG_ADB_SOURCE_CACHE_EXACT_KEY")
        != ADB_CACHE_KEY_REFERENCE
    ):
        issues.append("ADB source cache verification must reuse the named v2 key")
    prefetch_script = CI_MODULE.active_script_content(prefetch_fields.get("run", ""))
    if not all(
        marker in prefetch_script
        for marker in (
            '"$KLOGG_ADB_SOURCE_CACHE_MATCHED_KEY" == "$KLOGG_ADB_SOURCE_CACHE_EXACT_KEY"',
            "The exact ADB source cache failed verification",
            "exit 1",
        )
    ):
        issues.append("an invalid exact ADB source cache must fail closed")
    if prefetch_script.count(
        '--max-cache-bytes "$KLOGG_ADB_SOURCE_CACHE_MAX_BYTES"'
    ) != 2:
        issues.append("ADB source cache prefetch must enforce the named byte limit")
    if "github.run_id" in "\n".join(
        str(value) for value in (*restore_with.values(), *save_with.values())
    ):
        issues.append("ADB source cache keys must not use github.run_id")
    save_condition = save_fields.get("if", "")
    if (
        "github.event_name == 'push'" not in save_condition
        or "steps.cache-adb-sources.outputs.cache-hit != 'true'" not in save_condition
    ):
        issues.append("ADB source cache save must be push-only after an exact miss")
    if not (key_index < restore_index < prefetch_index < save_index < upload_index):
        issues.append("ADB source cache validation must precede save and upload")
    if "gh cache delete" in CI_MODULE.active_script_content(workflow):
        issues.append("ADB source cache fallback must not delete GitHub caches")
    return issues, prefetch_script


class AdbHelperCycle8ReleaseContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = read_json(FIXTURE)
        cls.lock = read_json(LOCK)
        cls.ci_build = read_text(CI_BUILD)
        cls.ci_release = read_text(CI_RELEASE)
        cls.build_action = read_text(BUILD_ACTION)
        cls.build_script = read_text(BUILD_SCRIPT)
        cls.superbuild = read_text(SUPERBUILD)
        cls.verify_script = read_text(VERIFY_SCRIPT)

    def test_ci_reuses_the_exact_v2_adb_cache_with_one_controlled_v1_fallback(self):
        issues, guard_script = adb_cache_contract(self.ci_build)
        self.assertEqual(issues, [], "\n".join(issues))
        self.assertIsNotNone(guard_script)

    def test_adb_cache_contract_rejects_key_fallback_spoof_and_guard_mutations(self):
        size_option = '--max-cache-bytes "$KLOGG_ADB_SOURCE_CACHE_MAX_BYTES"'
        mutations = {
            "missing fallback": self.ci_build.replace(
                f"          restore-keys: {ADB_CACHE_FALLBACK}\n", "", 1
            ),
            "overbroad fallback": self.ci_build.replace(
                ADB_CACHE_FALLBACK, "adb-helper-sources-", 1
            ),
            "run-id key": self.ci_build.replace(
                ADB_CACHE_KEY, ADB_CACHE_KEY + "-${{ github.run_id }}", 1
            ),
            "missing named limit": self.ci_build.replace(
                f"      KLOGG_ADB_SOURCE_CACHE_MAX_BYTES: {ADB_CACHE_MAX_BYTES}\n",
                "",
                1,
            ),
            "missing size enforcement": self.ci_build.replace(size_option, "", 1),
            "comment spoof": self.ci_build.replace(
                size_option, "# " + size_option, 1
            ),
            "invalid exact cache fallback": self.ci_build.replace(
                "The exact ADB source cache failed verification",
                "The restored ADB source cache failed verification",
                1,
            ),
        }
        for label, workflow in mutations.items():
            with self.subTest(label=label):
                issues, _ = adb_cache_contract(workflow)
                self.assertNotEqual(issues, [], label)

    def test_adb_cache_actual_size_guard_accepts_small_and_rejects_oversized_or_symlinked_closures(self):
        issues, prefetch_script = adb_cache_contract(self.ci_build)
        self.assertEqual(issues, [], "\n".join(issues))
        self.assertIsNotNone(prefetch_script)
        payload = b"1234"
        lock = {
            "sources": [
                {
                    "id": "archive",
                    "archive_file": "archive.tar.gz",
                    "archive_url": "https://example.invalid/archive.tar.gz",
                    "archive_sha256": hashlib.sha256(payload).hexdigest(),
                }
            ]
        }
        with tempfile.TemporaryDirectory() as parent:
            root = pathlib.Path(parent)
            lock_path = root / "lock.json"
            cache = root / "cache"
            lock_path.write_text(json.dumps(lock), encoding="utf-8")
            cache.mkdir()
            (cache / "archive.tar.gz").write_bytes(payload)

            def validate(maximum: int):
                return subprocess.run(
                    [
                        sys.executable,
                        str(PREFETCH_SCRIPT),
                        "--lock",
                        str(lock_path),
                        "--download-root",
                        str(cache),
                        "--offline",
                        "--max-cache-bytes",
                        str(maximum),
                    ],
                    text=True,
                    capture_output=True,
                    check=False,
                )

            accepted = validate(4096)
            self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)
            oversized = validate(4)
            self.assertNotEqual(oversized.returncode, 0)
            self.assertIn("exceeds", oversized.stdout + oversized.stderr)

            manifest = cache / "adb-helper-prefetch-manifest.json"
            manifest.unlink(missing_ok=True)
            os.symlink("archive.tar.gz", cache / "archive-link.tar.gz")
            symlinked = validate(4096)
            self.assertNotEqual(symlinked.returncode, 0)
            self.assertIn("unlocked entry", symlinked.stdout + symlinked.stderr)

    def test_fixture_distinguishes_buildability_from_native_device_qualification(self):
        classes = self.fixture.get("validation_classes")
        self.assertIsInstance(classes, dict)
        self.assertEqual(
            set(classes),
            {"locally-buildable", "cross-build-only", "native-device-qualified"},
        )
        self.assertIs(classes["locally-buildable"].get("release_qualified"), False)
        self.assertIs(classes["cross-build-only"].get("release_qualified"), False)
        self.assertIs(classes["native-device-qualified"].get("release_qualified"), True)
        self.assertIn(
            "native-device",
            classes["native-device-qualified"].get("required_receipts", []),
        )
        assigned = {
            plan.get("validation_class")
            for plan in self.fixture.get("helper_targets", {}).values()
        }
        self.assertIn("locally-buildable", assigned)
        self.assertIn("cross-build-only", assigned)
        self.assertNotIn(
            "native-device-qualified",
            assigned,
            "the RED fixture must not manufacture native-device evidence for CI-only builds",
        )

    def test_windows_x64_is_a_real_android_17_source_build_with_pinned_win32_patch_series(self):
        expected = self.fixture["helper_targets"]["windows-x86_64"]
        sources = records_by_id(self.lock.get("sources"))
        patches = self.lock.get("patches", [])
        violations = []

        manifest = sources.get("aosp-manifest", {})
        if manifest.get("tag") != expected["android_baseline"]:
            violations.append("the AOSP manifest is not locked to Android 17")
        nmeum = sources.get("android-tools-release", {})
        if nmeum.get("commit") != expected["nmeum_commit"] or nmeum.get("build_input") is not True:
            violations.append("the locked nmeum Android 17 source is not the Windows build input")
        platform_development = sources.get("windows-platform-development", {})
        if (
            platform_development.get("commit") != expected["platform_development_commit"]
            or platform_development.get("build_input") is not True
        ):
            violations.append("platform/development is not a verified Windows build input")
        if "adbwinapi" in sources:
            violations.append(
                "AdbWinApi/AdbWinUsbApi must be built from locked platform/development, not an external adbwinapi source"
            )

        patch_roles = {
            patch.get("role")
            for patch in patches
            if isinstance(patch, dict) and patch.get("target") == "windows-x86_64"
        }
        missing_roles = set(expected["required_patch_roles"]) - patch_roles
        if missing_roles:
            violations.append("missing pinned Windows patch roles: " + ", ".join(sorted(missing_roles)))
        for patch in patches:
            if not isinstance(patch, dict) or patch.get("target") != "windows-x86_64":
                continue
            path = ROOT / str(patch.get("path", ""))
            if not path.is_file() or re.fullmatch(r"[0-9a-f]{64}", str(patch.get("sha256", ""))) is None:
                violations.append(f"Windows patch is not path/hash locked: {patch!r}")

        windows_sources = "\n".join(
            read_text(path) for path in sorted(WINDOWS_PATCHES.glob("*.patch"))
        )
        for component in expected["required_source_components"]:
            if component not in windows_sources:
                violations.append(f"Windows source patch series does not build {component}")

        build_path = "\n".join((self.build_action, self.build_script, self.superbuild))
        if "source-build-incomplete" in build_path:
            violations.append("the native windows-x86_64 source build still exits as incomplete")

        self.assertEqual(violations, [], "\n".join(violations))

    def test_windows_x64_patch_series_honors_the_adb_only_build_boundary(self):
        adb_only_patches = [
            patch
            for patch in self.lock.get("patches", [])
            if isinstance(patch, dict)
            and patch.get("target") == "windows-x86_64"
            and patch.get("applies_to") == "android-tools-release"
            and patch.get("role") == "windows-adb-only"
            and patch.get("apply", True) is True
        ]
        self.assertEqual(len(adb_only_patches), 1)
        adb_only_patch = adb_only_patches[0]
        patch_path = ROOT / adb_only_patch["path"]
        self.assertTrue(patch_path.is_file())
        self.assertRegex(str(adb_only_patch.get("sha256", "")), r"^[0-9a-f]{64}$")

        patch_text = read_text(patch_path)
        self.assertIn('option(KLOGG_ADB_ONLY', patch_text)
        self.assertRegex(
            patch_text,
            re.compile(
                r"if\(NOT KLOGG_ADB_ONLY\).*?pkg_check_modules\(libpcre2-8",
                re.DOTALL,
            ),
        )
        self.assertRegex(
            patch_text,
            re.compile(
                r"include\(CMakeLists\.adb\.txt\).*?if\(NOT KLOGG_ADB_ONLY\).*?"
                r"include\(CMakeLists\.fastboot\.txt\)",
                re.DOTALL,
            ),
        )
        self.assertIn("add_library(libzip STATIC", patch_text)
        self.assertNotIn("pcre2", records_by_id(self.lock.get("dependencies")))

    def test_windows_x64_static_brotli_link_order_places_common_after_consumers(self):
        patches = [
            patch
            for patch in self.lock.get("patches", [])
            if isinstance(patch, dict)
            and patch.get("target") == "windows-x86_64"
            and patch.get("applies_to") == "android-tools-release"
            and patch.get("role") == "windows-static-link-order"
            and patch.get("apply", True) is True
        ]
        self.assertEqual(len(patches), 1)
        patch_text = read_text(ROOT / patches[0]["path"])
        self.assertIn(
            "-\tPkgConfig::libbrotlicommon\n"
            " \tPkgConfig::libbrotlidec\n"
            " \tPkgConfig::libbrotlienc\n"
            "+\tPkgConfig::libbrotlicommon\n",
            patch_text,
        )

    def test_windows_x64_helper_statically_links_the_mingw_runtime(self):
        patches = [
            patch
            for patch in self.lock.get("patches", [])
            if isinstance(patch, dict)
            and patch.get("target") == "windows-x86_64"
            and patch.get("role") == "private-windows-libusb"
            and patch.get("apply", True) is True
        ]
        self.assertEqual(len(patches), 1)
        patch_text = read_text(ROOT / patches[0]["path"])
        self.assertIn(
            'LINK_FLAGS "-municode -static -static-libgcc -static-libstdc++"',
            patch_text,
        )

    def test_binary_closure_inspection_precedes_execution_smoke(self):
        main = self.build_script.split("def main() -> int:", maxsplit=1)[1]
        self.assertLess(
            main.index(") = inspect_binary"),
            main.index("scripts/smoke_adb_helper.py"),
        )

    def test_adb_lock_has_platform_stable_lf_bytes(self):
        attributes = read_text(GIT_ATTRIBUTES)
        self.assertRegex(
            attributes,
            r"(?m)^packaging/adb/adb-helper\.lock\.json\s+text\s+eol=lf(?:\s|$)",
        )

    def test_windows_x64_closure_is_private_dynamic_and_contains_every_source_built_dll(self):
        expected = self.fixture["helper_targets"]["windows-x86_64"]
        target = self.lock.get("targets", {}).get("windows-x86_64", {})
        usb = target.get("usb", {})
        runtime_files = set(usb.get("runtime_files", []))
        imports = set(usb.get("required_imports", []))

        self.assertEqual(usb.get("backend"), "dynamic-libusb")
        self.assertEqual(usb.get("linkage"), "shared")
        self.assertEqual(runtime_files, set(expected["required_runtime_files"]))
        self.assertEqual(imports, set(expected["required_direct_imports"]))
        self.assertTrue(imports.issubset(runtime_files))
        self.assertEqual(
            usb.get("required_delayed_runtime_loads"),
            expected["required_delayed_runtime_loads"],
        )
        delayed_runtime_files = {
            record["runtime_file"] for record in usb["required_delayed_runtime_loads"]
        }
        self.assertFalse(imports & delayed_runtime_files)
        self.assertEqual(imports | delayed_runtime_files, runtime_files)
        self.assertIs(usb.get("replacement_probe_required"), True)

        system_policy = usb.get("allowed_system_imports_by_binary", {})
        observed_system_imports = {
            "adb.exe": {
                "ADVAPI32.dll",
                "KERNEL32.dll",
                "SHELL32.dll",
                "USER32.dll",
                "WS2_32.dll",
                "api-ms-win-crt-private-l1-1-0.dll",
            },
            "AdbWinApi.dll": {"KERNEL32.dll", "OLE32.dll", "SETUPAPI.dll"},
            "AdbWinUsbApi.dll": {"KERNEL32.dll", "OLE32.dll", "WINUSB.dll"},
            "libusb-1.0.dll": {
                "KERNEL32.dll",
                "api-ms-win-crt-private-l1-1-0.dll",
            },
        }
        self.assertEqual(set(system_policy), set(observed_system_imports))
        for binary_name, observed_imports in observed_system_imports.items():
            self.assertTrue(observed_imports.issubset(set(system_policy[binary_name])))
            self.assertNotIn("VCRUNTIME140.dll", system_policy[binary_name])

        helper_build = "\n".join((self.build_action, self.build_script, self.superbuild))
        self.assertNotRegex(
            helper_build,
            re.compile(
                r"(?:platform-tools-latest|ANDROID_SDK_ROOT|ANDROID_HOME|"
                r"(?:which|where|command\s+-v)\s+adb|(?:choco|winget|pacman|vcpkg)\s+install[^\n]*(?:adb|libusb|adbwin))",
                re.IGNORECASE,
            ),
        )
        for runtime in expected["required_runtime_files"]:
            self.assertIn(runtime, helper_build)

    def test_linux_appimage_and_arm64_helpers_share_a_locked_glibc_231_baseline(self):
        expected_targets = self.fixture["helper_targets"]
        targets = self.lock.get("targets", {})
        toolchains = self.lock.get("toolchains", {})
        failures = []

        for target_name in ("linux-x86_64", "linux-arm64"):
            expected_maximum = expected_targets[target_name]["glibc_maximum"]
            plan = targets.get(target_name, {})
            actual = plan.get("glibc_baseline")
            if not isinstance(actual, str) or version_tuple(actual) > version_tuple(expected_maximum):
                failures.append(
                    f"{target_name} GLIBC baseline {actual!r} exceeds {expected_maximum}"
                )
            toolchain = toolchains.get(target_name, {})
            toolchain_glibc = toolchain.get("glibc_version")
            if toolchain_glibc != actual:
                failures.append(
                    f"{target_name} toolchain does not lock the same GLIBC baseline as its binary target"
                )
            image = toolchain.get("container_image")
            digest = toolchain.get("container_digest")
            if not isinstance(image, str) or not image:
                failures.append(f"{target_name} lacks a locked helper build container image")
            if re.fullmatch(r"sha256:[0-9a-f]{64}", str(digest)) is None:
                failures.append(f"{target_name} lacks a locked helper build container digest")

        if targets.get("linux-x86_64", {}).get("glibc_baseline") != targets.get(
            "linux-arm64", {}
        ).get("glibc_baseline"):
            failures.append("Linux x86_64 and arm64 helper baselines do not match")

        appimage = self.fixture["package_targets"]["linux-appimage-x86_64"]
        if appimage["helper_target"] != "linux-x86_64" or appimage["maximum_glibc"] != "2.31":
            failures.append("the AppImage fixture is not bound to the glibc 2.31 x86_64 helper")

        self.assertEqual(failures, [], "\n".join(failures))

    def test_linux_helpers_do_not_depend_on_host_cxx_runtime_abis(self):
        linux_superbuild = section(self.superbuild, "elseif(UNIX)", "else()")
        self.assertIn("-static-libgcc", linux_superbuild)
        self.assertIn("-static-libstdc++", linux_superbuild)
        for target_name in ("linux-x86_64", "linux-arm64"):
            target = self.lock.get("targets", {}).get(target_name, {})
            imports = {str(item).lower() for item in target.get("allowed_dynamic_imports", [])}
            self.assertNotIn("libstdc++.so.6", imports)
            self.assertNotIn("libgcc_s.so.1", imports)

    def test_macos_arm64_helper_is_thin_and_matches_the_arm_package_minos(self):
        expected = self.fixture["helper_targets"]["macos-arm64"]
        target = self.lock.get("targets", {}).get("macos-arm64", {})
        usb = target.get("usb", {})
        mac_arm_entry = section(
            self.ci_build,
            "KLOGG_ARTIFACTS_ID: macos-arm-qt6",
            "    runs-on:",
        )

        self.assertEqual(target.get("arch"), expected["architecture"])
        self.assertEqual(target.get("deployment_target"), expected["deployment_target"])
        qualification = target.get("qualification")
        self.assertIsInstance(
            qualification,
            dict,
            "macOS arm64 must record locally-buildable evidence separately from native-device qualification",
        )
        self.assertEqual(qualification.get("validation_class"), expected["validation_class"])
        self.assertIs(
            qualification.get("release_qualified"),
            False,
            "a thin native build alone must not manufacture package/native-device qualification",
        )
        self.assertEqual(set(usb.get("frameworks", [])), set(expected["required_frameworks"]))
        self.assertTrue(set(expected["forbidden_imports"]).issubset(usb.get("forbidden_imports", [])))
        self.assertIn("KLOGG_ADB_HELPER_TARGET: macos-arm64", mac_arm_entry)
        self.assertIn(
            f"-DKLOGG_OSX_DEPLOYMENT_TARGET={expected['deployment_target']}",
            mac_arm_entry,
        )
        self.assertIn("-DCMAKE_OSX_ARCHITECTURES=arm64", self.superbuild)
        self.assertIn(
            f"-DCMAKE_OSX_DEPLOYMENT_TARGET={expected['deployment_target']}",
            self.superbuild,
        )

        inspection_contract = "\n".join((self.build_script, self.verify_script))
        for evidence in (
            "architectures",
            "deployment_target",
            "native_frameworks",
            "dynamic_imports",
        ):
            self.assertIn(evidence, inspection_contract)
        self.assertRegex(inspection_contract, r"architectures\s*!=\s*expected_architectures")

    def test_release_qualification_requires_build_smoke_and_package_receipts(self):
        required = set(self.fixture["qualification_receipts"])
        package_targets = self.lock.get("package_targets")
        failures = []
        if not isinstance(package_targets, dict):
            failures.append("lock lacks evidence-bearing package_targets qualification records")
            package_targets = {}

        for package_name, fixture_plan in self.fixture["package_targets"].items():
            plan = package_targets.get(package_name, {})
            qualification = plan.get("qualification")
            if not isinstance(qualification, dict):
                failures.append(f"{package_name} lacks a qualification object")
                continue
            state = qualification.get("validation_class")
            if state not in self.fixture["validation_classes"]:
                failures.append(f"{package_name} lacks a recognized validation class")
            receipts = set(qualification.get("required_receipts", []))
            if not required.issubset(receipts):
                failures.append(f"{package_name} does not require all qualification receipts")
            if qualification.get("release_qualified") is True:
                if state != "native-device-qualified":
                    failures.append(f"{package_name} claims release qualification without native-device evidence")
                if set(qualification.get("verified_receipts", [])) != receipts:
                    failures.append(f"{package_name} is qualified before every receipt is verified")
            if fixture_plan.get("helper_target") and plan.get("helper_target") != fixture_plan.get(
                "helper_target"
            ):
                failures.append(f"{package_name} is not bound to its exact helper target")

        for receipt_kind in required:
            normalized = receipt_kind.replace("-", "_")
            if normalized not in self.verify_script:
                failures.append(f"shared verifier does not require {receipt_kind} evidence")

        self.assertEqual(failures, [], "\n".join(failures))

    def test_windows_x86_package_never_consumes_the_x64_helper(self):
        windows_matrix = section(
            self.ci_build, "  WindowsX86:\n", "  WindowsAsan:\n"
        )
        x86_entry = windows_matrix
        matching_helper = re.search(r"^\s*KLOGG_ADB_HELPER_TARGET:\s*windows-x86\s*$", x86_entry, re.MULTILINE)
        fail_closed = re.search(r"^\s*KLOGG_PACKAGE_ENABLED:\s*false\s*$", x86_entry, re.MULTILINE)
        self.assertTrue(
            matching_helper or fail_closed,
            "Windows x86 packaging must be disabled or declare a matching windows-x86 helper",
        )
        if fail_closed:
            self.assertNotIn(
                "x86-Qt5-QTRegex",
                self.ci_build,
                "continuous release notes must not advertise disabled Windows x86 packages",
            )

        active_package = active_lines(read_text(WIN_PACKAGE))
        self.assertNotIn("--expected-target windows-x86_64", active_package)
        self.assertIn("--expected-target", active_package)
        self.assertRegex(active_package, r"expected-target[^\n]*(?:adb_target|KLOGG_ADB_HELPER_TARGET)")

    def test_macos_final_package_receipt_binds_signed_helper_and_notarized_dmg(self):
        mac_action = active_lines(read_text(MAC_PACKAGE))
        for marker in (
            "source_helper_sha256",
            "--signing-receipt",
            "--notarization-receipt",
            "--package-file",
        ):
            self.assertIn(marker, mac_action)
        final_verification = mac_action.rfind("verify_adb_helper_artifact.py")
        receipt_copy = mac_action.rfind("adb-helper-dmg-package-verification.json")
        self.assertGreater(final_verification, mac_action.find("notarytool submit"))
        self.assertGreater(receipt_copy, final_verification)

    def test_signing_and_notarization_are_explicit_release_qualification_gates(self):
        expected = self.fixture["package_targets"]["macos-arm64-dmg"]
        mac_action = active_lines(read_text(MAC_PACKAGE))
        release_policy = self.lock.get("release_policy", {})
        package_targets = self.lock.get("package_targets", {})
        mac_package = package_targets.get("macos-arm64-dmg", {}) if isinstance(package_targets, dict) else {}
        failures = []

        if release_policy.get("require_signing_for_release_qualification") is not True:
            failures.append("release policy does not require signing before qualification")
        if release_policy.get("require_notarization_for_macos_qualification") is not True:
            failures.append("release policy does not require macOS notarization before qualification")
        if expected["signing_required"] and "codesign --verify" not in mac_action:
            failures.append("active macOS package steps do not verify the app and DMG signatures")
        if expected["notarization_required"] and "notarytool submit" not in mac_action:
            failures.append("active macOS package steps do not notarize the DMG")
        if expected["notarization_required"] and "stapler validate" not in mac_action:
            failures.append("active macOS package steps do not validate the stapled ticket")
        qualification = mac_package.get("qualification", {})
        if qualification.get("release_qualified") is True and not {
            "signing",
            "notarization",
        }.issubset(set(qualification.get("verified_receipts", []))):
            failures.append("macOS package claims qualification without signing/notarization receipts")

        publishes_macos = "packages-macos-arm-qt6-vs-arm64/*.dmg" in self.ci_release
        if publishes_macos and ("codesign --verify" not in mac_action or "notarytool submit" not in mac_action):
            failures.append("release workflow publishes an unsigned or unnotarized macOS package")

        self.assertEqual(failures, [], "\n".join(failures))

    def test_cross_job_helper_consumers_verify_checksum_envelope_and_attestation(self):
        helper_job = section(self.ci_build, "  BuildAdbLinuxX64:\n", "  LinuxPackages:\n")
        linux_job = section(self.ci_build, "  LinuxPackages:\n", "  PrefetchIosNativeSources:\n")
        mac_job = section(self.ci_build, "  MacPackages:\n", "  MacSanitizers:\n")
        windows_job = section(self.ci_build, "  WindowsPackages:\n", "  WindowsX86:\n")

        self.assertIn("actions/attest-build-provenance", helper_job)
        self.assertNotIn("${{ matrix.target }}", helper_job)
        self.assertTrue(
            "adb-helper-linux-x86_64.tar.gz" in helper_job
            or (
                "KLOGG_ADB_HELPER_TARGET: linux-x86_64" in helper_job
                and "adb-helper-${{ env.KLOGG_ADB_HELPER_TARGET }}.tar.gz"
                in helper_job
            ),
            "the direct Linux helper archive must be bound by a literal or explicit job env",
        )
        self.assertIn("tar -czf", helper_job)
        self.assertIn("cygpath -u", helper_job)
        for label, job in (
            ("Linux", linux_job),
            ("macOS", mac_job),
            ("Windows", windows_job),
        ):
            with self.subTest(job=label):
                self.assertIn("verify_adb_helper_envelope.py", job)
                self.assertIn("gh attestation verify", job)
                self.assertIn("prefetch_artifacts/adb-helper-archive", job)
                self.assertIn("python3 scripts/extract_verified_tar.py", job)
                self.assertNotIn('tar -xzf "$archive"', job)
        self.assertIn(
            "-DKLOGG_ADB_HELPER_ARTIFACT_ROOT=/usr/local/prefetch_artifacts/adb-helper",
            linux_job,
        )

    def test_windows_uses_shared_verified_tar_extractor_for_native_paths(self):
        windows_job = section(self.ci_build, "  WindowsPackages:\n", "  WindowsX86:\n")
        extract = section(
            windows_job,
            "      - name: Extract mode-preserving ADB helper artifact\n",
            "      - uses: actions/download-artifact@",
        )
        active = active_lines(extract)
        self.assertIn("python3 scripts/extract_verified_tar.py", active)
        self.assertIn('--archive "$archive"', active)
        self.assertIn('--destination "$artifact_root"', active)
        self.assertNotIn('tar -xzf "$archive"', active)

    def test_windows_rejects_unbundled_mingw_runtime_dlls(self):
        target = self.lock.get("targets", {}).get("windows-x86_64", {})
        forbidden = {
            str(item).lower()
            for item in target.get("usb", {}).get("forbidden_imports", [])
        }
        self.assertTrue(
            {"libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"}.issubset(
                forbidden
            )
        )
        self.assertIn("forbidden_imports", self.build_script)
        self.assertIn("forbidden_imports", self.verify_script)

    def test_windows_compiler_closure_is_hash_locked_and_installed_offline(self):
        packages = self.lock.get("toolchain_packages")
        self.assertIsInstance(packages, list)
        package_ids = {
            package.get("id") for package in packages if isinstance(package, dict)
        }
        self.assertIn("mingw-w64-ucrt-x86_64-gcc", package_ids)
        self.assertIn("mingw-w64-ucrt-x86_64-libwinpthread", package_ids)
        self.assertIn("patch", package_ids)
        self.assertIn("nasm", package_ids)
        for package in packages:
            with self.subTest(package=package.get("id") if isinstance(package, dict) else package):
                self.assertIsInstance(package, dict)
                self.assertRegex(str(package.get("archive_sha256", "")), r"^[0-9a-f]{64}$")
                repository = (
                    "msys/x86_64"
                    if package.get("id") in {"patch", "nasm"}
                    else "mingw/ucrt64"
                )
                self.assertRegex(
                    str(package.get("archive_url", "")),
                    rf"^https://mirror\.msys2\.org/{repository}/.+\.pkg\.tar\.zst$",
                )
                self.assertIs(package.get("build_input"), False)
        helper_job = section(self.ci_build, "  BuildAdbLinuxX64:\n", "  LinuxPackages:\n")
        windows_setup = section(
            helper_job,
            "Prepare pinned MinGW compiler for the MSYS2 source patch series",
            "uses: actions/download-artifact@",
        )
        self.assertNotIn("mingw-w64-ucrt-x86_64-gcc\n", windows_setup)
        self.assertIn("pacman -U", helper_job)
        self.assertRegex(
            helper_job,
            r'"?\$RUNNER_TEMP"?/adb-helper-prefetch/\*\.pkg\.tar\.zst',
        )

    def test_linux_helper_container_writes_artifacts_as_the_runner_user(self):
        self.assertIn('--user "$(id -u):$(id -g)"', self.build_action)
        self.assertIn('-e HOME="$RUNNER_TEMP"', self.build_action)

    def test_source_build_artifacts_are_disconnected_hashed_attested_and_target_bound(self):
        expected = self.fixture["artifact_envelope"]
        helper_job = section(self.ci_build, "  BuildAdbLinuxX64:\n", "  LinuxPackages:\n")
        linux_job = section(self.ci_build, "  LinuxPackages:\n", "  PrefetchIosNativeSources:\n")
        mac_job = section(self.ci_build, "  MacPackages:\n", "  MacArmPackages:\n")
        mac_arm_job = section(self.ci_build, "  MacArmPackages:\n", "  MacSanitizers:\n")
        windows_job = section(self.ci_build, "  WindowsPackages:\n", "  WindowsX86:\n")
        combined_build = "\n".join((helper_job, self.build_action, self.superbuild))
        failures = []

        if expected["disconnected_source_build"]:
            for marker in ("--offline", "FETCHCONTENT_FULLY_DISCONNECTED", "FETCHCONTENT_UPDATES_DISCONNECTED"):
                if marker not in combined_build:
                    failures.append(f"source build lacks disconnected marker {marker}")
        if re.search(r"(?:curl|wget|Invoke-WebRequest)", active_lines(combined_build), re.IGNORECASE):
            failures.append("source-build job performs a network download")

        archives_complete_artifact_root = all(
            marker in helper_job
            for marker in (
                'artifact_root="$RUNNER_TEMP/adb-helper-artifact"',
                'tar -czf "$archive" -C "$artifact_root" .',
            )
        )
        for required_file in expected["required_files"]:
            if required_file not in combined_build or not archives_complete_artifact_root:
                failures.append(f"helper artifact omits {required_file}")
        if expected["sha256_required"] and not re.search(
            r"sha256(?:sum|sum\.exe|sum -|:)" , helper_job, re.IGNORECASE
        ):
            failures.append("helper artifact is not hashed before upload")
        if expected["signature_or_attestation_required"] and not re.search(
            r"(?:attest-build-provenance|cosign|SHA256SUMS\.sig)", helper_job, re.IGNORECASE
        ):
            failures.append("helper artifact has no signature or build attestation")
        if "${{ matrix.target }}" in helper_job:
            failures.append("direct helper jobs retain a dangling matrix target")
        if not (
            "name: adb-helper-linux-x86_64" in helper_job
            or (
                "KLOGG_ADB_HELPER_TARGET: linux-x86_64" in helper_job
                and "name: adb-helper-${{ env.KLOGG_ADB_HELPER_TARGET }}"
                in helper_job
            )
        ):
            failures.append("helper artifact name is not bound to the direct Linux target")
        if "--expected-target \"${{ inputs.target }}\"" not in self.build_action:
            failures.append("helper verification receipt is not bound to the explicit target input")

        linux_binding = (
            "adb-helper-linux-x86_64" in linux_job
            or (
                "adb_target: linux-x86_64" in linux_job
                and "adb-helper-${{ matrix.config.adb_target }}" in linux_job
            )
        )
        if not linux_binding:
            failures.append("linux-x86_64 package leg does not consume its exact helper artifact")

        shared_mac_env_binding = (
            "adb-helper-${{ env.KLOGG_ADB_HELPER_TARGET }}" in mac_job
        )
        for label, job, target in (
            ("macos-x86_64", mac_job, "macos-x86_64"),
            ("macos-arm64", mac_arm_job, "macos-arm64"),
        ):
            if not (
                f"adb-helper-{target}" in job
                or (
                    f"KLOGG_ADB_HELPER_TARGET: {target}" in job
                    and shared_mac_env_binding
                )
            ):
                failures.append(f"{label} package leg does not consume its exact helper artifact")
            if "matrix.config.adb_target" in job:
                failures.append(f"{label} package leg retains a dangling matrix target")

        windows_binding = (
            "adb-helper-windows-x86_64" in windows_job
            or (
                "KLOGG_ADB_HELPER_TARGET: windows-x86_64" in windows_job
                and "adb-helper-${{ env.KLOGG_ADB_HELPER_TARGET }}" in windows_job
            )
        )
        if not windows_binding:
            failures.append("windows-x86_64 package leg does not consume its exact helper artifact")
        if "matrix.config.adb_target" in windows_job:
            failures.append("windows-x86_64 package leg retains a dangling matrix target")

        self.assertEqual(failures, [], "\n".join(failures))


if __name__ == "__main__":
    unittest.main()
