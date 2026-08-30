import importlib.util
import os
import pathlib
import subprocess
import tempfile
import unittest


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
