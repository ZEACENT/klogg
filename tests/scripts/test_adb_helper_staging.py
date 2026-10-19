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
                  producer_fails=False, cmake_options=(), after_targets=""):
        source = self.root / name
        source.mkdir()
        (source / "cmake").mkdir()
        shutil.copyfile(ROOT / "cmake/StageAdbHelpers.cmake",
                        source / "cmake/StageAdbHelpers.cmake")
        stage = source / "verified helper" / "helpers"
        stage.mkdir(parents=True)
        (stage / "adb.exe").write_bytes(b"verified helper v1")
        (stage / "runtime.dll").write_bytes(b"verified runtime closure")
        package = source / "package support"
        package.mkdir()
        (package / "NOTICE.txt").write_bytes(b"verified legal support")
        app_source = source / "src/app"
        helper_source = source / "packaging/adb"
        app_source.mkdir(parents=True)
        helper_source.mkdir(parents=True)
        (app_source / "main.c").write_text("int main(void) { return 0; }\n", encoding="utf-8")
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
        (helper_source / "CMakeLists.txt").write_text(producer, encoding="utf-8")
        (app_source / "CMakeLists.txt").write_text(
            f"add_executable(klogg {'MACOSX_BUNDLE' if apple else ''} main.c)\n"
            f"add_executable(klogg_portable {'MACOSX_BUNDLE' if apple else ''} main.c)\n"
            + block + after_targets, encoding="utf-8")
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.14)\n"
            "project(HelperStaging C)\n"
            f"set(APPLE {'TRUE' if apple else 'FALSE'})\n"
            f"set(WIN32 {'TRUE' if windows else 'FALSE'})\n"
            'set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/output")\n'
            'set(KLOGG_ADB_HELPER_STAGE "${CMAKE_SOURCE_DIR}/verified helper")\n'
            'set(KLOGG_ADB_HELPER_PACKAGE_STAGE "${CMAKE_SOURCE_DIR}/package support")\n'
            f"set(KLOGG_ADB_HELPER_AVAILABLE {'TRUE' if available else 'FALSE'})\n"
            "add_custom_target(klogg_grep)\n"
            "add_custom_target(ci_build)\n"
            "add_subdirectory(packaging/adb)\n"
            "add_subdirectory(src/app)\n",
            encoding="utf-8")
        build = source / "build"
        generator = "Ninja Multi-Config" if multi else "Ninja"
        self.run_command([CMAKE, "-S", str(source), "-B", str(build), "-G", generator,
                          "-DCMAKE_MAKE_PROGRAM=" + NINJA,
                          "-DCMAKE_C_COMPILER=" + COMPILER, *cmake_options])
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
                self.assertEqual(graph.count("/cmake/StageAdbHelpers.cmake"), 1, graph)
                self.assertNotIn(" -E copy_directory ", graph)
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

    def test_single_config_output_override_can_be_absolute_or_relative(self):
        for name, runtime in (("absolute", str(self.root / "custom debug")),
                              ("relative", "relative debug")):
            with self.subTest(name=name):
                build, _ = self.configure(name, cmake_options=(
                    "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG=" + runtime))
                self.build(build)
                destination = (Path(runtime) if Path(runtime).is_absolute()
                               else build / "src/app" / runtime)
                executable = "klogg.exe" if os.name == "nt" else "klogg"
                self.assertTrue((destination / executable).is_file())
                self.assertTrue((destination / "helpers/adb.exe").is_file(),
                                "helpers must follow the effective executable directory")
                self.assertFalse((build / "output/helpers").exists())

    def test_multi_config_override_has_no_extra_configuration_suffix(self):
        capabilities = json.loads(self.run_command([CMAKE, "-E", "capabilities"]).stdout)
        if "Ninja Multi-Config" not in {item["name"] for item in capabilities["generators"]}:
            self.skipTest("this CMake does not provide Ninja Multi-Config")
        runtime = self.root / "debug override"
        build, _ = self.configure("config override", multi=True, cmake_options=(
            "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG=" + str(runtime),))
        self.build(build, config="Debug")
        executable = "klogg.exe" if os.name == "nt" else "klogg"
        self.assertTrue((runtime / executable).is_file())
        self.assertTrue((runtime / "helpers/adb.exe").is_file())
        self.assertFalse((build / "output/Debug/helpers").exists())
        self.build(build, config="Release")
        self.assertTrue((build / "output/Release/helpers/adb.exe").is_file())

    def test_late_target_directories_and_generator_expressions_are_resolved(self):
        properties = (
            'set_target_properties(klogg PROPERTIES RUNTIME_OUTPUT_DIRECTORY '
            '"${CMAKE_BINARY_DIR}/regular-$<CONFIG>")\n'
            'set_target_properties(klogg_portable PROPERTIES RUNTIME_OUTPUT_DIRECTORY '
            '"${CMAKE_BINARY_DIR}/portable-$<CONFIG>")\n')
        build, _ = self.configure("late properties", cmake_options=("-DCMAKE_BUILD_TYPE=Debug",),
                                  after_targets=properties)
        self.build(build, "klogg")
        executable_suffix = ".exe" if os.name == "nt" else ""
        self.assertTrue((build / "regular-Debug" / ("klogg" + executable_suffix)).is_file())
        self.assertFalse((build / "portable-Debug" / ("klogg_portable" + executable_suffix)).exists())
        self.build(build, "klogg_portable")
        for directory in ("regular-Debug", "portable-Debug"):
            self.assertTrue((build / directory / "helpers/adb.exe").is_file())
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


@unittest.skipUnless(CMAKE, "requires CMake")
class AdbHelperStagingScriptTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="adb staging data ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "verified/helpers"
        self.source.mkdir(parents=True)
        (self.source / "adb.exe").write_bytes(b"verified runtime")
        self.destination = self.root / "runtime/helpers"
        self.destination.mkdir(parents=True)
        (self.destination / "stale.dll").write_bytes(b"old runtime")

    def stage(self, manifest, *, trace=False):
        paths = self.root / "destinations.txt"
        paths.write_text(manifest, encoding="utf-8")
        script = ROOT / "cmake/StageAdbHelpers.cmake"
        command = [CMAKE]
        if trace:
            command += ["--trace-expand", "--trace-source=" + str(script)]
        command += ["-DSOURCE_HELPERS=" + str(self.source),
                    "-DDESTINATIONS_FILE=" + str(paths), "-P", str(script)]
        return subprocess.run(command, cwd=self.root, capture_output=True, text=True, timeout=20)

    def test_duplicate_destinations_are_copied_once(self):
        result = self.stage((str(self.destination) + "\n") * 2, trace=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr.count(" -E copy_directory "), 1)
        self.assertEqual((self.destination / "adb.exe").read_bytes(), b"verified runtime")
        self.assertFalse((self.destination / "stale.dll").exists())

    def test_invalid_data_is_rejected_before_any_destination_is_removed(self):
        marker = self.root / "must-not-execute"
        invalid = ("", "relative/helpers", str(self.root / "not-helpers"), str(self.source),
                   'file(WRITE "' + marker.as_posix() + '" "executed")')
        for value in invalid:
            with self.subTest(value=value):
                manifest = str(self.destination) + "\n" + value + "\n" if value else ""
                result = self.stage(manifest)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue((self.destination / "stale.dll").is_file())
                self.assertEqual((self.source / "adb.exe").read_bytes(), b"verified runtime")
                self.assertFalse(marker.exists())

    def test_filesystem_staging_failure_is_not_ignored(self):
        blocker = self.root / "not a directory"
        blocker.write_bytes(b"keep this file")
        result = self.stage(str(blocker / "helpers") + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, r"(?:Removing|Copying).*failed")
        self.assertEqual(blocker.read_bytes(), b"keep this file")


if __name__ == "__main__":
    unittest.main()
