import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).parents[2]
FIXTURE = ROOT / "tests" / "fixtures" / "adb_helper_cycle8_release_contract.json"
LOCK = ROOT / "packaging" / "adb" / "adb-helper.lock.json"
BUILD_SCRIPT = ROOT / "scripts" / "build_adb_helper.py"
VERIFY_SCRIPT = ROOT / "scripts" / "verify_adb_helper_artifact.py"
SUPERBUILD = ROOT / "packaging" / "adb" / "superbuild" / "CMakeLists.txt"
WINDOWS_PATCHES = ROOT / "packaging" / "adb" / "patches"
BUILD_ACTION = ROOT / ".github" / "actions" / "build-adb-helper" / "action.yml"
MAC_PACKAGE = ROOT / ".github" / "actions" / "agent-package-mac" / "action.yml"
WIN_PACKAGE = ROOT / ".github" / "actions" / "agent-package-win" / "action.yml"
CI_BUILD = ROOT / ".github" / "workflows" / "ci-build.yml"
CI_RELEASE = ROOT / ".github" / "workflows" / "ci-release.yml"


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

    def test_windows_x64_closure_is_private_dynamic_and_contains_every_source_built_dll(self):
        expected = self.fixture["helper_targets"]["windows-x86_64"]
        target = self.lock.get("targets", {}).get("windows-x86_64", {})
        usb = target.get("usb", {})
        runtime_files = set(usb.get("runtime_files", []))
        imports = set(usb.get("required_imports", []))

        self.assertEqual(usb.get("backend"), "dynamic-libusb")
        self.assertEqual(usb.get("linkage"), "shared")
        self.assertEqual(runtime_files, set(expected["required_runtime_files"]))
        self.assertTrue(imports.issuperset(runtime_files))
        self.assertIs(usb.get("replacement_probe_required"), True)

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
            "artifacts_id: macos-arm-qt6",
            "# Sanitizer legs:",
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
        self.assertIn("adb_target: macos-arm64", mac_arm_entry)
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
        windows_matrix = section(self.ci_build, "  Windows:\n", "  ci-gate:\n")
        x86_entry = section(
            windows_matrix,
            "artifacts_id: windows-x86-qt5",
            "# Sanitizer leg:",
        )
        matching_helper = re.search(r"^\s*adb_target:\s*windows-x86\s*$", x86_entry, re.MULTILINE)
        fail_closed = re.search(r"^\s*package:\s*false\s*$", x86_entry, re.MULTILINE)
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
        helper_job = section(self.ci_build, "  BuildAdbHelpers:\n", "  Linux:\n")
        linux_job = section(self.ci_build, "  Linux:\n", "  Mac:\n")
        mac_job = section(self.ci_build, "  Mac:\n", "  Windows:\n")
        windows_job = section(self.ci_build, "  Windows:\n", "  ci-gate:\n")

        self.assertIn("actions/attest-build-provenance", helper_job)
        for label, job in (
            ("Linux", linux_job),
            ("macOS", mac_job),
            ("Windows", windows_job),
        ):
            with self.subTest(job=label):
                self.assertIn("verify_adb_helper_envelope.py", job)
                self.assertIn("gh attestation verify", job)

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
        for package in packages:
            with self.subTest(package=package.get("id") if isinstance(package, dict) else package):
                self.assertIsInstance(package, dict)
                self.assertRegex(str(package.get("archive_sha256", "")), r"^[0-9a-f]{64}$")
                self.assertRegex(
                    str(package.get("archive_url", "")),
                    r"^https://mirror\.msys2\.org/mingw/ucrt64/.+\.pkg\.tar\.zst$",
                )
                self.assertIs(package.get("build_input"), False)
        helper_job = section(self.ci_build, "  BuildAdbHelpers:\n", "  Linux:\n")
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

    def test_source_build_artifacts_are_disconnected_hashed_attested_and_target_bound(self):
        expected = self.fixture["artifact_envelope"]
        helper_job = section(self.ci_build, "  BuildAdbHelpers:\n", "  Linux:\n")
        linux_job = section(self.ci_build, "  Linux:\n", "  Mac:\n")
        mac_job = section(self.ci_build, "  Mac:\n", "  Windows:\n")
        windows_job = section(self.ci_build, "  Windows:\n", "  ci-gate:\n")
        combined_build = "\n".join((helper_job, self.build_action, self.superbuild))
        failures = []

        if expected["disconnected_source_build"]:
            for marker in ("--offline", "FETCHCONTENT_FULLY_DISCONNECTED", "FETCHCONTENT_UPDATES_DISCONNECTED"):
                if marker not in combined_build:
                    failures.append(f"source build lacks disconnected marker {marker}")
        if re.search(r"(?:curl|wget|Invoke-WebRequest)", active_lines(combined_build), re.IGNORECASE):
            failures.append("source-build job performs a network download")

        for required_file in expected["required_files"]:
            if required_file not in helper_job:
                failures.append(f"helper artifact omits {required_file}")
        if expected["sha256_required"] and not re.search(
            r"sha256(?:sum|sum\.exe|sum -|:)" , helper_job, re.IGNORECASE
        ):
            failures.append("helper artifact is not hashed before upload")
        if expected["signature_or_attestation_required"] and not re.search(
            r"(?:attest-build-provenance|cosign|SHA256SUMS\.sig)", helper_job, re.IGNORECASE
        ):
            failures.append("helper artifact has no signature or build attestation")
        if "name: adb-helper-${{ matrix.target }}" not in helper_job:
            failures.append("helper artifact name is not target-bound")
        if "--expected-target \"${{ inputs.target }}\"" not in self.build_action:
            failures.append("helper verification receipt is not bound to the matrix target")

        package_bindings = {
            "linux-x86_64": (linux_job, "adb-helper-linux-x86_64"),
            "macos-matrix": (mac_job, "adb-helper-${{ matrix.config.adb_target }}"),
            "windows-matrix": (windows_job, "adb-helper-${{ matrix.config.adb_target }}"),
        }
        for label, (job, artifact_name) in package_bindings.items():
            if artifact_name not in job:
                failures.append(f"{label} package leg does not consume its exact helper artifact")

        self.assertEqual(failures, [], "\n".join(failures))


if __name__ == "__main__":
    unittest.main()
