"""Ordinary ADB/iOS object caching must not become native-artifact reuse."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import lint_ci_quality as quality

SCRIPT = ROOT / "scripts/ci_native_object_cache.py"
ACTION = ROOT / ".github/actions/build-adb-helper/action.yml"


def load_module():
    spec = importlib.util.spec_from_file_location("native_cache", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def context(event="push", ref="refs/heads/master"):
    env = {"GITHUB_EVENT_NAME": event, "GITHUB_REF": ref,
           "GITHUB_REPOSITORY": "ZEACENT/klogg", "GITHUB_REPOSITORY_ID": "123"}
    payload = {"repository": {"id": 123, "full_name": "ZEACENT/klogg"}}
    if event == "pull_request":
        payload.update(number=42, pull_request={"number": 42, "head": {
            "repo": {"id": 123, "full_name": "ZEACENT/klogg"}}})
    return env, payload


class NativeCacheActionTest(unittest.TestCase):
    def assert_cache_contract(self, text):
        inputs = quality.workflow_mapping_block(text.splitlines(), "inputs", 0)
        self.assertIsNotNone(inputs)
        self.assertIn("object-cache-enabled", inputs)
        option = quality.workflow_mapping_block(inputs["object-cache-enabled"][1], "object-cache-enabled", 2)
        self.assertIsNotNone(option)
        self.assertEqual(option["default"][0], "false")
        steps = [quality.workflow_step_fields(step) for step in quality.workflow_step_blocks(text.splitlines())]
        names = [fields.get("name") for fields, _ in steps]
        save = next(i for i, (fields, _) in enumerate(steps)
                    if fields.get("uses") == "./.github/actions/save-verified-compiler-cache")
        self.assertGreater(save, names.index("Generate SHA256SUMS for target-bound artifact"))
        self.assertEqual(steps[save][0]["if"], "${{ success() && steps.native-cache-finish.outputs.validated == 'true' && steps.native-cache.outputs.save-key != '' }}")
        self.assertEqual(steps[save][1]["with"], {"path": "${{ steps.native-cache.outputs.path }}",
                                                "key": "${{ steps.native-cache.outputs.save-key }}",
                                                "descriptor": "${{ steps.native-cache-descriptor.outputs.descriptor }}"})
        mac = next((fields, children) for fields, children in steps
                   if fields.get("name") == "Build native macOS helper from prefetched sources")
        self.assertIn("CMAKE_C_COMPILER_LAUNCHER", mac[1]["env"])
        self.assertIn("CMAKE_CXX_COMPILER_LAUNCHER", mac[1]["env"])
        self.assertEqual(mac[0]["run"].count("scripts/build_adb_helper.py"), 1)
        by_id = {fields["id"]: fields for fields, _ in steps if fields.get("id")}
        self.assertEqual(by_id["native-cache"]["if"], "${{ inputs.object-cache-enabled == 'true' && steps.native-cache-eligibility.outputs.eligible == 'true' }}")
        self.assertEqual(by_id["native-cache-eligibility"]["run"],
                         'python3 scripts/ci_native_object_cache.py eligible --target "$KLOGG_NATIVE_CACHE_TARGET"')
        self.assertEqual(by_id["native-cache-master"]["if"], "${{ steps.native-cache.outputs.own-pr == 'true' && steps.native-cache-primary.outputs.cache-matched-key == '' }}")

    def test_workflow_opts_in_only_ordinary_macos_helpers(self):
        workflow = (ROOT / ".github/workflows/ci-build.yml").read_text()
        jobs = quality.workflow_job_steps(workflow)
        expected = ("${{ startsWith(env.KLOGG_ADB_HELPER_TARGET, 'macos-') && "
                    "(github.event_name != 'workflow_dispatch' || "
                    "(inputs.environment-mode == 'off' && inputs.dependency-mode == 'off')) }}")
        for job in ("BuildAdbLinuxX64", "BuildAdbLinuxArm64", "BuildAdbWindowsX64",
                    "BuildAdbMacX64", "BuildAdbMacArm64"):
            with self.subTest(job=job):
                action = next(children["with"] for fields, children in
                              (quality.workflow_step_fields(step) for step in jobs[job])
                              if fields.get("uses") == "./.github/actions/build-adb-helper")
                self.assertEqual(action.get("object-cache-enabled"), expected)

    def assert_ios_cache_contract(self, text):
        jobs = quality.workflow_job_steps(text)
        for job in ("BuildIosNativeX64", "BuildIosNativeArm64"):
            steps = [quality.workflow_step_fields(step) for step in jobs[job]]
            fields = {field["id"]: (field, children) for field, children in steps if field.get("id")}
            names = [field.get("name") for field, _ in steps]
            self.assertIn("--component ios-native", fields["ios-cache"][0]["run"])
            self.assertIn("dependency-mode == 'off'", fields["ios-cache-eligibility"][0]["if"])
            self.assertIn("--native-identity", fields["ios-cache-descriptor"][0]["run"])
            self.assertEqual(fields["ios-cache-shared"][0]["if"],
                             "${{ steps.ios-cache-descriptor.outputs.eligible == 'true' }}")
            build = next(field for field, _ in steps if field.get("name") == "Build disconnected thin iOS native stack")
            for command in ("scripts/build_ios_native_stack.py", "scripts/verify_ios_native_stack.py",
                            "scripts/build_ios_native_legal_assets.py", "--compiler-launcher"):
                self.assertIn(command, build["run"])
            self.assertLess(names.index("Bind iOS native artifact with SHA256SUMS"),
                            names.index("Measure and bound verified iOS compiler objects"))
            exported = next((field, children) for field, children in steps
                            if field.get("name") == "Export shared iOS compiler objects")
            self.assertEqual(exported[0]["if"], "${{ success() && steps.ios-cache-finish.outputs.validated == 'true' && steps.ios-cache-descriptor.outputs.eligible == 'true' }}")
            self.assertEqual(exported[1]["with"]["path"], "${{ steps.ios-cache.outputs.path }}")
            self.assertEqual(exported[1]["with"]["descriptor"], "${{ steps.ios-cache-descriptor.outputs.descriptor }}")
            saved = next(children["with"] for field, children in steps
                         if field.get("name") == "Save verified scoped iOS compiler objects")
            self.assertEqual(saved["descriptor"], "${{ steps.ios-cache-descriptor.outputs.descriptor }}")

    def test_ios_anchor_caches_only_objects_and_retains_all_verification(self):
        self.assert_ios_cache_contract((ROOT / ".github/workflows/ci-build.yml").read_text())

    def test_ios_workflow_mutations_cannot_bypass_scope_or_export_native_bundles(self):
        text = (ROOT / ".github/workflows/ci-build.yml").read_text()
        for mutated in (
                text.replace("--component ios-native", "--component adb-helper"),
                text.replace("inputs.dependency-mode == 'off'", "inputs.dependency-mode != 'qualify'"),
                text.replace("id: ios-cache-descriptor", "id: unknown-ios-descriptor"),
                text.replace("success() && steps.ios-cache-finish.outputs.validated", "always() && steps.ios-cache-finish.outputs.validated"),
                text.replace("path: ${{ steps.ios-cache.outputs.path }}", "path: ${{ runner.temp }}/ios-native-stack")):
            with self.subTest(mutated=mutated[:40]), self.assertRaises((AssertionError, KeyError)):
                self.assert_ios_cache_contract(mutated)

    @unittest.skipUnless(Path("/bin/bash").is_file(), "requires Bash workflow shell")
    def test_ios_uncached_build_options_are_safe_under_bash_nounset(self):
        jobs = quality.workflow_job_steps((ROOT / ".github/workflows/ci-build.yml").read_text())
        build = next(fields for fields, _ in (quality.workflow_step_fields(step) for step in jobs["BuildIosNativeX64"])
                     if fields.get("name") == "Build disconnected thin iOS native stack")
        setup = build["run"].split("original_root=", 1)[0]
        # sysctl is not available on all CI hosts; its value is not the behavior under test.
        setup = setup.replace('$(sysctl -n hw.ncpu)', '2')
        result = subprocess.run(["/bin/bash", "-c", setup + '\nprintf "%s\\n" "${launcher_args[@]}"'],
                                env={**os.environ, "KLOGG_IOS_CACHE_STATE": "", "KLOGG_IOS_COMPILER_LAUNCHER": ""},
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--parallel", result.stdout)
        self.assertNotIn("--compiler-launcher", result.stdout)

    def test_adb_bridge_uses_outer_job_step_and_preserves_verified_save_order(self):
        steps = [quality.workflow_step_fields(step) for step in quality.workflow_step_blocks(ACTION.read_text().splitlines())]
        fields = {field["id"]: (field, children) for field, children in steps if field.get("id")}
        descriptor = fields["native-cache-descriptor"][0]["run"]
        self.assertIn('--native-identity "$KLOGG_NATIVE_CACHE_IDENTITY"', descriptor)
        self.assertIn('--required-step "Build and verify native ADB helper"', descriptor)
        self.assertIn('--export-step "Build and verify native ADB helper"', descriptor)
        self.assertEqual(fields["native-cache-shared"][0]["if"], "${{ steps.native-cache-descriptor.outputs.eligible == 'true' }}")
        exported = next((field, children) for field, children in steps
                        if field.get("name") == "Export shared native ADB compiler objects")
        self.assertEqual(exported[0]["if"], "${{ success() && steps.native-cache-finish.outputs.validated == 'true' && steps.native-cache-descriptor.outputs.eligible == 'true' }}")
        self.assertEqual(exported[1]["with"]["path"], "${{ steps.native-cache.outputs.path }}")
        self.assert_cache_contract(ACTION.read_text())

    def test_cache_is_opt_in_and_save_follows_all_existing_verification(self):
        self.assert_cache_contract(ACTION.read_text())

    def test_real_action_mutations_cannot_enable_or_promote_unverified_cache(self):
        text = ACTION.read_text()
        for changed in (text.replace('default: "false"', 'default: "true"'),
                        text.replace("success() && steps.native-cache-finish", "always() && steps.native-cache-finish"),
                        text.replace("key: ${{ steps.native-cache.outputs.save-key }}", "key: ${{ steps.native-cache.outputs.master-key }}"),
                        text.replace("CMAKE_CXX_COMPILER_LAUNCHER:", "# CMAKE_CXX_COMPILER_LAUNCHER:"),
                        text.replace("inputs.object-cache-enabled == 'true' && steps.native-cache-eligibility", "unknown() && steps.native-cache-eligibility"),
                        text.replace('inputs:\n', 'inputs: malformed\n')):
            with self.subTest(changed=changed[:40]), self.assertRaises(AssertionError):
                self.assert_cache_contract(changed)


class NativeCachePolicyTest(unittest.TestCase):
    def setUp(self):
        self.cache = load_module()

    def test_ios_component_has_separate_architecture_bound_ordinary_namespace(self):
        env, payload = context()
        adb = self.cache.cache_keys(env, payload, "macos-x86_64", "off", "a" * 64, "2026-10-18")
        ios = self.cache.cache_keys(env, payload, "x86_64", "off", "a" * 64, "2026-10-18",
                                    component="ios-native")
        self.assertTrue(ios["primary-key"].startswith("native-ios-ccache-v1-x86_64-"))
        self.assertNotEqual(ios["primary-key"], adb["primary-key"])
        for target, component in (("macos-x86_64", "ios-native"), ("x86_64", "adb-helper"),
                                  ("x86_64", "unknown")):
            with self.subTest(target=target, component=component), self.assertRaises(ValueError):
                self.cache.cache_keys(env, payload, target, "off", "a" * 64, "2026-10-18",
                                      component=component)
        for mode in ("qualify", "publish", "observe-ios-host"):
            env, payload = context("workflow_dispatch")
            payload["inputs"] = {"dependency-mode": mode}
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                self.cache.cache_keys(env, payload, "x86_64", "off", "a" * 64, "2026-10-18",
                                      component="ios-native")

    def test_prepare_exports_stable_identity_separately_from_ephemeral_state(self):
        env, payload = context()
        identity = {"core": "a" * 64, "compiler_sha256": "b" * 64, "component": "ios-native"}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            event = root / "event.json"
            event.write_text(json.dumps(payload))
            env.update(GITHUB_EVENT_PATH=str(event), RUNNER_TEMP=str(root),
                       GITHUB_RUN_ID="123", GITHUB_SHA="unrelated-app-revision")
            with mock.patch.dict(os.environ, env, clear=True), \
                    mock.patch.object(self.cache.platform, "system", return_value="Darwin"), \
                    mock.patch.object(self.cache.platform, "machine", return_value="x86_64"), \
                    mock.patch.object(self.cache.shutil, "which", return_value="/usr/local/bin/ccache"), \
                    mock.patch.object(self.cache, "compatibility_identity", return_value=identity), \
                    mock.patch.object(self.cache, "emit") as emit, contextlib.redirect_stdout(io.StringIO()):
                self.cache.prepare("x86_64", "ios-native")
            outputs = emit.call_args.args[0]
            persisted = json.loads(Path(outputs["identity-file"]).read_text())
            self.assertEqual(persisted, identity)
            self.assertEqual(outputs["compatibility-fingerprint"], self.cache.fingerprint(persisted))
            self.assertEqual(outputs["compiler-launcher"], "/usr/local/bin/ccache")
            self.assertNotIn("unrelated-app-revision", Path(outputs["identity-file"]).read_text())
            self.assertNotEqual(Path(outputs["identity-file"]).parent, Path(outputs["path"]))

    def test_exec_uses_trusted_environment_without_changing_compiler_command(self):
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "state.json"
            settings = self.cache.cache_environment(Path(directory) / "objects",
                                                     Path(directory) / "trusted.conf", directory)
            state = {"run": self.cache.run_identity(), "environment": settings}
            state_path.write_text(json.dumps(state))
            argv = [sys.executable, "builder.py", "--compiler-launcher", "/usr/local/bin/ccache"]
            with mock.patch.object(self.cache.subprocess, "run") as run, mock.patch.dict(
                    os.environ, {"CCACHE_COMPILERCHECK": "none", "CCACHE_PREFIX": "untrusted"}):
                run.return_value.returncode = 0
                self.cache.execute(state_path, argv)
            run.assert_called_once()
            self.assertEqual(run.call_args.args[0], argv)
            environment = run.call_args.kwargs["env"]
            self.assertEqual(environment["CCACHE_COMPILERCHECK"], "content")
            self.assertEqual(environment["CCACHE_PREFIX"], "")
            self.assertNotIn("CMAKE_C_COMPILER_LAUNCHER", settings)
            state["run"]["GITHUB_RUN_ID"] = "stale"
            state_path.write_text(json.dumps(state))
            with self.assertRaises(ValueError):
                self.cache.execute(state_path, argv)

    def test_relocated_cmake_and_ninja_do_not_fragment_content_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            first, second = Path(directory) / "first.exe", Path(directory) / "second.exe"
            first.write_bytes(b"same compiler tool bytes")
            second.write_bytes(first.read_bytes())
            first.chmod(0o755)
            second.chmod(0o755)
            with mock.patch.object(self.cache, "command", return_value="tool 1.0"):
                self.assertEqual(self.cache.tool_content_identity(str(first)),
                                 self.cache.tool_content_identity(str(second)))
                second.write_bytes(b"changed compiler tool bytes")
                self.assertNotEqual(self.cache.tool_content_identity(str(first)),
                                    self.cache.tool_content_identity(str(second)))

    def test_both_component_identities_ignore_tool_relocation_and_application_revision(self):
        stable = {"path": "/stable/tool", "resolved_path": "/stable/tool",
                  "sha256": "a" * 64, "version": "tool 1.0"}
        location = ["/runner/temp/first"]

        def tool(name):
            return {**stable, "path": location[0] + "/" + name,
                    "resolved_path": location[0] + "/" + name} if name in ("cmake", "ninja") else stable

        for component, target in (("adb-helper", "macos-x86_64"), ("ios-native", "x86_64")):
            with self.subTest(component=component), \
                    mock.patch.object(self.cache, "command", return_value="stable tool version"), \
                    mock.patch.object(self.cache, "digest", return_value="b" * 64) as digest, \
                    mock.patch.object(self.cache, "tool_identity", side_effect=tool), \
                    mock.patch.object(self.cache, "observe_ios_host_tools", return_value={
                        "tools": {"autoconf": stable}, "homebrew": {"autoconf": "2.72"}}):
                location[0] = "/runner/temp/first"
                before = self.cache.compatibility_identity(component, target, "/usr/bin/ccache")
                location[0] = "/runner/temp/second"
                with mock.patch.dict(os.environ, {"GITHUB_SHA": "other", "GITHUB_REF": "refs/heads/master"}):
                    after = self.cache.compatibility_identity(component, target, "/usr/bin/ccache")
                self.assertEqual(self.cache.fingerprint(before), self.cache.fingerprint(after))
                digest.return_value = "c" * 64
                changed = self.cache.compatibility_identity(component, target, "/usr/bin/ccache")
                self.assertNotEqual(self.cache.fingerprint(before), self.cache.fingerprint(changed))

    def test_ios_observed_identity_tracks_tools_without_run_or_source_revision(self):
        tool = {"path": "/usr/bin/tool", "resolved_path": "/usr/bin/tool",
                "sha256": "b" * 64, "version": "tool 1.0"}
        with mock.patch.object(self.cache, "command", return_value="tool 1.0"), \
                mock.patch.object(self.cache, "digest", return_value="c" * 64), \
                mock.patch.object(self.cache, "observe_ios_host_tools", return_value={
                    "tools": {"autoconf": tool}, "homebrew": {"autoconf": "autoconf 2.72"},
                    "runner_image": {"version": "unrelated-image"}}), \
                mock.patch.object(self.cache, "tool_identity", return_value=tool):
            identity = self.cache.compatibility_identity("ios-native", "x86_64", "/usr/bin/ccache")
        self.assertIn("host_tools", identity)
        self.assertIn("make", identity["host_tools"])
        self.assertNotIn("runner_image", identity)
        for forbidden in ("GITHUB_SHA", "GITHUB_REF", "GITHUB_RUN_ID", "unrelated-image"):
            self.assertNotIn(forbidden, json.dumps(identity))
        original = self.cache.fingerprint(identity)
        identity["host_tools"]["make"] = {**tool, "sha256": "d" * 64}
        self.assertNotEqual(original, self.cache.fingerprint(identity))

    def test_master_and_own_pr_namespaces_never_promote_pr_objects(self):
        env, payload = context()
        master = self.cache.cache_keys(env, payload, "macos-x86_64", "off", "a" * 64, "2026-10-18")
        env, payload = context("pull_request", "refs/pull/42/merge")
        pr = self.cache.cache_keys(env, payload, "macos-x86_64", "off", "a" * 64, "2026-10-18")
        self.assertEqual(master["primary-key"], pr["master-key"])
        self.assertIn("-pr-42-", pr["save-key"])
        self.assertNotEqual(master["save-key"], pr["save-key"])
        env, payload = context("workflow_dispatch", "refs/heads/master")
        self.assertEqual(self.cache.cache_keys(env, payload, "macos-x86_64", "off", "a" * 64,
                                               "2026-10-18")["save-key"], "")

    def test_forks_candidate_modes_and_spoofed_contexts_are_rejected(self):
        cases = []
        for mode in ("qualify", "publish", "observe-ios-host"):
            env, payload = context("workflow_dispatch")
            payload["inputs"] = {"dependency-mode": mode}
            cases.append((env, payload, "macos-x86_64", "off"))
        for field, value in (("id", 456), ("full_name", "fork/klogg")):
            env, payload = context("pull_request", "refs/pull/42/merge")
            payload["pull_request"]["head"]["repo"][field] = value
            cases.append((env, payload, "macos-x86_64", "off"))
        for event, ref, target, mode in (("push", "refs/heads/topic", "macos-arm64", "off"),
                                        ("schedule", "refs/heads/master", "macos-arm64", "off"),
                                        ("push", "refs/heads/master", "linux-x86_64", "off"),
                                        ("push", "refs/heads/master", "macos-arm64", "qualify")):
            env, payload = context(event, ref)
            cases.append((env, payload, target, mode))
        env, payload = context("pull_request", "refs/pull/43/merge")
        cases.append((env, payload, "macos-x86_64", "off"))
        for args in cases:
            with self.subTest(args=args), self.assertRaises(ValueError):
                self.cache.cache_keys(*args, "a" * 64, "2026-10-18")

    def test_malformed_events_and_environment_producer_modes_fail_closed(self):
        env, payload = context("pull_request", "refs/pull/42/merge")
        for malformed in (None, [], {}, {"repository": None}, {**payload, "pull_request": None},
                          {**payload, "pull_request": {"head": {"repo": None}}}):
            with self.subTest(event=malformed), self.assertRaises(ValueError):
                self.cache.cache_keys(env, malformed, "macos-arm64", "off", "a" * 64, "2026-10-18")
        env, payload = context("workflow_dispatch")
        payload["inputs"] = {"environment-mode": "qualify", "dependency-mode": "off"}
        with self.assertRaises(ValueError):
            self.cache.cache_keys(env, payload, "macos-arm64", "off", "a" * 64, "2026-10-18")

    def test_restored_config_cannot_override_bound_budget_or_compiler_check(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache = root / "objects"
            cache.mkdir()
            (cache / "ccache.conf").write_text("max_size = 5G\ncompiler_check = none\n")
            config = root / "trusted.conf"
            config.write_text("compiler_check = content\n")
            settings = self.cache.cache_environment(cache, config, root)
            self.assertEqual(settings["CCACHE_MAXSIZE"], "128M")
            self.assertEqual(settings["CCACHE_COMPILERCHECK"], "content")
            self.assertEqual(settings["CCACHE_CONFIGPATH"], str(config))
            with self.assertRaises(ValueError):
                self.cache.launcher("ccache", {"CCACHE_DIR": "/tmp/objects;bad"})
            link = root / "link"
            link.symlink_to(cache, target_is_directory=True)
            with self.assertRaises(ValueError):
                self.cache.occupancy(link)

    def test_identity_tracks_compiler_sdk_recipe_not_application_revision(self):
        identity = {"core": "a" * 64, "compiler": "b" * 64, "sdk": "26.6", "cache": "4.13"}
        original = self.cache.fingerprint(identity)
        for field in identity:
            changed = {**identity, field: "different"}
            self.assertNotEqual(original, self.cache.fingerprint(changed))
        env, payload = context()
        key = self.cache.cache_keys(env, payload, "macos-arm64", "off", original, "2026-10-18")
        env["GITHUB_SHA"] = "changed-application-version"
        self.assertEqual(key, self.cache.cache_keys(env, payload, "macos-arm64", "off", original,
                                                   "2026-10-18"))

    @unittest.skipUnless(all(shutil.which(tool) for tool in ("ccache", "clang", "cmake", "ninja")),
                         "native cache probe tools unavailable")
    def test_real_nested_externalprojects_cold_three_warm_and_header_invalidation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cache = root / "objects"
            config = root / "ccache.conf"
            config.write_text("max_size = 128M\ncompiler_check = content\ncompression = true\n")
            env = {**os.environ, **self.cache.cache_environment(cache, config, root)}
            launcher = self.cache.launcher(shutil.which("ccache"), env)
            env.update(CMAKE_C_COMPILER_LAUNCHER=launcher, CMAKE_CXX_COMPILER_LAUNCHER=launcher)
            source = root / "source"
            child = source / "child"
            child.mkdir(parents=True)
            (child / "value.h").write_text("#define VALUE 0\n")
            preamble = '#include "value.h"\n#ifndef EXTRA\n#define EXTRA 0\n#endif\n'
            (child / "probe.c").write_text(preamble + 'int main(void) { return VALUE + EXTRA; }\n')
            (child / "probe.cpp").write_text(preamble + 'int main() { return VALUE + EXTRA; }\n')
            (child / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.20)\n"
                "project(child C CXX)\nadd_executable(probe probe.c)\nadd_executable(cpp_probe probe.cpp)\n")
            (source / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.20)\n"
                "project(parent NONE)\ninclude(ExternalProject)\n"
                "ExternalProject_Add(child SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/child "
                "CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env PROBE=1 ${CMAKE_COMMAND} "
                "-S <SOURCE_DIR> -B <BINARY_DIR> -G Ninja "
                f"-DCMAKE_C_COMPILER={shutil.which('clang')} -DCMAKE_CXX_COMPILER={shutil.which('clang++')} "
                "BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> INSTALL_COMMAND \"\")\n"
                "ExternalProject_Add(ordinary SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/child "
                "CMAKE_GENERATOR Ninja INSTALL_COMMAND \"\")\n")

            def run(command):
                return subprocess.run(command, env=env, check=True, text=True, capture_output=True).stdout

            state_path = root / "state.json"
            state_path.write_text(json.dumps({"run": self.cache.run_identity(), "target": "probe",
                "identity": {"probe": True}, "keys": {}, "executable": shutil.which("ccache"),
                "environment": self.cache.cache_environment(cache, config, root), "cache": str(cache)}))
            self.cache.update(state_path, "start")
            stats = []
            for number in range(6):
                if number == 4:
                    (child / "value.h").write_text("#define VALUE 1\n")
                if number == 5:
                    env.update(CFLAGS="-DEXTRA=1", CXXFLAGS="-DEXTRA=1")
                build = root / "build"
                shutil.rmtree(build, ignore_errors=True)
                run(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja"])
                run(["cmake", "--build", str(build)])
                binary = build / "child-prefix/src/child-build/probe"
                result = subprocess.run([str(binary)], env=env, check=False)
                self.assertEqual(result.returncode, max(0, number - 3))
                stats.append(self.cache.statistics(shutil.which("ccache"), env))
            self.assertGreater(stats[0]["cache_miss"], 0)
            for index in range(1, 4):
                self.assertGreater(stats[index].get("direct_cache_hit", 0) + stats[index].get("preprocessed_cache_hit", 0),
                                   stats[index - 1].get("direct_cache_hit", 0) + stats[index - 1].get("preprocessed_cache_hit", 0))
                self.assertEqual(stats[index]["cache_miss"], stats[0]["cache_miss"])
            self.assertGreater(stats[4]["cache_miss"], stats[3]["cache_miss"])
            self.assertGreater(stats[5]["cache_miss"], stats[4]["cache_miss"])
            self.assertTrue(cache.is_dir())
            self.assertNotIn(root / "build", cache.parents)
            with contextlib.redirect_stdout(io.StringIO()):
                self.cache.update(state_path, "finish")
            report = json.loads((root / "report.json").read_text())
            self.assertEqual(report["budget_bytes"], self.cache.BUDGET)
            self.assertGreater(report["cache_deltas"]["cache_miss"], 0)
            self.assertLessEqual(report["occupancy_after_cleanup_bytes"], self.cache.BUDGET)
            state = json.loads(state_path.read_text())
            state["run"]["GITHUB_RUN_ID"] = "stale"
            state_path.write_text(json.dumps(state))
            with self.assertRaises(ValueError):
                self.cache.update(state_path, "finish")


if __name__ == "__main__":
    unittest.main()
