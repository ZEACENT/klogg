import hashlib
import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).parents[2]
LOCK = ROOT / "packaging" / "adb" / "adb-helper.lock.json"
ADB_CMAKE = ROOT / "packaging" / "adb" / "CMakeLists.txt"
ADB_SUPERBUILD = ROOT / "packaging" / "adb" / "superbuild" / "CMakeLists.txt"
ADB_BUILD_SCRIPT = ROOT / "scripts" / "build_adb_helper.py"
ROOT_CMAKE = ROOT / "CMakeLists.txt"
APP_CMAKE = ROOT / "src" / "app" / "CMakeLists.txt"
NOTICE = ROOT / "NOTICE"
BUILD_ACTION = ROOT / ".github" / "actions" / "build-adb-helper" / "action.yml"
CI_BUILD = ROOT / ".github" / "workflows" / "ci-build.yml"
CI_RELEASE = ROOT / ".github" / "workflows" / "ci-release.yml"
DOCKER_PACKAGE = ROOT / ".github" / "actions" / "docker-package" / "action.yml"
MAC_PACKAGE = ROOT / ".github" / "actions" / "agent-package-mac" / "action.yml"
WIN_PACKAGE = ROOT / ".github" / "actions" / "agent-package-win" / "action.yml"
APPIMAGE_SCRIPT = ROOT / "packaging" / "linux" / "appimage" / "generate_appimage.sh"
WIN_PREPARE = ROOT / "packaging" / "windows" / "prepare_release.cmd"
NSIS = ROOT / "packaging" / "windows" / "klogg.nsi"
SEVEN_Z_LIST = ROOT / "packaging" / "windows" / "7z_klogg_listfile.txt"
VERIFY_SCRIPT = ROOT / "scripts" / "verify_adb_helper_artifact.py"
SMOKE_SCRIPT = ROOT / "scripts" / "smoke_adb_helper.py"
TOOLCHAIN_SCRIPT = ROOT / "scripts" / "verify_adb_helper_toolchain.py"

HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")

EXPECTED_SOURCES = {
    "aosp-manifest": {
        "commit": "5bc9a7ce1cd78dd53613bbfd0ebf506e1e4adb0f",
        "tag": "android-17.0.0_r1",
        "repository": "platform/manifest",
    },
    "aosp-adb": {
        "commit": "9084198a2d4b0f6a0f174260fb42da33485b684d",
        "repository": "platform/packages/modules/adb",
    },
    "android-tools-release": {
        "commit": "0c15591afc76852efea2f46f29f94b60aac44750",
        "repository": "nmeum/android-tools",
        "archive_sha256": "2725d09f892a3a38e534429f47a321f58ecf6a3169caa42c915fb2cb7d46be0e",
    },
    "aosp-libusb": {
        "commit": "70460fc2b43c3948f9caae1fd4eacd2d666a872b",
        "repository": "platform/external/libusb",
    },
    "windows-platform-development": {
        "commit": "4dafd114fab3c3d9543a5aff0ad097f479915176",
        "repository": "platform/development",
    },
}

EXPECTED_TARGETS = {
    "linux-x86_64": ("linux", "x86_64"),
    "linux-arm64": ("linux", "arm64"),
    "windows-x86_64": ("windows", "x86_64"),
    "macos-x86_64": ("macos", "x86_64"),
    "macos-arm64": ("macos", "arm64"),
}

EXPECTED_INSTALL_PATHS = {
    "app-linux": "output/helpers/adb",
    "app-windows": "output/helpers/adb.exe",
    "app-macos": "output/klogg.app/Contents/MacOS/helpers/adb",
    "deb": "usr/bin/helpers/adb",
    "appimage": "usr/bin/helpers/adb",
    "dmg": "klogg.app/Contents/MacOS/helpers/adb",
    "nsis": "helpers/adb.exe",
    "7z": "release/helpers/adb.exe",
}

REQUIRED_RELEASE_ASSET_KINDS = {
    "source-archive",
    "licenses",
    "notices",
    "sbom",
    "source-offer",
    "source-manifest",
    "source-set-receipt",
}


def normalized(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", value.lower())


class AdbHelperReleaseContractTest(unittest.TestCase):
    def required_text(self, path: pathlib.Path) -> str:
        self.assertTrue(path.is_file(), f"required ADB helper contract file is missing: {path}")
        return path.read_text(encoding="utf-8")

    def lock(self) -> dict:
        source = self.required_text(LOCK)
        try:
            document = json.loads(source)
        except json.JSONDecodeError as error:
            self.fail(f"ADB helper lock is not valid JSON: {error}")
        self.assertIsInstance(document, dict)
        return document

    def source_map(self, document: dict) -> dict:
        sources = document.get("sources")
        self.assertIsInstance(sources, list, "lock.sources must be a JSON array")
        result = {}
        for source in sources:
            self.assertIsInstance(source, dict)
            source_id = source.get("id")
            self.assertIsInstance(source_id, str)
            self.assertNotIn(source_id, result, f"duplicate locked source id: {source_id}")
            result[source_id] = source
        return result

    def test_lock_records_exact_upstream_baseline_and_immutable_archives(self):
        document = self.lock()
        self.assertEqual(document.get("schema_version"), 2)
        sources = self.source_map(document)
        self.assertEqual(set(sources), set(EXPECTED_SOURCES))

        for source_id, expected in EXPECTED_SOURCES.items():
            with self.subTest(source=source_id):
                source = sources[source_id]
                commit = source.get("commit")
                self.assertEqual(commit, expected["commit"])
                self.assertRegex(commit, HEX40)
                if "tag" in expected:
                    self.assertEqual(source.get("tag"), expected["tag"])

                repository_url = source.get("repository_url", "")
                archive_url = source.get("archive_url", "")
                archive_sha256 = source.get("archive_sha256", "")
                self.assertIn(expected["repository"], repository_url)
                self.assertTrue(repository_url.startswith("https://"))
                self.assertTrue(archive_url.startswith("https://"))
                self.assertIn(commit, archive_url, "archive URL must name the immutable commit")
                self.assertRegex(archive_sha256, HEX64)
                self.assertNotEqual(archive_sha256, "0" * 64)
                self.assertNotRegex(
                    archive_url.lower(),
                    r"(?:refs/heads|/(?:main|master|latest|stable)(?:[/.]|$)|platform-tools-latest)",
                )
                if "archive_sha256" in expected:
                    self.assertEqual(archive_sha256, expected["archive_sha256"])

    def test_lock_hashes_every_patch_and_identifies_every_toolchain(self):
        document = self.lock()
        patches = document.get("patches")
        self.assertIsInstance(patches, list)
        self.assertTrue(patches, "the source-build patch series must be explicitly locked")
        for patch in patches:
            with self.subTest(patch=patch):
                relative_path = patch.get("path", "")
                expected_hash = patch.get("sha256", "")
                self.assertRegex(expected_hash, HEX64)
                patch_path = ROOT / relative_path
                self.assertTrue(patch_path.is_file(), f"locked patch does not exist: {relative_path}")
                actual_hash = hashlib.sha256(patch_path.read_bytes()).hexdigest()
                self.assertEqual(actual_hash, expected_hash)
                if patch.get("apply", True) is not False:
                    self.assertEqual(
                        patch.get("apply_tool"),
                        "gnu-patch",
                        "tarball source patches must not inherit an enclosing Git repository",
                    )

        adb_patch = self.required_text(
            ROOT / "packaging/adb/patches/0001-klogg-adb-only-private-usb.patch"
        )
        link_block = adb_patch.split(
            "@@ -217,16 +232,22 @@ target_link_libraries(adb", 1
        )[1].split("@@", 1)[0]
        self.assertLess(link_block.index("\tPkgConfig::libbrotlidec"), link_block.index("\tPkgConfig::libbrotlienc"))
        self.assertLess(
            link_block.index("\tPkgConfig::libbrotlienc"),
            link_block.index("+\tPkgConfig::libbrotlicommon"),
        )

        toolchains = document.get("toolchains")
        self.assertIsInstance(toolchains, dict)
        self.assertEqual(set(toolchains), set(EXPECTED_TARGETS))
        for target, toolchain in toolchains.items():
            with self.subTest(target=target):
                identity_fields = [
                    "identifier",
                    "compiler",
                    "compiler_version",
                    "cmake_version",
                    "cmake_generator",
                    "runner_image",
                ]
                if target.startswith("linux-"):
                    identity_fields.extend(("container_image", "container_digest"))
                else:
                    identity_fields.extend(("hosted_image_family", "ninja_version"))
                    self.assertNotIn(
                        "runner_image_revision",
                        toolchain,
                        "hosted image revisions rotate independently; lock the stable family and exact tools",
                    )
                for field in identity_fields:
                    self.assertIsInstance(toolchain.get(field), str)
                    self.assertTrue(toolchain[field].strip(), f"{target} lacks locked {field}")
                locked_identity = " ".join(toolchain[field] for field in identity_fields)
                self.assertNotRegex(
                    locked_identity.lower(),
                    r"\b(?:latest|current|stable|rolling)\b",
                )

    def test_locked_patch_support_files_have_canonical_checkout_bytes(self):
        attributes = self.required_text(ROOT / ".gitattributes")
        self.assertIn("**/patches/**/*.def text eol=lf", attributes)

    def test_native_toolchains_bind_stable_hosted_families_not_ephemeral_revisions(self):
        script = self.required_text(TOOLCHAIN_SCRIPT)
        workflow = self.required_text(CI_BUILD)
        self.assertIn("verify_hosted_image_family(expected, image_os)", script)
        self.assertNotIn('image_version != expected["runner_image_revision"]', script)
        self.assertIn('command_text(["cl", "/Bv"]', script)
        self.assertIn("lukka/get-cmake@fffaaafeea488556c2c12dad60690008bc1caacb", workflow)
        self.assertIn("cmakeVersion: 3.31.6", workflow)
        self.assertIn("ninjaVersion: 1.12.1", workflow)
        for family in ("macos15", "win22"):
            self.assertIn(f'"hosted_image_family": "{family}"', LOCK.read_text())

    def test_windows_build_selects_the_msys2_gnu_patch_binary_explicitly(self):
        workflow = self.required_text(BUILD_ACTION)
        windows_build = workflow.split(
            "- name: Build native Windows helper from prefetched sources", 1
        )[1].split("- name: Smoke and verify source-built ADB artifact", 1)[0]
        self.assertIn(
            'KLOGG_ADB_PATCH_EXECUTABLE="$(cygpath -w /usr/bin/patch.exe)"',
            windows_build,
        )

    def test_windows_hosted_toolchain_locks_the_serviced_msvc_release_family(self):
        toolchain = self.lock()["toolchains"]["windows-x86_64"]
        compiler_version = toolchain.get("compiler_version", "")
        self.assertRegex(
            compiler_version,
            r"^\d+\.\d+$",
            "windows-2022 services MSVC patch binaries in place; lock the stable "
            "major.minor release family rather than an ephemeral patch build",
        )
        self.assertIn(compiler_version, toolchain.get("identifier", ""))

    def test_windows_superbuild_uses_native_zlib_name_and_dll_definitions(self):
        superbuild = self.required_text(ADB_SUPERBUILD)
        self.assertIn(
            'set(_zlib_static_library "${KLOGG_ADB_INSTALL_PREFIX}/lib/libzlibstatic.a")',
            superbuild,
        )
        self.assertIn("-DZLIB_LIBRARY=${_zlib_static_library}", superbuild)
        self.assertIn(
            "find_program(KLOGG_ADB_NASM_EXECUTABLE NAMES nasm.exe REQUIRED)", superbuild
        )
        self.assertIn(
            "-DCMAKE_ASM_NASM_COMPILER=${KLOGG_ADB_NASM_EXECUTABLE}", superbuild
        )

        windows_patch = self.required_text(
            ROOT / "packaging/adb/patches/0006-platform-development-adbwinapi-cmake.patch"
        )
        self.assertGreaterEqual(windows_patch.count("_WINDLL"), 2)

    def test_lock_forbids_prebuilt_sdk_path_runtime_and_system_fallbacks(self):
        document = self.lock()
        policy = document.get("release_policy")
        self.assertIsInstance(policy, dict)
        expected_false = {
            "allow_google_platform_tools_prebuilt",
            "allow_path_adb",
            "allow_android_sdk_adb",
            "allow_runtime_download",
            "allow_floating_revisions",
            "allow_system_dependency_substitution",
        }
        self.assertTrue(expected_false.issubset(policy))
        for key in expected_false:
            self.assertIs(policy[key], False, f"release policy must set {key}=false")
        self.assertIs(policy.get("source_build_required"), True)
        self.assertIs(policy.get("fail_closed_on_missing_helper"), True)

    def test_lock_describes_a_complete_adb_and_platform_usb_contract(self):
        document = self.lock()
        helper = document.get("helper")
        self.assertIsInstance(helper, dict)
        self.assertEqual(helper.get("product"), "adb")
        self.assertEqual(helper.get("kind"), "complete-adb-executable")
        self.assertIs(helper.get("source_built"), True)
        self.assertIs(helper.get("server_only_fork"), False)
        self.assertIn("version", helper.get("required_client_commands", []))
        self.assertIn("help", helper.get("required_client_commands", []))
        self.assertIn("server", helper.get("required_roles", []))
        self.assertIn("client", helper.get("required_roles", []))

        targets = document.get("targets")
        self.assertIsInstance(targets, dict)
        self.assertEqual(set(targets), set(EXPECTED_TARGETS))
        for target, (expected_os, expected_arch) in EXPECTED_TARGETS.items():
            with self.subTest(target=target):
                plan = targets[target]
                self.assertEqual(plan.get("os"), expected_os)
                self.assertEqual(plan.get("arch"), expected_arch)
                self.assertEqual(plan.get("toolchain"), target)
                usb = plan.get("usb")
                self.assertIsInstance(usb, dict)
                if plan.get("source_build_status"):
                    self.assertIs(
                        plan.get("qualified"),
                        False,
                        f"incomplete target {target} must not claim release qualification",
                    )
                else:
                    self.assertIs(plan.get("qualified"), True)
                if expected_os == "linux":
                    self.assertRegex(plan.get("glibc_baseline", ""), r"^\d+\.\d+$")
                    loader = (
                        "ld-linux-x86-64.so.2"
                        if expected_arch == "x86_64"
                        else "ld-linux-aarch64.so.1"
                    )
                    self.assertIn(loader, plan.get("allowed_dynamic_imports", []))
                if expected_os in ("linux", "windows"):
                    self.assertEqual(usb.get("backend"), "dynamic-libusb")
                    self.assertEqual(usb.get("linkage"), "shared")
                    self.assertTrue(usb.get("required_imports"))
                    self.assertTrue(usb.get("runtime_files"))
                    if expected_os == "linux":
                        self.assertEqual(
                            set(usb.get("required_imports", [])),
                            set(usb.get("runtime_files", [])),
                        )
                        self.assertEqual(usb.get("required_delayed_runtime_loads", []), [])
                    else:
                        self.assertTrue(
                            set(usb.get("required_imports", [])).issubset(
                                set(usb.get("runtime_files", []))
                            )
                        )
                        self.assertTrue(usb.get("required_delayed_runtime_loads"))
                    self.assertIs(usb.get("replacement_probe_required"), True)
                else:
                    self.assertEqual(usb.get("backend"), "native-iokit")
                    self.assertIn("IOKit", usb.get("frameworks", []))
                    self.assertIn("CoreFoundation", usb.get("frameworks", []))
                    self.assertIn("libusb", usb.get("forbidden_imports", []))
                    self.assertIn(
                        "/usr/lib/libc++.1.dylib",
                        plan.get("allowed_dynamic_imports", []),
                    )

    def test_lock_install_layout_matches_adb_live_services_resolver(self):
        document = self.lock()
        self.assertEqual(document.get("install_paths"), EXPECTED_INSTALL_PATHS)

        resolver = self.required_text(ROOT / "src" / "ui" / "src" / "adbliveservices.cpp")
        self.assertIn('QStringLiteral( "helpers/%1" )', resolver)
        self.assertIn('QStringLiteral( "adb.exe" )', resolver)
        self.assertIn('QStringLiteral( "adb" )', resolver)

    def test_lock_requires_reproducibility_legal_and_source_offer_assets(self):
        document = self.lock()
        assets = document.get("release_assets")
        self.assertIsInstance(assets, list)
        kinds = {asset.get("kind") for asset in assets if isinstance(asset, dict)}
        self.assertTrue(REQUIRED_RELEASE_ASSET_KINDS.issubset(kinds))
        for asset in assets:
            with self.subTest(asset=asset):
                self.assertIs(asset.get("required"), True)
                self.assertIsInstance(asset.get("file_name"), str)
                self.assertTrue(asset["file_name"])
                self.assertIsInstance(asset.get("sha256_file"), str)
                self.assertTrue(asset["sha256_file"])

        sources = self.source_map(document)
        for source_id, source in sources.items():
            with self.subTest(source=source_id):
                legal = source.get("legal")
                self.assertIsInstance(legal, dict)
                self.assertTrue(legal.get("licenses"))
                self.assertTrue(legal.get("notices"))
                self.assertIsInstance(legal.get("source_offer_label"), str)
                self.assertTrue(legal["source_offer_label"])

    def test_lock_assigns_package_and_release_distribution_to_every_external_asset(self):
        assets = self.lock().get("release_assets")
        self.assertIsInstance(assets, list)
        by_kind = {
            asset.get("kind"): asset for asset in assets if isinstance(asset, dict)
        }
        self.assertEqual(set(by_kind), REQUIRED_RELEASE_ASSET_KINDS)
        for kind, asset in by_kind.items():
            with self.subTest(kind=kind):
                self.assertIs(asset.get("required"), True)
                distribution = asset.get("distribution")
                self.assertIsInstance(
                    distribution, dict, f"{kind} must declare package/release distribution"
                )
                self.assertIs(distribution.get("release_required"), True)
                self.assertIs(
                    distribution.get("package_required"),
                    kind != "source-archive",
                    f"unexpected package scope for {kind}",
                )

    def test_cmake_builds_and_installs_the_locked_helper_fail_closed(self):
        root_cmake = self.required_text(ROOT_CMAKE)
        adb_cmake = self.required_text(ADB_CMAKE)
        app_cmake = self.required_text(APP_CMAKE)
        combined = "\n".join((root_cmake, adb_cmake, app_cmake))

        self.assertRegex(root_cmake, r"add_subdirectory\(\s*packaging/adb\s*\)")
        for target in (
            "klogg_adb_helper",
            "klogg_adb_helper_source_archive",
            "klogg_adb_helper_licenses",
            "klogg_adb_helper_notices",
            "klogg_adb_helper_sbom",
            "klogg_adb_helper_source_offer",
            "klogg_adb_helper_verify",
            "klogg_adb_helper_smoke",
        ):
            self.assertIn(target, adb_cmake)

        self.assertRegex(combined, r"add_dependencies\(\s*klogg\s+klogg_adb_helper")
        self.assertRegex(
            app_cmake,
            r"add_custom_target\(\s*klogg_stage_adb_helpers\s+ALL",
            "incremental builds must restage a changed verified helper even when klogg does not relink",
        )
        self.assertRegex(
            app_cmake,
            r"add_dependencies\(\s*ci_build\s+klogg_stage_adb_helpers",
        )
        self.assertNotRegex(
            adb_cmake,
            r"set\(\s*FETCHCONTENT_FULLY_DISCONNECTED\b",
            "application packaging glue must not disable FetchContent globally for third-party dependencies",
        )
        superbuild = self.required_text(ROOT / "packaging" / "adb" / "superbuild" / "CMakeLists.txt")
        common_args = re.search(r"set\(_common_args(?P<body>.*?)\n\)", superbuild, re.DOTALL)
        self.assertIsNotNone(common_args)
        self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", common_args.group("body"))
        self.assertIn("-DFETCHCONTENT_UPDATES_DISCONNECTED=ON", common_args.group("body"))
        self.assertIn(
            "-DZLIB_LIBRARY=${_zlib_static_library}", superbuild
        )
        self.assertIn("adb-helper.lock.json", adb_cmake)
        self.assertRegex(adb_cmake, r"install\(\s*PROGRAMS[^)]+helpers", re.DOTALL)
        self.assertRegex(
            adb_cmake,
            re.compile(
                r"if\(NOT APPLE\).*?install\(\s*PROGRAMS[^)]+helpers.*?endif\(\)",
                re.DOTALL,
            ),
            "macOS must carry the helper only inside the signed app bundle",
        )
        self.assertRegex(adb_cmake, r"FATAL_ERROR[^\n]*(?:missing|not found)")
        self.assertNotRegex(adb_cmake, r"find_program\([^)]*\badb\b")
        self.assertNotRegex(adb_cmake, r"(?:find_package|pkg_check_modules|find_library)\([^)]*libusb")

    def test_package_definitions_stage_exact_resolver_paths_and_verify_first(self):
        files = {
            "linux-cpack": self.required_text(ROOT_CMAKE),
            "linux-package-action": self.required_text(DOCKER_PACKAGE),
            "appimage": self.required_text(APPIMAGE_SCRIPT),
            "mac-dmg": self.required_text(MAC_PACKAGE),
            "windows-package-action": self.required_text(WIN_PACKAGE),
            "windows-prepare": self.required_text(WIN_PREPARE),
            "windows-nsis": self.required_text(NSIS),
            "windows-7z": self.required_text(SEVEN_Z_LIST),
        }

        self.assertRegex(files["linux-cpack"], r"DESTINATION[^\n]*bin/helpers")
        self.assertIn("appdir/usr/bin/helpers/adb", files["appimage"])
        self.assertRegex(files["appimage"], r"test\s+-x[^\n]*usr/bin/helpers/adb")
        self.assertIn("--maximum-glibc-version 2.31", files["appimage"])
        self.assertIn("--maximum-glibc-version 2.31", files["appimage"])
        self.assertIn("$KLOGG_MAC_APP/Contents/MacOS/helpers/adb", files["mac-dmg"])
        self.assertIn(
            'cp -a "$KLOGG_BUILD_ROOT/output/klogg.app" "$KLOGG_MAC_APP"',
            files["mac-dmg"],
        )
        self.assertRegex(files["windows-prepare"], r"output\\helpers\\adb\.exe")
        self.assertRegex(files["windows-prepare"], r"release\\helpers\\adb\.exe")
        self.assertRegex(files["windows-prepare"], r"if\s+not\s+exist[^\n]*adb\.exe")
        self.assertRegex(files["windows-prepare"], r"if\s+not\s+exist[^\n]*libusb-1\.0\.dll")
        self.assertRegex(files["windows-nsis"], r"SetOutPath\s+\$INSTDIR\\helpers")
        self.assertRegex(files["windows-nsis"], r"File\s+release\\helpers\\adb\.exe")
        self.assertRegex(files["windows-nsis"], r"File\s+release\\helpers\\libusb-1\.0\.dll")
        self.assertNotRegex(
            files["windows-nsis"], r"File\s+/nonfatal\s+release\\helpers\\libusb-1\.0\.dll"
        )
        self.assertIn(r".\release\helpers\adb.exe", files["windows-7z"])
        self.assertIn(r".\release\helpers\libusb-1.0.dll", files["windows-7z"])

        for package in ("linux-package-action", "mac-dmg", "windows-package-action"):
            with self.subTest(package=package):
                self.assertIn("verify_adb_helper_artifact", files[package])
                self.assertIn("smoke_adb_helper", files[package])

    def test_release_matrix_builds_every_target_and_publishes_source_assets(self):
        build_action = self.required_text(BUILD_ACTION)
        ci_build = self.required_text(CI_BUILD)
        ci_release = self.required_text(CI_RELEASE)
        combined = "\n".join((build_action, ci_build, ci_release))

        for target in EXPECTED_TARGETS:
            self.assertIn(target, combined)
        for required in (
            "adb-helper.lock.json",
            "verify_adb_helper_artifact.py",
            "smoke_adb_helper.py",
            "FETCHCONTENT_FULLY_DISCONNECTED=ON",
        ):
            self.assertIn(required, combined)
        self.assertNotRegex(
            ci_build,
            re.compile(r"tar\s+-x[^\n]*adb-helper-source-cache", re.IGNORECASE),
            "the cross-job source cache must not be extracted by raw tar before member validation",
        )
        self.assertIn("--require-lock-binding", build_action)
        self.assertIn("/opt/python/cp311-cp311/bin/python3", build_action)
        superbuild = self.required_text(ADB_SUPERBUILD)
        build_script = self.required_text(ADB_BUILD_SCRIPT)
        self.assertIn('CMAKE_GENERATOR "${CMAKE_GENERATOR}"', superbuild)
        self.assertIn('-G "${CMAKE_GENERATOR}"', superbuild)
        self.assertIn("--enable-new-dtags", superbuild)
        self.assertIn("CMAKE_BUILD_PARALLEL_LEVEL", build_script)

        for kind in REQUIRED_RELEASE_ASSET_KINDS:
            self.assertIn(kind, ci_release)
        self.assertIn("adb-helper", ci_release)
        self.assertRegex(ci_release, r"sha256sum[^\n]*adb")

    def test_release_paths_do_not_use_prebuilt_sdk_path_or_floating_adb(self):
        paths = (
            ADB_CMAKE,
            BUILD_ACTION,
            CI_BUILD,
            CI_RELEASE,
            DOCKER_PACKAGE,
            MAC_PACKAGE,
            WIN_PACKAGE,
            APPIMAGE_SCRIPT,
            WIN_PREPARE,
        )
        combined = "\n".join(
            self.required_text(path).replace(
                "github.ref == 'refs/heads/master'", "github.ref is trusted master"
            )
            for path in paths
        )
        forbidden = {
            "Google platform-tools prebuilt": r"dl\.google\.com/[^\s'\"]*platform-tools|platform-tools-latest",
            "PATH adb lookup": r"(?:which|where|command\s+-v)\s+adb\b",
            "Android SDK adb": r"(?:ANDROID_HOME|ANDROID_SDK_ROOT|platform-tools[/\\]adb)",
            "floating source revision": r"(?:refs/heads|/archive/(?:main|master|latest|stable)|@(?:main|master|latest|stable)\b)",
            "runtime helper download": r"(?:curl|wget|Invoke-WebRequest)[^\n]*(?:platform-tools|\badb(?:\.exe)?\b)",
            "system libusb substitution": r"(?:find_package|pkg_check_modules|find_library)[^\n]*libusb",
        }
        for label, pattern in forbidden.items():
            with self.subTest(policy=label):
                self.assertNotRegex(combined, re.compile(pattern, re.IGNORECASE))

    def test_notice_and_source_manifest_cover_every_locked_material(self):
        document = self.lock()
        notice = normalized(self.required_text(NOTICE))
        source_manifest_asset = next(
            (
                asset
                for asset in document.get("release_assets", [])
                if asset.get("kind") == "source-manifest"
            ),
            None,
        )
        self.assertIsNotNone(source_manifest_asset)

        missing = []
        for source_id, source in self.source_map(document).items():
            labels = [source_id, source.get("repository_url", "")]
            legal = source.get("legal", {})
            labels.append(legal.get("notice_label", ""))
            if not any(label and normalized(label) in notice for label in labels):
                missing.append(source_id)
        self.assertEqual(missing, [], "NOTICE lacks locked ADB source coverage: " + ", ".join(missing))

    def test_shared_verifier_and_smoke_are_registered_with_ctest(self):
        tests_cmake = self.required_text(ROOT / "tests" / "CMakeLists.txt")
        self.assertTrue(VERIFY_SCRIPT.is_file(), f"missing shared artifact verifier: {VERIFY_SCRIPT}")
        self.assertTrue(SMOKE_SCRIPT.is_file(), f"missing shared package smoke probe: {SMOKE_SCRIPT}")
        self.assertIn("adb_helper_release_contract", tests_cmake)
        self.assertIn("test_adb_helper_release_contract.py", tests_cmake)
        self.assertIn("test_adb_helper_verifier_contract.py", tests_cmake)


if __name__ == "__main__":
    unittest.main()
