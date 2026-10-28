"""Windows release support and retained cross-platform compatibility contracts."""
from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("windows_support_quality", ROOT / "scripts/lint_ci_quality.py")
QUALITY = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(QUALITY)
WORKFLOW = ROOT / ".github/workflows/ci-build.yml"


class WindowsSupportPolicyTest(unittest.TestCase):
    def test_windows_jobs_are_x64_only_without_legacy_openssl_transport(self):
        text = WORKFLOW.read_text()
        blocks = QUALITY.workflow_job_blocks(text)
        windows = {name for name in blocks if name.startswith("Windows")}
        self.assertEqual(windows, {"WindowsPackages", "WindowsAsan"})
        self.assertNotIn("PrefetchOpenSsl", blocks)
        for job in windows:
            env = QUALITY.workflow_mapping_block(blocks[job], "env", 4)
            self.assertEqual(env["KLOGG_CONFIG_ARCH"][0], "x64")
            self.assertTrue(env["KLOGG_QT_VERSION"][0].startswith("6."))
            for step in QUALITY.workflow_job_steps(text)[job]:
                _, children = QUALITY.workflow_step_fields(step)
                self.assertNotEqual(children.get("with", {}).get("name"), "openssl-archive")
        self.assertEqual(QUALITY.ci_build_workflow_issues(text), [])

    def test_windows_architecture_mutations_are_rejected(self):
        text = WORKFLOW.read_text()
        for job in ("WindowsPackages", "WindowsAsan"):
            block = "\n".join(QUALITY.workflow_job_blocks(text)[job])
            old = "      KLOGG_CONFIG_ARCH: x64"
            self.assertIn(old, block)
            for replacement in (
                "      KLOGG_CONFIG_ARCH: x86",
                "      KLOGG_CONFIG_ARCH: arm64",
                "      # KLOGG_CONFIG_ARCH: x64",
                "      KLOGG_CONFIG_ARCH: x86 # KLOGG_CONFIG_ARCH: x64",
                '      KLOGG_CONFIG_ARCH: ""',
                old + "\n      KLOGG_CONFIG_ARCH: x86",
            ):
                with self.subTest(job=job, replacement=replacement):
                    changed = text.replace(block, block.replace(old, replacement, 1), 1)
                    issues = QUALITY.ci_build_workflow_issues(changed)
                    self.assertTrue(any("Windows architecture" in issue for issue in issues), issues)

    def test_linux_qt5_and_qt_regex_remain_supported(self):
        blocks = QUALITY.workflow_job_blocks(WORKFLOW.read_text())
        tsan_env = QUALITY.workflow_mapping_block(blocks["LinuxTsan"], "env", 4)
        self.assertIn("Qt5_DIR", tsan_env["KLOGG_CONFIG_CMAKE_OPTS"][0])
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn("KLOGG_USE_VECTORSCAN", cmake)

    def test_cmake_rejects_32_bit_windows_without_rejecting_other_targets(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        guard = "if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)"
        self.assertIn(guard, cmake)
        block = guard + cmake.split(guard, 1)[1].split("endif()", 1)[0] + "endif()\n"
        with tempfile.TemporaryDirectory() as scratch:
            script = pathlib.Path(scratch) / "platform-check.cmake"
            script.write_text(block)
            for windows, pointer_size, rejected in (("ON", 4, True), ("ON", 8, False),
                                                     ("OFF", 4, False), ("OFF", 8, False)):
                with self.subTest(windows=windows, pointer_size=pointer_size):
                    result = subprocess.run(["cmake", "-DWIN32=" + windows,
                        "-DCMAKE_SIZEOF_VOID_P=" + str(pointer_size), "-P", str(script)],
                        capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode != 0, rejected, result.stderr)
                    if rejected:
                        self.assertIn("v26.08.08.1096", result.stderr)

    def test_package_entry_points_and_feeds_are_x64_only(self):
        packaging = ROOT / "packaging/windows"
        batch = (packaging / "prepare_release.cmd").read_text()
        guard = '@if not "%KLOGG_ARCH%"=="x64" ('
        self.assertTrue(batch.startswith(guard))
        self.assertIn("exit /b 1", batch.split("\n)", 1)[0])
        self.assertNotIn("_32_md_", batch)
        installer = (packaging / "klogg.nsi").read_text()
        preamble = installer.split("!include", 1)[0]
        for condition in ('!ifndef PLATFORM', '!if "${PLATFORM}" != "x64"', '!ifdef ARCH32'):
            self.assertIn("!error", preamble.split(condition, 1)[1].split("!endif", 1)[0])
        scoop = json.loads((packaging / "scoop/klogg.json").read_text())
        self.assertEqual(set(scoop["architecture"]), {"64bit"})
        chocolatey = (packaging / "chocolatey/tools/chocolateyInstall.ps1").read_text()
        self.assertIn("Is64BitOperatingSystem", chocolatey.split("$packageArgs", 1)[0])
        self.assertNotRegex(chocolatey, r"(?m)^\s*(?:url|checksum|checksumType)\s*=")
        tests = (ROOT / "tests/CMakeLists.txt").read_text()
        windows_tests = tests.split("if(WIN32)", 1)[1].split("endif()", 1)[0]
        self.assertIn("tests/scripts/test_windows_support_policy.py", windows_tests)

    @unittest.skipUnless(os.name == "nt", "requires native Windows cmd")
    def test_unsupported_batch_target_cannot_delete_existing_stage(self):
        with tempfile.TemporaryDirectory() as scratch:
            release = pathlib.Path(scratch) / "release"
            release.mkdir()
            sentinel = release / "keep.txt"
            sentinel.write_text("existing stage")
            for architecture in ("x86", "arm64", ""):
                with self.subTest(architecture=architecture):
                    result = subprocess.run(["cmd", "/d", "/c", str(ROOT / "packaging/windows/prepare_release.cmd")],
                        env={**os.environ, "KLOGG_ARCH": architecture, "KLOGG_WORKSPACE": scratch},
                        capture_output=True, text=True, timeout=15)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("Windows packaging supports only x64", result.stdout)
                    self.assertEqual(sentinel.read_text(), "existing stage")

    @unittest.skipUnless(shutil.which("makensis"), "requires NSIS compiler")
    def test_nsis_rejects_unsupported_targets_before_reading_payload(self):
        prefix = "/" if os.name == "nt" else "-"
        for defines in ([], ["PLATFORM=x86"], ["PLATFORM=x64", "ARCH32"]):
            with self.subTest(defines=defines):
                result = subprocess.run([shutil.which("makensis"), prefix + "V2",
                    *[prefix + "D" + define for define in defines],
                    str(ROOT / "packaging/windows/klogg.nsi")],
                    capture_output=True, text=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Windows packaging", result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which("pwsh"), "requires PowerShell")
    def test_local_qt5_packages_require_x64_ssl_without_x86_contamination(self):
        cases = (("x64",), ("x86",), ("x64", "x86"), ())
        for architectures in cases:
            with self.subTest(architectures=architectures), tempfile.TemporaryDirectory() as scratch:
                for architecture in architectures:
                    suffix = "-x64" if architecture == "x64" else ""
                    for library in ("libcrypto", "libssl"):
                        (pathlib.Path(scratch) / (library + "-1_1" + suffix + ".dll")).write_bytes(b"fixture")
                result = subprocess.run([shutil.which("pwsh"), "-NoProfile", "-File",
                    str(ROOT / "packaging/windows/verify_ssl.ps1"), "-ReleaseDir", scratch, "-QtVersion", "Qt5"],
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode == 0, architectures == ("x64",), result.stdout + result.stderr)

    def test_readme_identifies_last_unsupported_x86_release(self):
        readme = (ROOT / "README.md").read_text()
        self.assertIn("Windows x86 (32-bit) support has ended", readme)
        self.assertIn("https://github.com/ZEACENT/klogg/releases/tag/v26.08.08.1096", readme)


if __name__ == "__main__":
    unittest.main()
