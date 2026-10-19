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
                  producer_fails=False, cmake_options=(), after_targets="", trace_staging=False):
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
        if trace_staging:
            invocation = '-P "${CMAKE_SOURCE_DIR}/cmake/StageAdbHelpers.cmake"'
            block = block.replace(invocation, '--trace-expand '
                                  '"--trace-source=${CMAKE_SOURCE_DIR}/cmake/StageAdbHelpers.cmake" ' + invocation)
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
                build, _ = self.configure("windows" if windows else "linux", windows=windows,
                                          trace_staging=True)
                graph = self.run_command([NINJA, "-C", str(build), "-t", "commands",
                                          "klogg", "klogg_portable", "klogg_stage_adb_helpers"]).stdout
                # Role nodes may be no-ops. Count actual executed copies rather
                # than helper invocations or a timing-dependent filesystem race.
                self.assertNotIn(" -E copy_directory ", graph)
                result = self.build(build, "all")
                self.assertEqual((result.stdout + result.stderr).count(" -E copy_directory "), 1)
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

    def test_nested_output_destinations_reject_before_deleting_existing_applications(self):
        cases = (("klogg", "klogg_portable", True, "klogg"),
                 ("klogg_portable", "klogg", True, "klogg_portable"),
                 ("klogg", "klogg_portable", False, "ci_build"))
        for index, (outer_target, inner_target, windows, requested) in enumerate(cases):
            with self.subTest(outer=outer_target, requested=requested):
                properties = (
                    'set_target_properties(' + outer_target + ' PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/outer")\n'
                    'set_target_properties(' + inner_target + ' PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/outer/helpers/inner")\n')
                build, stage = self.configure("nested outputs " + str(index), windows=windows,
                                              available=False, after_targets=properties)
                self.build(build, inner_target)
                executable = build / "outer/helpers/inner" / (inner_target + (".exe" if os.name == "nt" else ""))
                original_executable = executable.read_bytes()
                sentinels = (build / "outer/helpers/keep-outer", build / "outer/helpers/inner/helpers/keep-inner")
                for sentinel in sentinels:
                    sentinel.parent.mkdir(parents=True, exist_ok=True)
                    sentinel.write_bytes(b"preexisting output")
                root_cmake = build.parent / "CMakeLists.txt"
                root_cmake.write_text(root_cmake.read_text().replace(
                    "set(KLOGG_ADB_HELPER_AVAILABLE FALSE)", "set(KLOGG_ADB_HELPER_AVAILABLE TRUE)"))
                self.run_command([CMAKE, "-S", str(build.parent), "-B", str(build)])
                result = self.build(build, requested, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("destination paths overlap", result.stdout + result.stderr)
                self.assertEqual(executable.read_bytes(), original_executable)
                for sentinel in sentinels:
                    self.assertEqual(sentinel.read_bytes(), b"preexisting output")
                self.assertEqual((stage / "adb.exe").read_bytes(), b"verified helper v1")

    def test_nested_parent_output_alias_rejects_before_deleting_existing_executable(self):
        properties = (
            'set_target_properties(klogg PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/outer")\n'
            'set_target_properties(klogg_portable PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/portable-alias")\n')
        build, _ = self.configure("nested output via parent alias", available=False, after_targets=properties)
        inner = build / "outer/helpers/inner"
        inner.mkdir(parents=True)
        alias = build / "portable-alias"
        if alias.exists():
            shutil.rmtree(alias)
        try:
            alias.symlink_to(inner, target_is_directory=True)
        except OSError:
            if os.name == "nt":
                self.skipTest("directory symlinks are unavailable")
            raise
        self.build(build, "klogg_portable")
        executable = inner / ("klogg_portable.exe" if os.name == "nt" else "klogg_portable")
        original_executable = executable.read_bytes()
        root_cmake = build.parent / "CMakeLists.txt"
        root_cmake.write_text(root_cmake.read_text().replace(
            "set(KLOGG_ADB_HELPER_AVAILABLE FALSE)", "set(KLOGG_ADB_HELPER_AVAILABLE TRUE)"))
        self.run_command([CMAKE, "-S", str(build.parent), "-B", str(build)])
        result = self.build(build, "klogg", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("destination paths overlap", result.stdout + result.stderr)
        self.assertTrue(alias.is_symlink())
        self.assertEqual(executable.read_bytes(), original_executable)

    def test_direct_app_ignores_unrequested_app_with_blocked_output_parent(self):
        properties = (
            'set_target_properties(klogg PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/regular/bin")\n'
            'set_target_properties(klogg_portable PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/portable/bin")\n')
        for target, own, unused in (("klogg", "regular", "portable"),
                                    ("klogg_portable", "portable", "regular")):
            with self.subTest(target=target):
                build, stage = self.configure("blocked " + target, after_targets=properties, trace_staging=True)
                blocker = build / unused
                if blocker.exists():
                    shutil.rmtree(blocker)
                blocker.write_bytes(b"unrequested output parent must remain untouched")
                result = self.build(build, target)
                self.assertEqual((result.stdout + result.stderr).count(" -E copy_directory "), 1)
                self.assertEqual((build / own / "bin/helpers/adb.exe").read_bytes(), b"verified helper v1")
                self.assertEqual(blocker.read_bytes(), b"unrequested output parent must remain untouched")
                executable = build / own / "bin" / (target + (".exe" if os.name == "nt" else ""))
                original_mtime = executable.stat().st_mtime_ns
                destination = build / own / "bin/helpers"
                (destination / "stale.dll").write_bytes(b"old private runtime")
                (stage / "adb.exe").write_bytes(b"verified helper v2")
                refreshed = self.build(build, target)
                self.assertEqual((refreshed.stdout + refreshed.stderr).count(" -E copy_directory "), 1)
                self.assertEqual((destination / "adb.exe").read_bytes(), b"verified helper v2")
                self.assertFalse((destination / "stale.dll").exists())
                self.assertEqual(executable.stat().st_mtime_ns, original_mtime)
                self.assertEqual(blocker.read_bytes(), b"unrequested output parent must remain untouched")

    def test_linux_ci_build_does_not_stage_unrequested_portable_output(self):
        properties = (
            'set_target_properties(klogg PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/regular/bin")\n'
            'set_target_properties(klogg_portable PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/portable/bin")\n')
        build, _ = self.configure("linux requested apps", windows=False, after_targets=properties,
                                  trace_staging=True)
        blocker = build / "portable"
        if blocker.exists():
            shutil.rmtree(blocker)
        blocker.write_bytes(b"unused portable parent")
        result = self.build(build, "ci_build")
        self.assertEqual((result.stdout + result.stderr).count(" -E copy_directory "), 1)
        self.assertTrue((build / "regular/bin/helpers/adb.exe").is_file())
        self.assertEqual(blocker.read_bytes(), b"unused portable parent")

    def test_shared_parent_alias_has_one_actual_copy_for_parallel_apps(self):
        properties = (
            'set_target_properties(klogg PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/regular")\n'
            'set_target_properties(klogg_portable PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/portable")\n')
        build, _ = self.configure("aliased runtime", after_targets=properties, trace_staging=True)
        regular, portable = build / "regular", build / "portable"
        regular.mkdir(exist_ok=True)
        if portable.exists():
            shutil.rmtree(portable)
        try:
            portable.symlink_to(regular, target_is_directory=True)
        except OSError:
            if os.name == "nt":
                self.skipTest("directory symlinks are unavailable")
            raise
        result = self.build(build, "all")
        self.assertEqual((result.stdout + result.stderr).count(" -E copy_directory "), 1)
        self.assertEqual((portable / "helpers/adb.exe").read_bytes(), b"verified helper v1")
        self.assertTrue(portable.is_symlink())

    def test_leaf_helpers_alias_does_not_change_shared_private_routing(self):
        properties = (
            'set_target_properties(klogg PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/regular")\n'
            'set_target_properties(klogg_portable PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/portable")\n')
        build, _ = self.configure("leaf runtime alias", after_targets=properties, trace_staging=True)
        regular, portable = build / "regular/helpers", build / "portable/helpers"
        regular.mkdir(parents=True, exist_ok=True)
        portable.parent.mkdir(parents=True, exist_ok=True)
        (regular / "stale.dll").write_bytes(b"old runtime")
        try:
            portable.symlink_to(regular, target_is_directory=True)
        except OSError:
            if os.name == "nt":
                self.skipTest("directory symlinks are unavailable")
            raise
        result = self.build(build, "all")
        self.assertEqual((result.stdout + result.stderr).count(" -E copy_directory "), 2)
        self.assertFalse(portable.is_symlink())
        for destination in (regular, portable):
            self.assertEqual((destination / "adb.exe").read_bytes(), b"verified helper v1")
            self.assertFalse((destination / "stale.dll").exists())

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

    def stage(self, manifest, *, trace=False, windows_host=None):
        paths = self.root / "destinations.txt"
        paths.write_text(manifest, encoding="utf-8")
        script = ROOT / "cmake/StageAdbHelpers.cmake"
        command = [CMAKE]
        if windows_host is not None:
            command += ["-DCMAKE_HOST_WIN32=" + ("TRUE" if windows_host else "FALSE")]
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

    def test_removal_command_is_available_in_cmake314(self):
        result = self.stage(str(self.destination) + "\n", trace=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # cmake -E rm first appeared in 3.17, above the declared baseline.
        self.assertNotIn(" -E rm ", result.stderr)
        self.assertFalse((self.destination / "stale.dll").exists())

    def test_windows_current_tree_is_reused_without_any_destructive_write(self):
        (self.source / "nested/empty").mkdir(parents=True)
        (self.source / "nested/.runtime").write_bytes(b"hidden runtime")
        shutil.rmtree(self.destination)
        shutil.copytree(self.source, self.destination)
        paths = [self.destination, *self.destination.rglob("*")]
        for path in paths:
            os.utime(path, ns=(1000000000000000000, 1000000000000000000))
        original = {path: (path.stat().st_ino, path.stat().st_mtime_ns) for path in paths}
        for _ in range(2):
            result = self.stage(str(self.destination) + "\n", trace=True, windows_host=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn(" -E copy_directory ", result.stderr)
            self.assertNotIn(" -E remove", result.stderr)
            self.assertNotIn(" -E rm ", result.stderr)
            self.assertEqual({path: (path.stat().st_ino, path.stat().st_mtime_ns)
                              for path in paths}, original)

    def test_windows_current_tree_still_validates_source_and_overlap(self):
        shutil.rmtree(self.destination)
        shutil.copytree(self.source, self.destination)
        original_source = self.source
        self.source = self.destination
        result = self.stage(str(self.destination) + "\n", windows_host=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", result.stdout + result.stderr)
        self.source = original_source
        shutil.rmtree(self.source)
        result = self.stage(str(self.destination) + "\n", windows_host=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source directory is missing", result.stdout + result.stderr)
        self.assertEqual((self.destination / "adb.exe").read_bytes(), b"verified runtime")

    def test_windows_refresh_checks_complete_tree_types_and_bytes(self):
        (self.source / "nested/empty").mkdir(parents=True)
        (self.source / "nested/.runtime").write_bytes(b"hidden runtime")
        for mutation in ("changed", "missing", "stale", "empty-directory", "type", "missing-root"):
            with self.subTest(mutation=mutation):
                shutil.rmtree(self.destination)
                shutil.copytree(self.source, self.destination)
                if mutation == "changed":
                    target = self.destination / "adb.exe"
                    old_stat = target.stat()
                    target.write_bytes(b"corrupt! runtime")
                    self.assertEqual(target.stat().st_size, old_stat.st_size)
                    os.utime(target, ns=(old_stat.st_atime_ns, old_stat.st_mtime_ns))
                elif mutation == "missing":
                    (self.destination / "nested/.runtime").unlink()
                elif mutation == "stale":
                    (self.destination / ".stale").write_bytes(b"obsolete runtime")
                elif mutation == "empty-directory":
                    (self.destination / "stale-empty").mkdir()
                elif mutation == "type":
                    (self.destination / "nested/empty").rmdir()
                    (self.destination / "nested/empty").write_bytes(b"")
                else:
                    shutil.rmtree(self.destination)
                result = self.stage(str(self.destination) + "\n", trace=True, windows_host=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stderr.count(" -E copy_directory "), 1)
                self.assertEqual((self.destination / "adb.exe").read_bytes(), b"verified runtime")
                self.assertEqual((self.destination / "nested/.runtime").read_bytes(), b"hidden runtime")
                self.assertTrue((self.destination / "nested/empty").is_dir())
                self.assertEqual(sorted(str(path.relative_to(self.destination))
                                        for path in self.destination.rglob("*")),
                                 sorted(str(path.relative_to(self.source))
                                        for path in self.source.rglob("*")))

    def test_windows_equal_byte_leaf_symlink_is_replaced_not_reused(self):
        external = self.root / "external/helpers"
        shutil.copytree(self.source, external)
        shutil.rmtree(self.destination)
        self.alias(self.destination, external)
        result = self.stage(str(self.destination) + "\n", trace=True, windows_host=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr.count(" -E copy_directory "), 1)
        self.assertFalse(self.destination.is_symlink())
        self.assertEqual((external / "adb.exe").read_bytes(), b"verified runtime")

    def test_windows_equal_byte_nested_symlink_is_replaced_not_reused(self):
        (self.source / "nested").mkdir()
        (self.source / "nested/runtime.dll").write_bytes(b"runtime closure")
        external = self.root / "external"
        shutil.copytree(self.source, external)
        shutil.rmtree(self.destination)
        shutil.copytree(self.source, self.destination)
        shutil.rmtree(self.destination / "nested")
        self.alias(self.destination / "nested", external / "nested")
        result = self.stage(str(self.destination) + "\n", trace=True, windows_host=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr.count(" -E copy_directory "), 1)
        self.assertFalse((self.destination / "nested").is_symlink())
        self.assertEqual((external / "nested/runtime.dll").read_bytes(), b"runtime closure")

    @unittest.skipIf(os.name == "nt", "requires POSIX directory permissions")
    def test_failed_removal_is_fatal_and_does_not_copy_over_remaining_tree(self):
        if os.geteuid() == 0:
            self.skipTest("root bypasses directory permissions")
        self.destination.parent.chmod(0o500)
        try:
            result = self.stage(str(self.destination) + "\n", trace=True, windows_host=False)
        finally:
            self.destination.parent.chmod(0o700)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Removing stale ADB helpers failed", result.stdout + result.stderr)
        self.assertNotIn(" -E copy_directory ", result.stderr)
        self.assertTrue(self.destination.is_dir())
        self.assertEqual((self.source / "adb.exe").read_bytes(), b"verified runtime")

    @unittest.skipIf(os.name == "nt", "requires POSIX execute permissions")
    def test_non_windows_refresh_preserves_source_permission_changes(self):
        helper = self.source / "adb.exe"
        helper.chmod(0o600)
        result = self.stage(str(self.destination) + "\n", windows_host=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        helper.chmod(0o755)
        result = self.stage(str(self.destination) + "\n", windows_host=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.destination / "adb.exe").stat().st_mode & 0o777, 0o755)

    @unittest.skipUnless(os.name == "nt", "requires Windows file-sharing semantics")
    def test_windows_locked_current_helper_is_reused_but_changed_helper_fails(self):
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                                      wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        result = self.stage(str(self.destination) + "\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        handle = kernel.CreateFileW(str(self.destination / "adb.exe"), 0x80000000,
                                    1, None, 3, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value, ctypes.get_last_error())
        try:
            result = self.stage(str(self.destination) + "\n", trace=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn(" -E copy_directory ", result.stderr)
            (self.source / "adb.exe").write_bytes(b"changed runtime")
            result = self.stage(str(self.destination) + "\n")
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Removing stale ADB helpers failed", result.stdout + result.stderr)
            self.assertEqual((self.destination / "adb.exe").read_bytes(), b"verified runtime")
        finally:
            self.assertTrue(kernel.CloseHandle(handle))

    def test_destination_access_path_overlap_is_rejected_before_mutation(self):
        for order in ("forward", "reverse"):
            with self.subTest(order=order):
                first = self.root / order / "runtime/helpers"
                outside = self.root / order / "outside"
                second = first / "output-link/helpers"
                first.mkdir(parents=True)
                (outside / "helpers").mkdir(parents=True)
                sentinel = outside / "portable-executable"
                sentinel.write_bytes(b"preexisting executable")
                (first / "keep-first").write_bytes(b"first output")
                (outside / "helpers/keep-second").write_bytes(b"second output")
                self.alias(first / "output-link", outside)
                destinations = (first, second) if order == "forward" else (second, first)
                result = self.stage("".join(str(path) + "\n" for path in destinations))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("destination paths overlap", result.stdout + result.stderr)
                self.assertTrue((first / "output-link").is_symlink())
                self.assertEqual(sentinel.read_bytes(), b"preexisting executable")
                self.assertEqual((first / "keep-first").read_bytes(), b"first output")
                self.assertEqual((outside / "helpers/keep-second").read_bytes(), b"second output")

    def test_destination_prefix_siblings_remain_independent(self):
        second = self.destination.parent / "helpers-sibling/app/helpers"
        result = self.stage(str(self.destination) + "\n" + str(second) + "\n", trace=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stderr.count(" -E copy_directory "), 2)
        self.assertEqual((second / "adb.exe").read_bytes(), b"verified runtime")

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

    def alias(self, link, target):
        try:
            link.symlink_to(target, target_is_directory=True)
        except OSError:
            if os.name == "nt":
                self.skipTest("directory symlinks are unavailable")
            raise

    def test_destination_ancestor_of_source_is_rejected_before_any_removal(self):
        ancestor = self.root / "protected/helpers"
        self.source = ancestor / "input/helpers"
        self.source.mkdir(parents=True)
        (self.source / "adb.exe").write_bytes(b"verified runtime")
        sentinel = ancestor / "keep-input"
        sentinel.write_bytes(b"protected input")
        result = self.stage(str(self.destination) + "\n" + str(ancestor) + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", (result.stdout + result.stderr).lower())
        self.assertEqual((self.source / "adb.exe").read_bytes(), b"verified runtime")
        self.assertEqual(sentinel.read_bytes(), b"protected input")
        self.assertTrue((self.destination / "stale.dll").exists())

    def test_destination_descendant_of_source_is_rejected_before_any_removal(self):
        # A file blocker prevents recursive copying on the old implementation,
        # while still exercising a selected path canonically inside the source.
        blocker = self.source / "blocked"
        blocker.write_bytes(b"protected input")
        result = self.stage(str(self.destination) + "\n" + str(blocker / "helpers") + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", (result.stdout + result.stderr).lower())
        self.assertEqual((self.source / "adb.exe").read_bytes(), b"verified runtime")
        self.assertEqual(blocker.read_bytes(), b"protected input")
        self.assertTrue((self.destination / "stale.dll").exists())

    def test_nonexistent_descendant_through_parent_alias_is_rejected(self):
        blocker = self.source / "blocked"
        blocker.write_bytes(b"protected input")
        alias = self.root / "source-alias"
        self.alias(alias, self.source)
        result = self.stage(str(self.destination) + "\n" + str(alias / "blocked/helpers") + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", (result.stdout + result.stderr).lower())
        self.assertEqual((self.source / "adb.exe").read_bytes(), b"verified runtime")
        self.assertTrue((self.destination / "stale.dll").exists())

    def test_source_alias_inside_destination_is_rejected(self):
        ancestor = self.root / "protected/helpers"
        actual_source = ancestor / "input/helpers"
        actual_source.mkdir(parents=True)
        (actual_source / "adb.exe").write_bytes(b"verified runtime")
        alias = self.root / "verified-input-alias"
        self.alias(alias, actual_source)
        self.source = alias
        result = self.stage(str(ancestor) + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", (result.stdout + result.stderr).lower())
        self.assertTrue(alias.is_symlink())
        self.assertEqual((actual_source / "adb.exe").read_bytes(), b"verified runtime")

    def test_dot_segment_source_alias_is_rejected_without_deleting_input(self):
        intermediate = self.source.parent / "intermediate"
        intermediate.mkdir()
        result = self.stage(str(intermediate / "../helpers") + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", (result.stdout + result.stderr).lower())
        self.assertEqual((self.source / "adb.exe").read_bytes(), b"verified runtime")

    def test_source_leaf_alias_is_rejected_before_removing_the_link(self):
        shutil.rmtree(self.destination)
        self.alias(self.destination, self.source)
        result = self.stage(str(self.destination) + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", (result.stdout + result.stderr).lower())
        self.assertTrue(self.destination.is_symlink())
        self.assertEqual((self.source / "adb.exe").read_bytes(), b"verified runtime")

    def test_broken_leaf_alias_into_source_descendant_is_rejected(self):
        shutil.rmtree(self.destination)
        self.alias(self.destination, self.source / "missing/helpers")
        result = self.stage(str(self.destination) + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", (result.stdout + result.stderr).lower())
        self.assertTrue(self.destination.is_symlink())
        self.assertEqual((self.source / "adb.exe").read_bytes(), b"verified runtime")

    def test_destination_cannot_delete_an_alias_used_to_access_the_source(self):
        alias = self.destination / "input-alias"
        self.alias(alias, self.source)
        actual_source = self.source
        self.source = alias
        result = self.stage(str(self.destination) + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", (result.stdout + result.stderr).lower())
        self.assertTrue(alias.is_symlink())
        self.assertEqual((actual_source / "adb.exe").read_bytes(), b"verified runtime")
        self.assertTrue((self.destination / "stale.dll").exists())

    def test_deletion_uses_original_leaf_entry_not_resolved_alias_target(self):
        external = self.root / "external/helpers"
        external.mkdir(parents=True)
        sentinel = external / "must-remain"
        sentinel.write_bytes(b"unrelated external data")
        shutil.rmtree(self.destination)
        self.alias(self.destination, external)
        result = self.stage(str(self.destination) + "\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sentinel.read_bytes(), b"unrelated external data")
        self.assertFalse(self.destination.is_symlink())
        self.assertEqual((self.destination / "adb.exe").read_bytes(), b"verified runtime")

    @unittest.skipUnless(os.name == "nt", "Windows filesystem path comparison")
    def test_windows_case_alias_of_source_is_rejected(self):
        destination = str(self.source.parent).upper() + "/helpers"
        result = self.stage(destination + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("overlap", (result.stdout + result.stderr).lower())
        self.assertEqual((self.source / "adb.exe").read_bytes(), b"verified runtime")

    def test_filesystem_staging_failure_is_not_ignored(self):
        blocker = self.root / "not a directory"
        blocker.write_bytes(b"keep this file")
        result = self.stage(str(blocker / "helpers") + "\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout + result.stderr, r"(?:Removing|Copying).*failed")
        self.assertEqual(blocker.read_bytes(), b"keep this file")


if __name__ == "__main__":
    unittest.main()
