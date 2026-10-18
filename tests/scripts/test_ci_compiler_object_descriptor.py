import json
import os
from pathlib import Path
import sys
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
try:
    import ci_compiler_object_descriptor as descriptor
except ModuleNotFoundError:
    descriptor = None


class CompilerDescriptorTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(descriptor, "runtime compiler descriptor generation is not implemented")
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for name in descriptor.MATERIAL_FILES:
            file = self.root / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text("pinned recipe\n")
        self.compiler = self.root / "clang"
        self.compiler.write_bytes(b"pinned compiler")
        self.qmake = self.root / "qmake"
        self.qmake.write_bytes(b"pinned qmake")
        self.env = {"GITHUB_WORKSPACE": str(self.root), "KLOGG_WORKSPACE": str(self.root),
                    "KLOGG_LABEL": "intel-qt6", "KLOGG_CONFIG_ARCH": "x64",
                    "KLOGG_CONFIG_OS": "macos-26-intel", "KLOGG_QT_VERSION": "6.9.3",
                    "KLOGG_QT_ARCH": "clang_64", "KLOGG_QT_MODULES": "qt5compat",
                    "KLOGG_QT_ARCHIVES": "qtbase", "KLOGG_CONFIG_PACKAGE_TAG": "vs-gen",
                    "KLOGG_CMAKE_OPTS": "-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DROOT=" + str(self.root),
                    "KLOGG_CONFIG_CMAKE_OPTS": "-DKLOGG_USE_LTO=ON", "RUNNER_OS": "macOS",
                    "GITHUB_RUN_ID": "101", "GITHUB_RUN_ATTEMPT": "1", "GITHUB_SHA": "a" * 40}
        self.tree = "b" * 40
        self.native_identity = {"core": "a" * 64, "compiler_sha256": "b" * 64,
                                "compiler": "clang", "sdk": "sdk", "sdk_version": "26.0",
                                "xcode": "26.6", "cmake": "3.31.6", "ninja": "1.12.1",
                                "ccache": "4.13", "compiler_check": "content"}
        self.calls = []

    def probe(self, arguments, *, cwd=None):
        self.calls.append(arguments)
        if arguments[:2] == ["git", "rev-parse"]:
            return self.tree
        if arguments[:2] == ["git", "diff"]:
            return ""
        if arguments[:2] == ["xcrun", "--find"]:
            return str(self.compiler)
        if arguments == ["xcrun", "--show-sdk-path"]:
            return "/Applications/Xcode.app/SDKs/MacOSX.sdk"
        if arguments == ["xcrun", "--show-sdk-version"]:
            return "26.0"
        if arguments == ["xcodebuild", "-version"]:
            return "Xcode 26.6"
        if arguments[-2:] == ["-query", "QT_VERSION"]:
            return self.env["KLOGG_QT_VERSION"]
        if arguments[-1] == "--version":
            return "pinned tool 1.0"
        self.fail("unexpected probe " + repr(arguments))

    def make(self, **kwargs):
        env = kwargs.pop("env", self.env)
        with mock.patch.object(descriptor.shutil, "which", return_value=str(self.qmake)):
            return descriptor.make_descriptor(
                kwargs.pop("component", "application"), kwargs.pop("label", env["KLOGG_LABEL"]),
                "Application lane", ["Build", "Tests", "Package"], "Export compiler objects",
                root=self.root, env=env, probe=self.probe, **kwargs)

    def test_both_runtime_clis_support_isolated_python_outside_the_checkout(self):
        # CWD modules must never become an import escape hatch for -I.
        (self.root / "ci_ccache.py").write_text("raise RuntimeError('untrusted CWD import')\n")
        for script in ("ci_compiler_object_descriptor.py", "ci_compiler_object_transport.py"):
            with self.subTest(script=script):
                result = subprocess.run([sys.executable, "-I", str(ROOT / "scripts" / script), "--help"],
                                        cwd=self.root, capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("usage:", result.stdout)

    def test_macos_descriptor_binds_actual_tools_flags_qt_and_dependency_trees(self):
        result = self.make()
        self.assertEqual(result["kind"], "ccache")
        self.assertEqual(result["budget_bytes"], 500_000_000)
        first = descriptor.transport.fingerprint(result["compatibility"])
        self.env["KLOGG_CMAKE_OPTS"] += " -DENABLE_SANITIZER_ADDRESS=ON"
        self.assertNotEqual(first, descriptor.transport.fingerprint(self.make()["compatibility"]))
        self.env["KLOGG_CMAKE_OPTS"] = self.env["KLOGG_CMAKE_OPTS"].replace(" -DENABLE_SANITIZER_ADDRESS=ON", "")
        self.tree = "c" * 40
        self.assertNotEqual(first, descriptor.transport.fingerprint(self.make()["compatibility"]))

    def test_application_source_revision_and_workspace_location_are_not_compatibility_keys(self):
        first = self.make()["compatibility"]
        source = self.root / "src/app.cpp"
        source.parent.mkdir()
        source.write_text("changed application source")
        self.env["GITHUB_SHA"] = "d" * 40
        self.env["GITHUB_WORKSPACE"] = self.env["KLOGG_WORKSPACE"] = "/another/checkout"
        self.env["KLOGG_CMAKE_OPTS"] = self.env["KLOGG_CMAKE_OPTS"].replace(str(self.root), "/another/checkout")
        self.assertEqual(first, self.make()["compatibility"])

    def test_compiler_bytes_and_qt_version_changes_invalidate(self):
        first = self.make()["compatibility"]
        self.compiler.write_bytes(b"new compiler")
        self.assertNotEqual(first, self.make()["compatibility"])
        self.compiler.write_bytes(b"pinned compiler")
        self.env["KLOGG_QT_VERSION"] = "6.10.2"
        self.assertNotEqual(first, self.make()["compatibility"])

    def test_linux_uses_digest_qualified_image_not_host_compiler(self):
        self.env.update(RUNNER_OS="Linux", KLOGG_LABEL="ubuntu-24.04-deb", KLOGG_CONFIG_OS="ubuntu_noble")
        image = "ghcr.io/zeacent/klogg@sha256:" + "a" * 64
        first = self.make(image=image)
        self.assertEqual(first["compatibility"]["toolchain"]["image"], image)
        self.assertFalse(any(command[0] == "xcrun" for command in self.calls))
        self.assertNotEqual(first["compatibility"], self.make(image=image.replace("a" * 64, "b" * 64))["compatibility"])
        with self.assertRaisesRegex(ValueError, "digest"):
            self.make(image="ubuntu:latest")

    def test_windows_reuses_current_validated_state_identity_and_existing_budget(self):
        self.env.update(RUNNER_OS="Windows", KLOGG_LABEL="x64-qt6")
        state = {"ready": True, "run": {key: self.env[key] for key in ("GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_SHA")},
                 "identity": {"compiler_sha256": "c" * 64, "cmake_options": "-DROOT=" + str(self.root)},
                 "budget": 2 * 1024 * 1024 * 1024}
        path = self.root / "windows.json"
        path.write_text(json.dumps(state))
        result = self.make(windows_state=path)
        self.assertEqual(result["kind"], "sccache")
        self.assertEqual(result["budget_bytes"], state["budget"])
        self.assertEqual(result["compatibility"]["toolchain"]["compiler_sha256"], "c" * 64)
        state["run"]["GITHUB_RUN_ATTEMPT"] = "2"
        path.write_text(json.dumps(state))
        with self.assertRaisesRegex(ValueError, "current-run"):
            self.make(windows_state=path)

    def test_native_embeds_existing_identity_without_app_tree_or_host_probes(self):
        path = self.root / "native.json"
        identity = self.native_identity
        path.write_text(json.dumps(identity))
        first = self.make(component="native", label="native-adb-macos-x86_64", native_identity=path)
        self.env["GITHUB_SHA"] = "f" * 40
        self.tree = "e" * 40
        self.assertEqual(first, self.make(component="native", label="native-adb-macos-x86_64", native_identity=path))
        self.assertEqual(first["compatibility"], identity)
        self.assertEqual(self.calls, [])
        self.assertEqual(first["budget_bytes"], 128_000_000)

    def test_native_policy_uses_validated_component_schema_never_label_guess(self):
        path = self.root / "native.json"
        path.write_text(json.dumps(self.native_identity))
        adb = self.make(component="native", label="native-ios-misleading", native_identity=path)
        self.assertIn("scripts/build_adb_helper.py", adb["policy_files"])
        self.assertNotIn("scripts/build_ios_native_stack.py", adb["policy_files"])
        ios = {**self.native_identity, "component": "ios-native", "target": "arm64",
               "compiler_path": "/toolchain/clang", "cxx": {"sha256": "c" * 64},
               "host_tools": {"make": "pinned"}, "homebrew": {"prefix": "/opt/homebrew"},
               "build_tools": {"cmake": "pinned"}, "tool_environment": {"SDKROOT": "sdk"}}
        path.write_text(json.dumps(ios))
        result = self.make(component="native", label="native-adb-misleading", native_identity=path)
        self.assertIn("scripts/build_ios_native_stack.py", result["policy_files"])
        self.assertNotIn("scripts/build_adb_helper.py", result["policy_files"])
        self.assertNotIn(".github/actions/build-adb-helper/action.yml", result["policy_files"])
        for mutation in ({"component": "unknown"}, {"target": "unreviewed"}, {"core": "not-a-digest"}):
            with self.subTest(mutation=mutation):
                path.write_text(json.dumps({**ios, **mutation}))
                with self.assertRaisesRegex(ValueError, "native identity"):
                    self.make(component="native", native_identity=path)

    def test_external_config_cannot_be_restored_cache_configuration(self):
        current = self.make()
        output = descriptor.trusted_config(self.root, current, self.env)
        config = Path(output["config-path"])
        self.assertNotIn(".ccache", config.parts)
        self.assertIn("max_size = 500M", config.read_text())
        self.assertIn("compiler_check = content", config.read_text())
        self.assertIn("sloppiness =\n", config.read_text())
        self.assertEqual(output["container-config-path"], "/usr/local/.ci-cache-config/intel-qt6.conf")
        config.unlink()
        config.symlink_to(self.compiler)
        with self.assertRaisesRegex(ValueError, "configuration"):
            descriptor.trusted_config(self.root, current, self.env)

    @unittest.skipUnless(shutil.which("ccache"), "ccache is not installed on this host")
    def test_real_ccache_accepts_external_config_with_exact_existing_budget(self):
        current = self.make()
        outputs = descriptor.trusted_config(self.root, current, self.env)
        env = {key: value for key, value in os.environ.items() if not key.startswith("CCACHE_")}
        env["CCACHE_CONFIGPATH"] = outputs["config-path"]
        result = subprocess.run(["ccache", "--get-config", "max_size"], env=env,
                                check=True, capture_output=True, text=True, timeout=30)
        self.assertEqual(descriptor.ci_ccache.size_bytes(result.stdout), current["budget_bytes"])

    def test_cli_fork_prepares_only_trusted_config_without_git_or_toolchain_probes(self):
        repository = {"id": 12, "full_name": descriptor.transport.REPOSITORY}
        event = {"repository": repository, "number": 80,
                 "pull_request": {"number": 80, "head": {"repo": {"id": 99, "full_name": "fork/repo"}}}}
        event_file, output_file = self.root / "event.json", self.root / "github-output"
        event_file.write_text(json.dumps(event))
        env = {"GITHUB_REPOSITORY": descriptor.transport.REPOSITORY, "GITHUB_REPOSITORY_ID": "12",
               "GITHUB_EVENT_NAME": "pull_request", "GITHUB_REF": "refs/pull/80/merge",
               "GITHUB_EVENT_PATH": str(event_file), "GITHUB_OUTPUT": str(output_file),
               "RUNNER_OS": "macOS", "KLOGG_LABEL": "intel-qt6"}
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(descriptor.transport, "identity") as identity:
            with mock.patch.object(descriptor, "make_descriptor") as make:
                result = descriptor.main(["--component", "application", "--label", "intel-qt6",
                                          "--producer-job", "Application", "--required-step", "Build",
                                          "--export-step", "Export", "--output", str(self.root / "descriptor.json"),
                                          "--repo-root", str(self.root)])
        self.assertEqual(result, 0)
        identity.assert_not_called()
        make.assert_not_called()
        self.assertIn("eligible=false", output_file.read_text())
        self.assertFalse((self.root / "descriptor.json").exists())
        config = self.root / ".ci-cache-config/intel-qt6.conf"
        self.assertTrue(config.is_file())
        self.assertIn("max_size = 500M", config.read_text())
        self.assertIn("config-path=" + str(config.resolve()), output_file.read_text())

    def test_dispatch_prepares_only_enabled_application_ccache_configs(self):
        cases = (("application", "Linux", "ubuntu-24.04-deb", True),
                 ("application", "Windows", "x64-qt6", False),
                 ("native", "macOS", "native-adb-macos-x86_64", False),
                 ("application", "Linux", "ubuntu-22.04-tsan", False))
        event_file = self.root / "dispatch.json"
        event_file.write_text(json.dumps({"repository": {"id": 12, "full_name": descriptor.transport.REPOSITORY}}))
        for index, (component, platform, label, configured) in enumerate(cases):
            with self.subTest(component=component, platform=platform, label=label):
                root = self.root / str(index)
                root.mkdir()
                env = {"GITHUB_REPOSITORY": descriptor.transport.REPOSITORY, "GITHUB_REPOSITORY_ID": "12",
                       "GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_REF": "refs/heads/master",
                       "GITHUB_EVENT_PATH": str(event_file), "GITHUB_OUTPUT": str(root / "outputs"),
                       "RUNNER_OS": platform, "KLOGG_LABEL": label}
                with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(descriptor, "command") as probe:
                    descriptor.main(["--component", component, "--label", label, "--producer-job", "Lane",
                                     "--required-step", "Build", "--export-step", "Export",
                                     "--output", str(root / "descriptor.json"), "--repo-root", str(root)])
                probe.assert_not_called()
                self.assertEqual((root / ".ci-cache-config" / (label + ".conf")).exists(), configured)
                self.assertFalse((root / "descriptor.json").exists())
                if configured:
                    self.assertIn("container-config-path=/usr/local/.ci-cache-config/", (root / "outputs").read_text())

    def test_same_repo_events_eligible_forks_and_dispatch_skip_before_probes(self):
        repository = {"id": 12, "full_name": descriptor.transport.REPOSITORY}
        env = {"GITHUB_REPOSITORY": descriptor.transport.REPOSITORY, "GITHUB_REPOSITORY_ID": "12",
               "GITHUB_EVENT_NAME": "pull_request", "GITHUB_REF": "refs/pull/80/merge"}
        event = {"repository": repository, "number": 80,
                 "pull_request": {"number": 80, "head": {"repo": repository}}}
        self.assertTrue(descriptor.eligible(env, event))
        event["pull_request"]["head"]["repo"] = {"id": 13, "full_name": "fork/klogg"}
        self.assertFalse(descriptor.eligible(env, event))
        env.update(GITHUB_EVENT_NAME="workflow_dispatch", GITHUB_REF="refs/heads/master")
        self.assertFalse(descriptor.eligible(env, event))
        env.update(GITHUB_EVENT_NAME="push", GITHUB_SHA="a" * 40)
        event = {"repository": repository, "ref": "refs/heads/master", "after": "a" * 40}
        self.assertTrue(descriptor.eligible(env, event))


if __name__ == "__main__":
    unittest.main()
