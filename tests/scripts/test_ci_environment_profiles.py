"""Concrete image family, lane parity and policy identity contracts."""
from __future__ import annotations

import copy
import hashlib
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_environment as ci
import ci_environment_profiles as profiles_api
import qualify_ci_analysis as analysis

RECIPES = ROOT / "ci/environments/recipes.json"
PROFILES = ROOT / "ci/environments/profiles.json"
SCRIPT = ROOT / "scripts/ci_environment_profiles.py"
ANALYSIS = "noble-qt693-analysis"
EXPECTED = {
    "focal-qt5-gcc13": {"appimage"}, "jammy-qt5": {"deb", "asan-lsan", "ubsan"},
    "noble-qt6": {"deb"}, "resolute-qt6": {"deb"}, "jammy-qt5-tsan": {"tsan"},
    ANALYSIS: {"static", "coverage", "codeql"},
}


class EnvironmentProfilesTest(unittest.TestCase):
    def setUp(self):
        self.catalog = ci.load_json(RECIPES)
        self.profiles = ci.load_json(PROFILES)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        paths = set(self.profiles["verification_files"] + self.profiles["linux_defaults"]["verification_files"])
        for family in self.catalog["families"].values():
            paths.update(family["recipe_files"])
        for family in self.profiles["families"].values():
            for profile in family.values():
                paths.update(profile["verification_files"])
        for relative in paths:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            origin = ROOT / relative
            # Isolate schema tests from the parallel owner's pending new recipe.
            path.write_bytes(origin.read_bytes() if origin.is_file() else b"fixture recipe input\n")

    def policy(self, family="jammy-qt5", materials=None):
        return profiles_api.build_policy(self.catalog, self.profiles, family, self.root, role_materials=materials)

    @staticmethod
    def codeql_material():
        # Synthetic test content hash, never written into a production lock.
        return {"name": "codeql-bundle",
                "url": "https://github.com/github/codeql-action/releases/download/codeql-bundle-v2.23.4/codeql-bundle-linux64.tar.gz",
                "sha256": hashlib.sha256(b"synthetic CodeQL fixture bytes").hexdigest()}

    def test_six_concrete_families_and_required_local_profile_sets(self):
        ci.validate_catalog(self.catalog)
        self.assertEqual(set(self.catalog["families"]), set(EXPECTED))
        self.assertEqual(set(self.profiles["families"]), set(EXPECTED))
        for family, expected in EXPECTED.items():
            self.assertEqual(set(self.catalog["families"][family]["profiles"]), expected)
            self.assertEqual(set(self.profiles["families"][family]), expected)
        self.assertIsNone(profiles_api.validate_profiles(self.catalog, self.profiles, self.root))
        broken = copy.deepcopy(self.profiles)
        del broken["families"]["jammy-qt5"]["asan-lsan"]
        with self.assertRaises(ci.ContractError):
            profiles_api.validate_profiles(self.catalog, broken, self.root)

    def test_recipe_catalog_declares_tsan_copy_and_analysis_installer_sources(self):
        tsan = self.catalog["families"]["jammy-qt5-tsan"]
        self.assertEqual(set(tsan["recipe_files"]), {
            "docker/ubuntu22.04-tsan/Dockerfile", "docker/ubuntu22.04-tsan/apt_snapshot_retry.sh",
            "docker/ubuntu22.04-tsan/verify_elf_runtime_closure.sh",
            "docker/ubuntu22.04-tsan/patches/fix_qt5_qobject_tsan_publication.patch",
            "scripts/ci_install_locked_apt.sh", "docker/ubuntu22.04-tsan/README.md"})
        self.assertEqual(tsan.get("build_args", {}).get("KLOGG_APT_STAGE"), "locked")
        self.assertIn("scripts/verify_tsan_qt_sources.py",
                      self.profiles["families"]["jammy-qt5-tsan"]["tsan"]["verification_files"])
        analysis = self.catalog["families"][ANALYSIS]
        self.assertEqual(analysis["context"], "docker/ubuntu24.04-analysis")
        self.assertTrue({"scripts/ci_install_locked_apt.sh", "scripts/extract_verified_tar.py",
                         "scripts/prefetch_adb_helper_sources.py"} <= set(analysis["recipe_files"]))
        for family in self.catalog["families"].values():
            self.assertFalse(any("codeql" in path or path.startswith("src/") for path in family["recipe_files"]))

    def test_codeql_declares_its_compilation_census_input(self):
        config = profiles_api.profile_configuration(self.profiles, ANALYSIS, "codeql")
        self.assertIn("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", config["cmake_options"])

    def test_every_analysis_configure_is_explicitly_disconnected(self):
        for role in ("static", "coverage", "codeql"):
            config = profiles_api.profile_configuration(self.profiles, ANALYSIS, role)
            self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", config["cmake_options"])

    def test_policy_binds_qualification_implementation_not_consumer_migration_files(self):
        common = set(self.profiles["verification_files"])
        self.assertTrue({".github/workflows/ci-environments.yml", "scripts/ci_environment_pipeline.py"} <= common)
        linux = set(self.profiles["linux_defaults"]["verification_files"])
        self.assertIn(".github/actions/linux-validate/action.yml", linux)
        self.assertNotIn(".github/workflows/ci-build.yml", linux)
        for role in ("static", "coverage", "codeql"):
            files = set(self.profiles["families"][ANALYSIS][role]["verification_files"])
            self.assertTrue({"scripts/qualify_ci_analysis.py", "scripts/run_linux_analysis.sh"} <= files)
            self.assertNotIn(".github/actions/agent-setup/action.yml", files)
            self.assertFalse(any(path.startswith(".github/workflows/") for path in files))

    def test_extracted_source_transport_remains_bound_by_package_policies(self):
        relative = "scripts/ci_actions_transport.py"
        transport = self.root / relative
        transport.write_bytes(b"original transport fixture\n")
        targets = (("focal-qt5-gcc13", "appimage"), ("jammy-qt5", "deb"),
                   ("noble-qt6", "deb"), ("resolute-qt6", "deb"))
        before = {family: self.policy(family) for family, _ in targets}
        transport.write_bytes(b"changed transport fixture\n")
        for family, role in targets:
            with self.subTest(family=family):
                after = self.policy(family)
                self.assertTrue(before[family] != after,
                                "extracted transport bytes must affect qualification identity")
                self.assertIn(relative, after["profiles"][role]["verification_files"])

    def test_preparation_security_bytes_change_every_policy_not_recipe(self):
        paths = (
            "cmake/prepare_efsw_source.cmake",
            "cmake/verify_pinned_source.cmake",
            "scripts/prepare_efsw_git.py",
            "scripts/prepare_efsw_snapshot.py",
            "cmake/apply_git_patch_series.cmake",
            "3rdparty/CMakeLists.txt",
        )
        materials = {"codeql": [self.codeql_material()]}
        for relative in paths:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            original = path.read_bytes() if path.is_file() else b"preparation fixture\n"
            path.write_bytes(original)
            before = {family: self.policy(family, materials if family == ANALYSIS else None)
                      for family in EXPECTED}
            recipes = {family: ci.recipe_identity(self.catalog, family, self.root)
                       for family in EXPECTED}
            path.write_bytes(original + b"\n# changed preparation security boundary\n")
            for family in EXPECTED:
                with self.subTest(path=relative, family=family):
                    after = self.policy(family, materials if family == ANALYSIS else None)
                    self.assertNotEqual(ci.policy_identity(before[family]), ci.policy_identity(after),
                                        "preparation security bytes must invalidate qualification")
                    self.assertEqual(recipes[family], ci.recipe_identity(self.catalog, family, self.root))
                    for role in EXPECTED[family]:
                        self.assertIn(relative, after["profiles"][role]["verification_files"])
            path.write_bytes(original)

    def test_missing_recipe_is_an_explicit_prerequisite_not_a_fake_identity(self):
        missing = self.root / "docker/ubuntu24.04-analysis/Dockerfile"
        missing.unlink()
        with self.assertRaisesRegex(ci.ContractError, "docker/ubuntu24.04-analysis/Dockerfile"):
            profiles_api.validate_profiles(self.catalog, self.profiles, self.root)
        self.assertIsNone(profiles_api.validate_profiles(self.catalog, self.profiles, self.root, require_recipe_files=False))
        self.assertEqual(self.policy()["family"], "jammy-qt5")

    def test_current_linux_lane_and_shared_option_parity(self):
        workflow = (ROOT / ".github/workflows/ci-build.yml").read_text(encoding="utf-8")
        prepare = (ROOT / ".github/actions/prepare-workspace-env/action.yml").read_text(encoding="utf-8")
        defaults = re.search(r'KLOGG_CMAKE_OPTS=(.+?) \$\{\{ inputs.cmake-options', prepare).group(1)
        docker_observation = ["-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
        self.assertEqual(self.profiles["linux_defaults"]["cmake_options"], shlex.split(defaults) + docker_observation)
        lanes = [("focal-qt5-gcc13", "appimage", "ubuntu-20.04-appimage"),
                 ("jammy-qt5", "deb", "ubuntu-22.04-deb"),
                 ("jammy-qt5", "asan-lsan", "ubuntu-22.04-asan-lsan"),
                 ("jammy-qt5", "ubsan", "ubuntu-22.04-ubsan"),
                 ("noble-qt6", "deb", "ubuntu-24.04-deb"),
                 ("resolute-qt6", "deb", "ubuntu-26.04-deb")]
        for family, profile, label in lanes:
            with self.subTest(label=label):
                block = re.search(r"label: " + re.escape(label) + r"\n(.*?)(?=\n\s*- os:|\n\s*runs-on:)", workflow, re.S).group(1)
                config = self.profiles["families"][family][profile]["configuration"]
                options = re.search(r"cmake_opts: ([^\n]+)", block).group(1)
                self.assertEqual(config["cmake_options"], shlex.split(options))
                sanitizer = re.search(r"sanitizer: ([^\n]+)", block).group(1).strip('"')
                self.assertEqual(config["sanitizer"], sanitizer)
                self.assertEqual(config["package_settings"]["tag"], re.search(r"package_tag: ([^\n]+)", block).group(1))
                effective = profiles_api.profile_configuration(self.profiles, family, profile)["cmake_options"]
                expected = shlex.split(defaults) + docker_observation + shlex.split(options)
                if config["package"]:
                    expected += ["-DKLOGG_ADB_HELPER_REQUIRED=ON", "-DKLOGG_ADB_HELPER_TARGET=linux-x86_64",
                                 "-DKLOGG_ADB_HELPER_ARTIFACT_ROOT=@WORKSPACE@/prefetch_artifacts/adb-helper"]
                    self.assertEqual(config["package_settings"]["generator"], re.search(r"cpack_gen:([^\n]*)", block).group(1).strip())
                    self.assertEqual(config["package_settings"]["suffix"], re.search(r"package_suffix: ([^\n]+)", block).group(1))
                    self.assertEqual(config["package_settings"]["check_command"], re.search(r"check_command:([^\n]*)", block).group(1).strip())
                expected += ["-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
                             "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache", "-DCPM_SOURCE_CACHE=@WORKSPACE@/cpm_cache"]
                self.assertEqual(effective, expected)

    def test_tsan_exact_qt_compiler_and_runtime_prefix_without_ccache(self):
        workflow = (ROOT / ".github/workflows/ci-build.yml").read_text(encoding="utf-8")
        options = re.search(r"KLOGG_CONFIG_CMAKE_OPTS: (-DCMAKE_C_COMPILER=clang-14[^\n]+)", workflow).group(1)
        config = self.profiles["families"]["jammy-qt5-tsan"]["tsan"]["configuration"]
        self.assertEqual(config["cmake_options"], shlex.split(options))
        effective = profiles_api.profile_configuration(self.profiles, "jammy-qt5-tsan", "tsan")
        for option in ("-DKLOGG_TSAN_QT_VERSION=5.15.19", "-DCMAKE_C_COMPILER=clang-14", "-DCMAKE_CXX_COMPILER=clang++-14",
                       "-DCMAKE_PREFIX_PATH=/opt/qt5-tsan", "-DQt5_DIR=/opt/qt5-tsan/lib/cmake/Qt5"):
            self.assertIn(option, effective["cmake_options"])
        self.assertFalse(any("ccache" in option for option in effective["cmake_options"]))
        env = profiles_api.profile_environment(self.profiles, "jammy-qt5-tsan", "tsan")
        self.assertEqual(env["LD_LIBRARY_PATH"], "/opt/qt5-tsan/lib")
        self.assertEqual(env["TSAN_OPTIONS"], "halt_on_error=1:external_symbolizer_path=/usr/bin/llvm-symbolizer-14")
        self.assertEqual(env["LANG"], "C.UTF-8")
        self.assertNotIn("suppressions=", env["TSAN_OPTIONS"])

    def test_only_package_roles_require_adb_artifact_and_package_verifier(self):
        for family, profiles in self.profiles["families"].items():
            for name, profile in profiles.items():
                config = profiles_api.profile_configuration(self.profiles, family, name)
                wants_adb = "-DKLOGG_ADB_HELPER_REQUIRED=ON" in config["cmake_options"]
                self.assertEqual(wants_adb, config["package"])
                self.assertEqual("scripts/verify_adb_helper_artifact.py" in profile["verification_files"], config["package"])

    def test_adb_acquisition_helpers_bind_package_fixture_policy(self):
        acquisition = {
            "scripts/ci_environment_fixture.py",
            "scripts/ci_environment_source_cache.py",
            "scripts/prefetch_adb_helper_sources.py",
            "scripts/prefetch_adb_manifest_fallback.py",
            "scripts/prefetch_adb_source_context.py",
            "scripts/prefetch_adb_libusb_fallback.py",
            "scripts/prefetch_adb_source_closure.py",
        }
        for relative in acquisition:
            script = self.root / relative
            if not script.exists():
                script.parent.mkdir(parents=True, exist_ok=True)
                script.write_bytes((ROOT / relative).read_bytes())
        for family, roles in self.profiles["families"].items():
            for role, profile in roles.items():
                with self.subTest(family=family, role=role):
                    files = set(profile["verification_files"])
                    if profile["configuration"]["package"]:
                        self.assertEqual(acquisition - files, set())
                    elif family == ANALYSIS and role == "codeql":
                        self.assertEqual(acquisition & files, {"scripts/prefetch_adb_helper_sources.py"})
                    else:
                        self.assertTrue(acquisition.isdisjoint(files))
        before = ci.policy_identity(self.policy())
        recipe = ci.recipe_identity(self.catalog, "jammy-qt5", self.root)
        for relative in acquisition:
            with self.subTest(script=relative):
                script = self.root / relative
                original = script.read_bytes()
                try:
                    script.write_bytes(b"# revised acquisition policy\n")
                    self.assertNotEqual(before, ci.policy_identity(self.policy()))
                    self.assertEqual(recipe, ci.recipe_identity(self.catalog, "jammy-qt5", self.root))
                finally:
                    script.write_bytes(original)

    def test_sanitizer_runtime_environment_matches_current_workflow(self):
        workflow = (ROOT / ".github/workflows/ci-build.yml").read_text(encoding="utf-8")
        for profile, keys in (("asan-lsan", ("ASAN_OPTIONS", "LSAN_OPTIONS")), ("ubsan", ("UBSAN_OPTIONS",))):
            env = profiles_api.profile_environment(self.profiles, "jammy-qt5", profile)
            for key in keys:
                actual = re.search(r'echo "' + key + r'=([^"\n]+)"', workflow).group(1)
                self.assertEqual(env[key], actual)
            self.assertEqual({key for key in env if key in {"ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS"}}, set(keys))

    def test_analysis_runtime_paths_agree_with_materialized_image(self):
        dockerfile = (ROOT / "docker/ubuntu24.04-analysis/Dockerfile").read_text(encoding="utf-8")
        image_env = dict(re.findall(r"(?:^ENV |^\s+)([A-Z_][A-Z0-9_]*)=([^\s\\]+)", dockerfile, re.M))
        self.assertEqual(image_env["BOOST_ROOT"], "/opt/klogg-tools/boost")
        qt_root = "/opt/klogg-tools/qt/6.9.3/gcc_64"
        self.assertEqual(image_env["QT_ROOT_DIR"], qt_root)
        self.assertEqual(image_env["CMAKE_PREFIX_PATH"], qt_root)
        self.assertEqual(image_env["LD_LIBRARY_PATH"], qt_root + "/lib")
        self.assertEqual(image_env["QT_PLUGIN_PATH"], qt_root + "/plugins")
        for profile in ("static", "coverage", "codeql"):
            environment = profiles_api.profile_environment(self.profiles, ANALYSIS, profile, workspace="/__w/klogg/klogg")
            with self.subTest(profile=profile):
                self.assertEqual(environment["BOOST_ROOT"], image_env["BOOST_ROOT"])
                # Unspecified Qt paths remain inherited from the image. Any
                # explicit overrides must agree with its materialized tools.
                for key in set(environment) & set(image_env):
                    self.assertEqual(environment[key], image_env[key], key)

    def observe_analysis_configure(self, role, *, document=None, cache=None):
        """Run configure through a real subprocess and record the CMake boundary."""
        tools = self.root / "fake-tools"
        tools.mkdir(exist_ok=True)
        receipt = self.root / "configure-command.json"
        cmake = tools / "cmake"
        cmake.write_text(
            "#!" + sys.executable + "\n"
            "import json, os, pathlib, sys\n"
            "pathlib.Path(os.environ['CONFIGURE_RECEIPT']).write_text(json.dumps({"
            "'args': sys.argv[1:], 'cwd': os.getcwd()}))\n", encoding="utf-8")
        cmake.chmod(0o755)
        workspace = self.root / "private workspace"
        workspace.mkdir(exist_ok=True)
        build = workspace / "build"
        environment = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ.get("PATH", ""),
                           CONFIGURE_RECEIPT=str(receipt))
        kwargs = {} if cache is None else {"cpm_source_cache": cache}
        analysis.configure(role, workspace, build, subprocess.run, environment,
                           self.profiles if document is None else document, **kwargs)
        observed = json.loads(receipt.read_text())
        self.assertEqual(observed["cwd"], str(workspace.resolve()))
        self.assertEqual(observed["args"][:4], ["-S", str(workspace), "-B", str(build)])
        return observed["args"][4:], workspace

    def test_analysis_roles_execute_ordered_profile_options_with_runtime_workspace(self):
        for role in ("static", "coverage", "codeql"):
            with self.subTest(role=role):
                options, workspace = self.observe_analysis_configure(role)
                config = profiles_api.profile_configuration(self.profiles, ANALYSIS, role)
                self.assertEqual(options, [value.replace("@WORKSPACE@", str(workspace))
                                           for value in config["cmake_options"]])
                self.assertEqual(config["role"], role)
                self.assertFalse(config["package"])
                self.assertEqual(config["environment"]["KLOGG_QT_VERSION"], "6.9.3")
                self.assertIn("-DCPM_SOURCE_CACHE=" + str(workspace / "cpm_cache"), options)
                self.assertNotIn("/usr/local", " ".join(options))
                self.assertIn("@WORKSPACE@", " ".join(config["cmake_options"]))

    def test_configure_consumes_profile_order_not_a_second_inline_policy(self):
        document = copy.deepcopy(self.profiles)
        for role in ("static", "coverage", "codeql"):
            with self.subTest(role=role):
                config = document["families"][ANALYSIS][role]["configuration"]
                config["cmake_options"] += ["-DMODEL_ORDER=first", "-DMODEL_ORDER=last"]
                options, workspace = self.observe_analysis_configure(role, document=document)
                expected = profiles_api.profile_configuration(document, ANALYSIS, role)["cmake_options"]
                self.assertEqual(options, [value.replace("@WORKSPACE@", str(workspace)) for value in expected])
                self.assertLess(options.index("-DMODEL_ORDER=first"), options.index("-DMODEL_ORDER=last"))

    def test_coverage_cache_override_changes_only_the_authenticated_cache_path(self):
        original = copy.deepcopy(self.profiles)
        default, workspace = self.observe_analysis_configure("coverage")
        cache = self.root / "original checkout" / "authenticated cpm cache"
        actual, _ = self.observe_analysis_configure("coverage", cache=cache)
        expected = ["-DCPM_SOURCE_CACHE=" + str(cache) if value.startswith("-DCPM_SOURCE_CACHE=")
                    else value for value in default]
        self.assertEqual(actual, expected)
        self.assertEqual(sum(value.startswith("-DCPM_SOURCE_CACHE=") for value in actual), 1)
        self.assertNotIn("-DCPM_SOURCE_CACHE=" + str(workspace / "cpm_cache"), actual)
        self.assertEqual(self.profiles, original, "runtime paths must not mutate the qualification policy")

    def test_verifier_content_changes_policy_not_recipe_or_application_sha(self):
        recipe = ci.recipe_identity(self.catalog, "jammy-qt5", self.root)
        original = self.policy()
        self.assertEqual(original["schema_version"], 1)
        self.assertEqual(set(original["profiles"]), EXPECTED["jammy-qt5"])
        for value in original["profiles"].values():
            self.assertRegex(value["verification_files"]["scripts/ci_environment.py"], r"^sha256:[0-9a-f]{64}$")
        (self.root / "current-application-sha").write_text("a" * 40, encoding="ascii")
        self.assertEqual(recipe, ci.recipe_identity(self.catalog, "jammy-qt5", self.root))
        self.assertEqual(original, self.policy())
        verifier = self.root / "scripts/ci_environment.py"
        verifier.write_bytes(verifier.read_bytes() + b"\n# changed policy verifier\n")
        self.assertNotEqual(ci.policy_identity(original), ci.policy_identity(self.policy()))
        self.assertEqual(recipe, ci.recipe_identity(self.catalog, "jammy-qt5", self.root))

    def test_policy_requires_exact_official_locked_codeql_material_without_guessing(self):
        with self.assertRaisesRegex(ci.ContractError, "codeql-bundle"):
            self.policy(ANALYSIS)
        supplied = {"codeql": [self.codeql_material()]}
        policy = self.policy(ANALYSIS, supplied)
        self.assertEqual(policy["profiles"]["codeql"]["role_materials"], supplied["codeql"])
        self.assertEqual(policy["profiles"]["static"]["role_materials"], [])
        self.assertEqual(policy["profiles"]["coverage"]["role_materials"], [])
        for field, value in (("url", "https://example.invalid/codeql.tar.gz"),
                             ("url", "https://github.com/github/codeql-action/releases/latest/download/codeql-bundle-linux64.tar.gz"),
                             ("sha256", "0" * 64), ("sha256", ""), ("name", "unexpected")):
            bad = copy.deepcopy(supplied)
            bad["codeql"][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ci.ContractError):
                self.policy(ANALYSIS, bad)
        with self.assertRaises(ci.ContractError):
            self.policy(ANALYSIS, {"static": supplied["codeql"], "codeql": supplied["codeql"]})
        with self.assertRaises(ci.ContractError):
            self.policy(ANALYSIS, {"codeql": supplied["codeql"] * 2})

    def test_env_rendering_rejects_injection_and_quotes_shell_values(self):
        for workspace in ("/tmp/work\nINJECTED=1", "/tmp/work\rX=2", "relative", "/tmp/../escape", "/tmp/$(touch bad)"):
            with self.subTest(workspace=workspace), self.assertRaises(ci.ContractError):
                profiles_api.profile_environment(self.profiles, ANALYSIS, "static", workspace=workspace)
        for env in ({"GOOD": "bad\nINJECTED=1"}, {"GOOD": "bad\rINJECTED=1"}, {"bad-name": "value"}):
            with self.assertRaises(ci.ContractError):
                profiles_api.format_environment(env)
        rendered = profiles_api.format_environment({"SAFE": "$(printf injected); a b"}, style="shell")
        result = subprocess.run(["sh", "-c", rendered + '\nprintf "%s" "$SAFE"'], capture_output=True, text=True, check=False)
        self.assertEqual(result.stdout, "$(printf injected); a b")

    def test_policy_rejects_unsafe_missing_and_symlink_verification_paths(self):
        for path in ("../escape", "/absolute", "scripts\\verifier.py", "scripts/missing.py"):
            other = copy.deepcopy(self.profiles)
            other["verification_files"] = [path]
            with self.subTest(path=path), self.assertRaises(ci.ContractError):
                profiles_api.build_policy(self.catalog, other, "jammy-qt5", self.root)
        verifier = self.root / "scripts/ci_environment.py"
        verifier.unlink()
        verifier.symlink_to(self.root / "scripts/ci_environment_profiles.py")
        with self.assertRaises(ci.ContractError):
            self.policy()

    def test_cli_profile_env_and_catalog_check_have_explicit_output(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "profile-env", "--profiles", str(PROFILES),
                                 "--family", "jammy-qt5", "--profile", "asan-lsan", "--format", "github-env"],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("KLOGG_SANITIZER=address\n", result.stdout)
        self.assertIn("KLOGG_PACKAGE_ENABLED=false\n", result.stdout)
        result = subprocess.run([sys.executable, str(SCRIPT), "catalog-check", "--catalog", str(RECIPES),
                                 "--profiles", str(PROFILES), "--repo-root", str(self.root)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"families": 6', result.stdout)


if __name__ == "__main__":
    unittest.main()
