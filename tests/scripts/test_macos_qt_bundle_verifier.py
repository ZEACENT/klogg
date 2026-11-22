import importlib.util
import os
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "verify_macos_qt_bundle.py"
ACTION = ROOT / ".github" / "actions" / "agent-package-mac" / "action.yml"


class MacosQtBundleVerifierTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SCRIPT.is_file():
            raise AssertionError(f"missing macOS Qt bundle verifier: {SCRIPT}")
        spec = importlib.util.spec_from_file_location("verify_macos_qt_bundle", SCRIPT)
        cls.module = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        spec.loader.exec_module(cls.module)

    def test_external_qt_dependency_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            app = pathlib.Path(directory) / "klogg.app"
            app.mkdir()
            issues = self.module.dependency_issues(
                app,
                app / "Contents/MacOS/klogg",
                ["/usr/local/opt/qtbase/lib/QtCore.framework/Versions/A/QtCore"],
            )
        self.assertTrue(any("external Qt dependency" in issue for issue in issues), issues)

    def test_bundled_qt_framework_dependency_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            app = pathlib.Path(directory) / "klogg.app"
            framework = app / "Contents/Frameworks/QtCore.framework"
            framework.mkdir(parents=True)
            issues = self.module.dependency_issues(
                app,
                app / "Contents/MacOS/klogg",
                ["@rpath/QtCore.framework/Versions/A/QtCore", "/usr/lib/libSystem.B.dylib"],
            )
        self.assertEqual(issues, [])

    def test_rpath_dependency_requires_the_bundled_framework_runpath(self):
        binary = pathlib.Path("/tmp/klogg.app/Contents/MacOS/klogg")
        dependencies = ["@rpath/QtCore5Compat.framework/Versions/A/QtCore5Compat"]
        issues = self.module.executable_rpath_issues(binary, dependencies, [])
        self.assertTrue(any("missing Qt framework runpath" in issue for issue in issues))

        issues = self.module.executable_rpath_issues(
            binary, dependencies, ["@executable_path/../Frameworks"]
        )
        self.assertEqual(issues, [])

    def test_rpath_loader_diagnostic_is_not_an_external_runtime_image(self):
        output = """\
dyld: Library not loaded: @rpath/QtCore5Compat.framework/Versions/A/QtCore5Compat
  Referenced from: /tmp/klogg.app/Contents/MacOS/klogg
"""
        issues = self.module.runtime_issues(pathlib.Path("/tmp/klogg.app"), output)
        self.assertFalse(
            any("runtime loaded Qt outside" in issue for issue in issues), issues
        )

    def test_runtime_image_paths_preserve_spaces_and_exact_bundle_boundaries(self):
        app = pathlib.Path("/tmp/Qt SDK/relocated bundle/klogg.app")
        relative = "Contents/Frameworks/QtCore.framework/Versions/A/QtCore"
        for prefix in ("dyld[42]: <01234567-89AB-CDEF-0123-456789ABCDEF>", "dyld: loaded:"):
            with self.subTest(prefix=prefix):
                bundled = f"{prefix} {app / relative}\n"
                self.assertEqual(self.module.runtime_issues(app, bundled), [])
                for outside in (pathlib.Path("/tmp/External Qt SDK/QtCore.framework/Versions/A/QtCore"),
                                pathlib.Path(str(app) + ".other") / relative):
                    with self.subTest(outside=outside):
                        issues = self.module.runtime_issues(app, f"{prefix} {outside}\n")
                        self.assertTrue(any("runtime loaded Qt outside" in issue for issue in issues), issues)

    def test_runtime_bundle_prefix_is_not_a_containment_check(self):
        app = pathlib.Path("/tmp/klogg.app").resolve()
        output = f"dyld[42]: <UUID> {app}.other/Contents/Frameworks/QtCore.framework/QtCore\n"
        issues = self.module.runtime_issues(app, output)
        self.assertTrue(any("runtime loaded Qt outside" in issue for issue in issues), issues)

    def test_runtime_qpa_and_duplicate_qt_diagnostics_are_rejected(self):
        output = """\
Class Foo is implemented in both /usr/local/QtCore and /tmp/klogg.app/QtCore
You might be loading two sets of Qt binaries into the same process.
qt.qpa.plugin: Could not load the Qt platform plugin \"cocoa\" even though it was found.
"""
        issues = self.module.runtime_issues(pathlib.Path("/tmp/klogg.app"), output)
        self.assertGreaterEqual(len(issues), 3)

    def test_packaging_uses_a_fresh_stage_and_pinned_deployment_tool(self):
        action = ACTION.read_text(encoding="utf-8")
        self.assertIn('build_root_abs="$(cd "$KLOGG_BUILD_ROOT" && pwd)"', action)
        self.assertIn('KLOGG_MAC_PACKAGE_ROOT="$build_root_abs/package-stage"', action)
        self.assertNotIn(
            'KLOGG_MAC_PACKAGE_ROOT="$KLOGG_BUILD_ROOT/package-stage"', action
        )
        self.assertIn("KLOGG_MAC_APP must be absolute", action)
        self.assertIn('cp -a "$KLOGG_BUILD_ROOT/output/klogg.app" "$KLOGG_MAC_APP"', action)
        self.assertIn('"$KLOGG_MAC_APP/Contents/Frameworks"', action)
        self.assertIn('"$KLOGG_MAC_APP/Contents/PlugIns"', action)
        self.assertIn('"$KLOGG_QT_DIR/bin/macdeployqt" "$KLOGG_MAC_APP"', action)
        self.assertNotIn("macdeployqt ./output/klogg.app", action)
        self.assertIn("scripts/verify_macos_qt_bundle.py", action)
        self.assertGreaterEqual(action.count("--smoke cocoa"), 2)
        self.assertGreaterEqual(action.count("--smoke-if-present offscreen"), 2)
        self.assertIn("macos-qt-mounted-dmg-smoke.log", action)

    def test_bundle_requires_cli_with_matching_architecture_and_framework_runpath(self):
        with tempfile.TemporaryDirectory() as directory:
            app = pathlib.Path(directory) / "klogg.app"
            cli = app / "Contents/MacOS/kloggctl"
            for relative in ("Contents/MacOS/klogg", "Contents/MacOS/kloggctl",
                             "Contents/PlugIns/platforms/libqcocoa.dylib"):
                path = app / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture")
            resources = app / "Contents/Resources"
            resources.mkdir()
            (resources / "qt.conf").write_text("[Paths]\nPlugins = PlugIns\n")
            (app / "Contents/Frameworks/QtCore.framework").mkdir(parents=True)
            dependencies = ["@rpath/QtCore.framework/Versions/A/QtCore"]
            with mock.patch.object(self.module, "macho_dependencies", return_value=dependencies), \
                 mock.patch.object(self.module, "macho_rpaths", return_value=[self.module.QT_FRAMEWORK_RPATH]) as rpaths, \
                 mock.patch.object(self.module, "command_output", return_value="arm64") as architectures:
                self.assertEqual(self.module.validate_bundle(app, "arm64"), [])
                self.assertIn(mock.call(["lipo", "-archs", str(cli)]), architectures.call_args_list)
                rpaths.side_effect = lambda path: [] if path == cli else [self.module.QT_FRAMEWORK_RPATH]
                issues = self.module.validate_bundle(app, "arm64")
                self.assertTrue(any("kloggctl" in issue and "runpath" in issue for issue in issues), issues)
                rpaths.side_effect = None
                architectures.side_effect = lambda command: "x86_64" if command[-1] == str(cli) else "arm64"
                issues = self.module.validate_bundle(app, "arm64")
                self.assertTrue(any("kloggctl" in issue and "architecture" in issue for issue in issues), issues)
                cli.unlink()
                issues = self.module.validate_bundle(app, "arm64")
                self.assertTrue(any("missing" in issue and "kloggctl" in issue for issue in issues), issues)

    def test_package_smoke_runs_offline_cli_without_developer_loader_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            app = pathlib.Path(directory) / "relocated/klogg.app"
            plugin = app / "Contents/PlugIns/platforms/libqcocoa.dylib"
            plugin.parent.mkdir(parents=True)
            plugin.write_bytes(b"fixture")
            result = subprocess.CompletedProcess([], 0, "version", "")
            with mock.patch.dict(os.environ, {
                    "DYLD_FRAMEWORK_PATH": "/developer/Qt",
                    "KLOGG_PORTABLE_CONFIG_DIR": "/real/config",
                    "KLOGG_CAPTURE_COORDINATION_ROOT": "/real/capture",
                 }), \
                 mock.patch.object(self.module.subprocess, "run", return_value=result) as run:
                self.assertEqual(self.module.smoke_bundle(app, "cocoa", required=True, log_output=None), [])
                cli_calls = [call for call in run.call_args_list if call.args[0][0].endswith("/kloggctl")]
                self.assertEqual([call.args[0][1:] for call in cli_calls], [["--version"], ["--help"]])
                for call in run.call_args_list:
                    environment = call.kwargs["env"]
                    self.assertNotIn("DYLD_FRAMEWORK_PATH", environment)
                    self.assertNotEqual(environment["HOME"], os.environ.get("HOME"))
                    for name in ("KLOGG_PORTABLE_CONFIG_DIR", "KLOGG_CAPTURE_COORDINATION_ROOT"):
                        self.assertEqual(pathlib.Path(environment[name]).parent,
                                         pathlib.Path(environment["HOME"]))
                run.side_effect = lambda command, **kwargs: subprocess.CompletedProcess(
                    command, 1 if command[0].endswith("/kloggctl") else 0, "", "")
                issues = self.module.smoke_bundle(app, "cocoa", required=True, log_output=None)
                self.assertTrue(any("kloggctl" in issue and "exited" in issue for issue in issues), issues)
                for error, diagnostic in ((FileNotFoundError("missing CLI"), "could not execute"),
                                          (subprocess.TimeoutExpired("kloggctl", 30), "timed out")):
                    with self.subTest(diagnostic=diagnostic):
                        run.side_effect = [result, error, result]
                        issues = self.module.smoke_bundle(app, "cocoa", required=True, log_output=None)
                        self.assertTrue(any("kloggctl" in issue and diagnostic in issue for issue in issues), issues)

    def test_package_stage_path_survives_build_root_working_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace = pathlib.Path(directory)
            app = workspace / "build_root" / "output" / "klogg.app"
            app.mkdir(parents=True)
            env = os.environ.copy()
            env["KLOGG_BUILD_ROOT"] = "build_root"
            subprocess.run(
                [
                    "sh",
                    "-c",
                    """
set -eu
build_root_abs="$(cd "$KLOGG_BUILD_ROOT" && pwd)"
KLOGG_MAC_PACKAGE_ROOT="$build_root_abs/package-stage"
KLOGG_MAC_APP="$KLOGG_MAC_PACKAGE_ROOT/klogg.app"
mkdir -p "$KLOGG_MAC_PACKAGE_ROOT"
cp -a "$KLOGG_BUILD_ROOT/output/klogg.app" "$KLOGG_MAC_APP"
cd "$KLOGG_BUILD_ROOT"
test -d "$KLOGG_MAC_APP"
""",
                ],
                cwd=workspace,
                env=env,
                check=True,
            )


if __name__ == "__main__":
    unittest.main()
