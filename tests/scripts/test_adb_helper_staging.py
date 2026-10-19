"""Exercise the application helper staging graph without Qt or native downloads."""
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CMAKE = shutil.which("cmake")
NINJA = shutil.which("ninja")
COMPILER = next((shutil.which(name) for name in ("cc", "clang", "gcc", "cl")
                 if shutil.which(name)), None)


@unittest.skipUnless(CMAKE and NINJA and COMPILER, "requires CMake, Ninja and a C compiler")
class AdbHelperStagingTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="adb staging ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def run_command(self, command, *, check=True):
        result = subprocess.run(command, cwd=self.root, capture_output=True, text=True,
                                timeout=60)
        if check:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def configure(self, name, *, windows=True, multi=False, available=True, apple=False,
                  producer_fails=False):
        source = self.root / name
        source.mkdir()
        stage = source / "verified helper" / "helpers"
        stage.mkdir(parents=True)
        (stage / "adb.exe").write_bytes(b"verified helper v1")
        (stage / "runtime.dll").write_bytes(b"verified runtime closure")
        package = source / "package support"
        package.mkdir()
        (package / "NOTICE.txt").write_bytes(b"verified legal support")
        (source / "main.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
        app = (ROOT / "src/app/CMakeLists.txt").read_text(encoding="utf-8")
        block = app.split("add_dependencies(klogg klogg_adb_helper)", 1)[1]
        block = "add_dependencies(klogg klogg_adb_helper)" + block.split(
            "set_target_properties(klogg PROPERTIES AUTORCC ON)", 1)[0]
        producer = "add_custom_target(klogg_adb_helper)\n"
        if producer_fails:
            (source / "reject.cmake").write_text(
                'message(FATAL_ERROR "verified producer rejected input")\n', encoding="utf-8")
            producer = ('add_custom_target(klogg_adb_helper COMMAND "${CMAKE_COMMAND}" '
                        '-P "${CMAKE_SOURCE_DIR}/reject.cmake")\n')
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.14)\n"
            "project(HelperStaging C)\n"
            f"set(APPLE {'TRUE' if apple else 'FALSE'})\n"
            f"set(WIN32 {'TRUE' if windows else 'FALSE'})\n"
            'set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output")\n'
            'set(KLOGG_ADB_HELPER_STAGE "${CMAKE_SOURCE_DIR}/verified helper")\n'
            'set(KLOGG_ADB_HELPER_PACKAGE_STAGE "${CMAKE_SOURCE_DIR}/package support")\n'
            f"set(KLOGG_ADB_HELPER_AVAILABLE {'TRUE' if available else 'FALSE'})\n"
            + producer + "add_custom_target(klogg_grep)\n"
            "add_custom_target(ci_build)\n"
            f"add_executable(klogg {'MACOSX_BUNDLE' if apple else ''} main.c)\n"
            f"add_executable(klogg_portable {'MACOSX_BUNDLE' if apple else ''} main.c)\n" + block,
            encoding="utf-8")
        build = source / "build"
        generator = "Ninja Multi-Config" if multi else "Ninja"
        self.run_command([CMAKE, "-S", str(source), "-B", str(build), "-G", generator,
                          "-DCMAKE_MAKE_PROGRAM=" + NINJA,
                          "-DCMAKE_C_COMPILER=" + COMPILER])
        return build, stage

    def build(self, build, target="ci_build", *, config=None, check=True):
        command = [CMAKE, "--build", str(build), "--parallel", "8", "--target", target]
        if config:
            command += ["--config", config]
        return self.run_command(command, check=check)

    @staticmethod
    def executable(build, target):
        return build / "output" / (target + (".exe" if os.name == "nt" else ""))

    def test_shared_directory_has_one_writer_for_parallel_applications(self):
        for windows in (True, False):
            with self.subTest(windows=windows):
                build, _ = self.configure("windows" if windows else "linux", windows=windows)
                graph = self.run_command([NINJA, "-C", str(build), "-t", "commands",
                                          "klogg", "klogg_portable", "klogg_stage_adb_helpers"]).stdout
                # Assert graph ownership, not whether a filesystem race happens
                # to reproduce on this host. Each old POST_BUILD was a writer.
                self.assertEqual(graph.count("copy_directory"), 1, graph)
                self.build(build, "all")
                self.assertTrue(self.executable(build, "klogg").is_file())
                self.assertTrue(self.executable(build, "klogg_portable").is_file())
                self.assertEqual((build / "output/helpers/adb.exe").read_bytes(),
                                 b"verified helper v1")
                self.assertTrue((build / "output/helpers/runtime.dll").is_file())

    def test_either_app_stages_independently_and_refreshes_without_relinking(self):
        for target in ("klogg", "klogg_portable"):
            with self.subTest(target=target):
                build, stage = self.configure(target)
                other = "klogg_portable" if target == "klogg" else "klogg"
                self.build(build, target)
                self.assertFalse(self.executable(build, other).exists())
                executable = self.executable(build, target)
                original_mtime = executable.stat().st_mtime_ns
                destination = build / "output/helpers"
                (destination / "stale.dll").write_bytes(b"obsolete runtime")
                (stage / "adb.exe").write_bytes(b"verified helper v2")
                self.build(build, target)
                self.assertEqual((destination / "adb.exe").read_bytes(), b"verified helper v2")
                self.assertFalse((destination / "stale.dll").exists())
                self.assertEqual(executable.stat().st_mtime_ns, original_mtime)
                self.assertFalse(self.executable(build, other).exists())

    def test_multi_config_uses_each_configuration_runtime_directory(self):
        capabilities = json.loads(self.run_command([CMAKE, "-E", "capabilities"]).stdout)
        if "Ninja Multi-Config" not in {item["name"] for item in capabilities["generators"]}:
            self.skipTest("this CMake does not provide Ninja Multi-Config")
        build, _ = self.configure("multi config", multi=True)
        for config in ("Debug", "Release"):
            with self.subTest(config=config):
                self.build(build, config=config)
                self.assertEqual((build / "output" / config / "helpers/adb.exe").read_bytes(),
                                 b"verified helper v1")
        self.assertFalse((build / "output/helpers").exists())

    def test_missing_verified_helper_fails_before_publishing_applications(self):
        build, stage = self.configure("missing helper")
        shutil.rmtree(stage)
        result = self.build(build, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("copy", (result.stdout + result.stderr).lower())
        self.assertFalse(self.executable(build, "klogg").exists())
        self.assertFalse(self.executable(build, "klogg_portable").exists())

    def test_producer_failure_prevents_staging_and_application_builds(self):
        build, _ = self.configure("rejected helper", producer_fails=True)
        result = self.build(build, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("verified producer rejected input", result.stdout + result.stderr)
        self.assertFalse((build / "output/helpers").exists())
        self.assertFalse(self.executable(build, "klogg").exists())
        self.assertFalse(self.executable(build, "klogg_portable").exists())

    @unittest.skipUnless(platform.system() == "Darwin", "requires real macOS bundles")
    def test_macos_bundles_retain_private_runtime_and_package_support(self):
        build, _ = self.configure("mac bundles", windows=False, apple=True)
        self.build(build, "all")
        for target in ("klogg", "klogg_portable"):
            with self.subTest(target=target):
                contents = build / "output" / (target + ".app") / "Contents"
                self.assertEqual((contents / "MacOS/helpers/adb.exe").read_bytes(),
                                 b"verified helper v1")
                self.assertEqual((contents / "SharedSupport/adb-helper/NOTICE.txt").read_bytes(),
                                 b"verified legal support")
                self.run_command([str(contents / "MacOS" / target)])
        self.assertFalse((build / "output/helpers").exists())

    def test_disabled_optional_helper_does_not_create_a_staging_task(self):
        build, stage = self.configure("disabled helper", available=False)
        shutil.rmtree(stage)
        self.build(build)
        self.assertFalse((build / "output/helpers").exists())
        graph = self.run_command([NINJA, "-C", str(build), "-t", "commands", "ci_build"]).stdout
        self.assertNotIn("copy_directory", graph)


if __name__ == "__main__":
    unittest.main()
