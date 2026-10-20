#!/usr/bin/env python3
"""Deterministic contracts for native package overlap and its artifact rendezvous."""
import importlib.util
import copy
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))


def assert_cache_directory(case, cache, key, expected):
    prefix = key + ":PATH="
    values = [line[len(prefix):] for line in cache.splitlines() if line.startswith(prefix)]
    case.assertEqual(len(values), 1, "expected one exact cache directory entry: " + key)
    # CMake expands Windows short paths; spelling is not filesystem identity.
    case.assertTrue(Path(values[0]).samefile(expected), key + " names a different directory")


class CachePathIdentityTests(unittest.TestCase):
    def test_cache_directory_compares_identity_not_path_spelling(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = root / "verified helper"
            expected.mkdir()
            (root / "intermediate").mkdir()
            alias = root / "intermediate" / ".." / expected.name
            cache = "KLOGG_ADB_HELPER_ARTIFACT_ROOT:PATH=" + expected.as_posix() + "\n"
            for path in (expected, alias):
                with self.subTest(path=path):
                    assert_cache_directory(self, cache, "KLOGG_ADB_HELPER_ARTIFACT_ROOT", path)
            other = root / "different" / expected.name
            other.mkdir(parents=True)
            with self.assertRaises(AssertionError):
                assert_cache_directory(self, cache, "KLOGG_ADB_HELPER_ARTIFACT_ROOT", other)

    def test_cache_directory_requires_one_existing_exact_entry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            entry = "HELPER:PATH=" + root.as_posix() + "\n"
            for invalid in ("", "//" + entry, "OTHER_" + entry, entry + entry,
                            "HELPER:PATH=" + str(root / "missing") + "\n"):
                with self.subTest(cache=invalid), self.assertRaises((AssertionError, OSError)):
                    assert_cache_directory(self, invalid, "HELPER", root)


@unittest.skipUnless(os.name == "posix" and shutil.which("ninja") and shutil.which("cmake"),
                     "native Ninja/CMake fixtures require POSIX build tools")
class NativeOverlapTests(unittest.TestCase):
    def setUp(self):
        self.spec = importlib.util.find_spec("ci_native_overlap")
        self.assertIsNotNone(self.spec, "native overlap orchestration is not implemented")
        import ci_native_overlap
        self.module = ci_native_overlap
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_object_wrapper_only_builds_reachable_objects_and_preserves_ninja_log(self):
        build = self.root / "build"
        build.mkdir()
        (build / "build.ninja").write_text(
            "rule touch\n  command = touch $out\n"
            "build a.o: touch\nbuild excluded.o: touch\nbuild app: touch a.o\n"
            "build ci_build: phony app\n")
        database = [{"directory": str(build), "file": "a.cpp", "output": name}
                    for name in ("a.o", "excluded.o")]
        (build / "compile_commands.json").write_text(json.dumps(database))
        command = self.module.object_command(build)
        subprocess.run(command, cwd=build, check=True, capture_output=True)
        self.assertTrue((build / "a.o").exists())
        self.assertFalse((build / "app").exists())
        self.assertFalse((build / "excluded.o").exists())
        before = (build / ".ninja_log").read_text()
        subprocess.run(["ninja", "ci_build"], cwd=build, check=True, capture_output=True)
        self.assertTrue((build / ".ninja_log").read_text().startswith(before))

    def test_preparation_links_test_aggregate_but_not_package_app(self):
        build = self.root / "build"
        build.mkdir()
        (build / "build.ninja").write_text(
            "rule touch\n  command = touch $out\n"
            "build a.o: touch\nbuild testbin: touch a.o\n"
            "build tests/klogg_test_build: phony testbin\nbuild app: touch a.o\n"
            "build ci_build: phony app tests/klogg_test_build\n")
        (build / "compile_commands.json").write_text(json.dumps([
            {"directory": str(build), "file": "a.cpp", "output": "a.o"}]))
        subprocess.run(self.module.object_command(build), check=True, capture_output=True)
        self.assertTrue((build / "testbin").exists())
        self.assertFalse((build / "app").exists())

    def test_preparation_rejects_test_aggregate_that_would_link_package_app(self):
        build = self.root / "build"
        build.mkdir()
        (build / "build.ninja").write_text(
            "rule touch\n  command = touch $out\nbuild a.o: touch\n"
            "build output/klogg.app/Contents/MacOS/klogg: touch a.o\n"
            "build tests/klogg_test_build: phony output/klogg.app/Contents/MacOS/klogg\n"
            "build ci_build: phony tests/klogg_test_build\n")
        (build / "compile_commands.json").write_text(json.dumps([
            {"directory": str(build), "file": "a.cpp", "output": "a.o"}]))
        with self.assertRaisesRegex(ValueError, "package app link"):
            self.module.object_command(build)

    def test_object_wrapper_decodes_ninja_shell_quoted_inputs(self):
        build = self.root / "space build"
        build.mkdir()
        (build / "build.ninja").write_text(
            "rule touch\n  command = touch $out\n"
            "build a$ b.o: touch\nbuild app: touch a$ b.o\nbuild ci_build: phony app\n")
        (build / "compile_commands.json").write_text(json.dumps([
            {"directory": str(build), "file": "a.cpp", "output": "a b.o"}]))
        subprocess.run(self.module.object_command(build), check=True, capture_output=True)
        self.assertTrue((build / "a b.o").exists())
        self.assertFalse((build / "app").exists())

    def test_object_target_cannot_escape_build_root(self):
        build = self.root / "build"
        build.mkdir()
        (build / "compile_commands.json").write_text(json.dumps([
            {"directory": str(build), "file": "x.cpp", "output": "../outside.o"}]))
        with self.assertRaisesRegex(ValueError, "outside build root"):
            self.module.object_targets(build, {"../outside.o"})

    def test_real_cmake_two_phase_retains_objects_and_requires_finalization_receipt(self):
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\nproject(Overlap C CXX)\n"
            "option(KLOGG_ADB_HELPER_REQUIRED \"Native ADB\" OFF)\n"
            "option(KLOGG_ENABLE_IOS_NATIVE_STACK \"Native iOS\" OFF)\n"
            "configure_file(config.h.in config.h)\n"
            "add_library(core STATIC core.c)\n"
            "target_include_directories(core PRIVATE ${CMAKE_CURRENT_BINARY_DIR})\n"
            "add_executable(app main.cpp)\ntarget_link_libraries(app PRIVATE core)\n"
            "add_executable(core_tests main.cpp)\ntarget_link_libraries(core_tests PRIVATE core)\n"
            "add_custom_target(klogg_test_build DEPENDS core_tests)\n"
            "add_custom_target(ci_build DEPENDS app klogg_test_build)\n"
            "include(CheckFunctionExists)\nCHECK_FUNCTION_EXISTS(mmap HAVE_MMAP)\n")
        (self.root / "config.h.in").write_text("#cmakedefine HAVE_MMAP\n")
        (self.root / "core.c").write_text('#include "config.h"\nint core(void) { return 7; }\n')
        (self.root / "main.cpp").write_text('extern "C" int core(void);\nint main() { return core() == 7 ? 0 : 1; }\n')
        build = self.root / "build"
        env = {"RUNNER_OS": "macOS", "GITHUB_REPOSITORY": "owner/repo", "GITHUB_RUN_ID": "17",
               "GITHUB_RUN_ATTEMPT": "1", "GITHUB_SHA": "a" * 40, "GITHUB_EVENT_NAME": "push",
               "KLOGG_IOS_ARCHITECTURE": "x86_64", "KLOGG_CONFIG_ARCH": "x64",
               "KLOGG_LABEL": "fixture", "KLOGG_CONFIG_PACKAGE_TAG": "native",
               "KLOGG_CI_ENVIRONMENT_IDENTITY": "fixture-toolchain"}
        options = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"]
        native = ["-DKLOGG_ADB_HELPER_REQUIRED=ON", "-DKLOGG_ENABLE_IOS_NATIVE_STACK=ON",
                  "-DKLOGG_ADB_HELPER_ARTIFACT_ROOT=/adb", "-DKLOGG_IOS_NATIVE_STACK_ROOT=/ios"]
        with mock.patch.dict("os.environ", env):
            with mock.patch.object(self.module.subprocess, "run", wraps=subprocess.run) as commands:
                self.module.execute("prepare", self.root, build, options + native)
            configure_calls = [call.args[0] for call in commands.call_args_list
                               if call.args[0][0] == "cmake" and "-S" in call.args[0]]
            self.assertFalse((build / "app").exists())
            obj = build / "CMakeFiles/app.dir/main.cpp.o"
            stamp = obj.stat().st_mtime_ns
            retained = [build / "CMakeFiles/core.dir/core.c.o", build / "libcore.a", build / "core_tests"]
            retained_stamps = [path.stat().st_mtime_ns for path in retained]
            with self.assertRaisesRegex(ValueError, "receipt"):
                self.module.execute("finalize", self.root, build, options + native)
            receipt = self.root / "receipt.json"
            names = {"adb-helper-macos-x86_64": "Source-built ADB helper macos-x86_64",
                     "ios-native-x86_64": "iOS native stack x86_64",
                     "adb-helper-package-support": "Build ADB helper legal assets"}
            artifacts = {name: {"producer_name": producer, "producer_attempt": 1,
                                "producer_id": index + 1, "artifact_id": index + 10}
                         for index, (name, producer) in enumerate(names.items())}
            receipt.write_text(json.dumps({"schema_version": 1,
                "identity": self.module.rendezvous.identity(), "artifacts": artifacts}))
            self.module.execute("finalize", self.root, build, options + native, receipt)
            self.assertTrue((build / "app").exists())
            self.assertEqual(obj.stat().st_mtime_ns, stamp)
            self.assertEqual([path.stat().st_mtime_ns for path in retained], retained_stamps)
            self.assertEqual(len(configure_calls), 2)
            self.assertEqual(configure_calls[0], configure_calls[1])
            state = json.loads((build / self.module.STATE).read_text())
            self.assertEqual(state["status"], "finalized")
            report = json.loads((build / "ci-build-metrics-17-1.json").read_text())
            self.assertEqual(report["measurement_scope"], "phased-build")
            self.assertEqual(report["build_wall_ms"], sum(part["build_wall_ms"] for part in report["phases"].values()))
            self.assertEqual([part["purpose"] for part in report["phases"]["prepare"]["configure"]["passes"]],
                             ["initial", "precompile-stabilization"])
            self.assertEqual(len(report["phases"]["finalize"]["configure"]["passes"]), 1)
            for phase in report["phases"].values():
                self.assertEqual(phase["configure"]["wall_ms"], sum(part["wall_ms"] for part in phase["configure"]["passes"]))
                self.assertEqual(phase["provenance"]["native_configure_strategy"], self.module.CONFIGURE_STRATEGY)
            with self.assertRaisesRegex(ValueError, "prepared build"):
                self.module.execute("finalize", self.root, build, options + native, receipt)

    def test_ninja_escaping(self):
        self.assertEqual(self.module.ninja_escape("a b:c$d"), "a$ b$:c$$d")
        with self.assertRaises(ValueError):
            self.module.ninja_escape("a\nb")

    def test_phase_option_contract(self):
        common = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo"]
        self.module.validate_options(common, "prepare")
        with self.assertRaisesRegex(ValueError, "native"):
            self.module.validate_options(common + ["-DKLOGG_ENABLE_IOS_NATIVE_STACK=ON"], "prepare")
        with self.assertRaisesRegex(ValueError, "required"):
            self.module.validate_options(common, "finalize")
        native = ["-DKLOGG_ADB_HELPER_REQUIRED=ON", "-DKLOGG_ENABLE_IOS_NATIVE_STACK=ON",
                  "-DKLOGG_ADB_HELPER_ARTIFACT_ROOT=/adb", "-DKLOGG_IOS_NATIVE_STACK_ROOT=/ios"]
        self.module.validate_options(common + native, "finalize")
        self.assertEqual(self.module.common_options(common + native), common)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.module.validate_options(common + native + [native[0]], "finalize")


@unittest.skipUnless(os.environ.get("RUNNER_OS") == "Windows" or (shutil.which("ninja") and shutil.which("cmake")),
                     "requires native Ninja/CMake on local hosts")
class WindowsNativeOverlapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        super().setUpClass()
        clocks = {}
        for name in ("monotonic", "perf_counter"):
            info = time.get_clock_info(name)
            clocks[name] = {"implementation": info.implementation, "resolution": info.resolution,
                            "monotonic": info.monotonic}
        print("Native overlap runtime clocks: " + json.dumps(clocks, sort_keys=True), flush=True)

    def setUp(self):
        if os.environ.get("RUNNER_OS") == "Windows":
            for tool in ("cmake", "ninja", "cl"):
                self.assertIsNotNone(shutil.which(tool), "the hosted native guard requires " + tool)
            self.assertEqual(os.name, "nt", "the hosted guard requires native Windows Python")
        import ci_native_overlap
        self.module = ci_native_overlap
        temporary = tempfile.TemporaryDirectory(prefix="native overlap ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_windows_cmake_options_preserve_backslashes_and_quoted_values(self):
        options = r'-G Ninja -DROOT=D:\a\klogg\prefetch -DSDK="C:\Program Files\sdk" -DCMAKE_INSTALL_PREFIX=/usr'
        self.assertEqual(self.module.parse_options(options, windows=True), [
            "-G", "Ninja", r"-DROOT=D:\a\klogg\prefetch", r"-DSDK=C:\Program Files\sdk",
            "-DCMAKE_INSTALL_PREFIX=/usr"])
        with self.assertRaises(ValueError):
            self.module.parse_options('-DSDK="unterminated', windows=True)
        self.assertEqual(self.module.parse_options("-G Ninja '-DNAME=two words'"),
                         ["-G", "Ninja", "-DNAME=two words"])

    def test_windows_profile_requires_only_adb_and_current_legal_support(self):
        profile = self.module.rendezvous.profile("Windows", "x86_64")
        self.assertEqual(profile["gates"], ["KLOGG_ADB_HELPER_REQUIRED"])
        self.assertEqual(profile["artifacts"], {
            "adb-helper-windows-x86_64": "Source-built ADB helper windows-x86_64",
            "adb-helper-package-support": "Build ADB helper legal assets"})
        for platform, architecture in (("Windows", "arm64"), ("Windows", "x86"), ("Linux", "x86_64")):
            with self.subTest(platform=platform, architecture=architecture), self.assertRaises(ValueError):
                self.module.rendezvous.profile(platform, architecture)
        native = ["-DKLOGG_ADB_HELPER_REQUIRED=ON", "-DKLOGG_ADB_HELPER_ARTIFACT_ROOT=/verified",
                  "-DKLOGG_ADB_HELPER_TARGET=windows-x86_64"]
        self.module.validate_options(native, "finalize", platform="Windows")
        for options in (native + ["-DKLOGG_ENABLE_IOS_NATIVE_STACK=ON"], native[:-1],
                        [item.replace("windows-x86_64", "macos-x86_64") for item in native]):
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.module.validate_options(options, "finalize", platform="Windows")

    def test_windows_ninja_prepares_msvc_objects_but_never_package_executables(self):
        build = self.root / "space build"
        build.mkdir()
        (build / "CMakeFiles/app.dir").mkdir(parents=True)
        cmake = Path(shutil.which("cmake")).as_posix()
        (build / "build.ninja").write_text(
            'rule touch\n  command = "' + cmake + '" -E touch $out\n'
            "build CMakeFiles/app.dir/a$ b.obj: touch\n"
            "build klogg.exe: touch CMakeFiles/app.dir/a$ b.obj\n"
            "build ci_build: phony klogg.exe\n")
        (build / "compile_commands.json").write_text(json.dumps([{
            "directory": str(build), "file": "a.cpp",
            "command": 'cl.exe /Fo"CMakeFiles\\app.dir\\a b.obj" /c a.cpp'}]))
        command = self.module.object_command(build, windows=True)
        subprocess.run(command, check=True, capture_output=True)
        self.assertTrue((build / "CMakeFiles/app.dir/a b.obj").is_file())
        self.assertFalse((build / "klogg.exe").exists())
        with (build / "build.ninja").open("a") as stream:
            stream.write("build klogg_test_build: phony klogg.exe\n")
        with self.assertRaisesRegex(ValueError, "package app link"):
            self.module.object_command(build, windows=True)
        (build / "compile_commands.json").write_text(json.dumps([{
            "directory": str(build), "file": "a.cpp", "command": r"cl.exe /Fo..\outside.obj /c a.cpp"}]))
        with self.assertRaisesRegex(ValueError, "outside build root"):
            self.module.object_targets(build, {"../outside.obj"}, windows=True)

    def test_native_windows_two_phase_keeps_test_links_and_defers_app_resources(self):
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\nproject(Overlap C CXX)\n"
            'option(KLOGG_ADB_HELPER_REQUIRED "ADB gate" OFF)\n'
            'option(KLOGG_ENABLE_IOS_NATIVE_STACK "iOS gate" OFF)\n'
            'set(KLOGG_ADB_HELPER_ARTIFACT_ROOT "" CACHE PATH "Verified artifact")\n'
            'set(KLOGG_ADB_HELPER_TARGET "" CACHE STRING "Native target")\n'
            'if(KLOGG_ADB_HELPER_REQUIRED AND NOT EXISTS "${KLOGG_ADB_HELPER_ARTIFACT_ROOT}/verified")\n'
            '  message(FATAL_ERROR "verified helper missing")\nendif()\n'
            "add_library(core STATIC core.c)\n"
            "add_library(resources OBJECT qrc_app_translations.cpp)\n"
            "add_executable(klogg main.cpp $<TARGET_OBJECTS:resources>)\n"
            "add_executable(klogg_portable main.cpp $<TARGET_OBJECTS:resources>)\n"
            "add_executable(stable_tests main.cpp)\n"
            "foreach(target klogg klogg_portable stable_tests)\n"
            '  set_target_properties(${target} PROPERTIES SUFFIX ".exe")\n'
            "  target_link_libraries(${target} PRIVATE core)\nendforeach()\n"
            "add_custom_target(klogg_test_build DEPENDS stable_tests)\n"
            "add_custom_target(ci_build DEPENDS klogg klogg_portable klogg_test_build)\n"
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/fixture-paths.txt" CONTENT '
            '"$<TARGET_FILE:core>\\n$<TARGET_FILE:stable_tests>\\n$<TARGET_FILE:klogg>\\n$<TARGET_FILE:klogg_portable>\\n")\n')
        (self.root / "core.c").write_text("int core(void) { return 7; }\n")
        (self.root / "main.cpp").write_text('extern "C" int core(void);\nint main() { return core() == 7 ? 0 : 1; }\n')
        (self.root / "qrc_app_translations.cpp").write_text("int resource(void) { return 1; }\n")
        build, artifact = self.root / "build", self.root / "verified helper"
        env = {"RUNNER_OS": "Windows", "GITHUB_REPOSITORY": "owner/repo", "GITHUB_RUN_ID": "17",
               "GITHUB_RUN_ATTEMPT": "2", "GITHUB_SHA": "a" * 40, "GITHUB_EVENT_NAME": "push",
               "KLOGG_ADB_HELPER_TARGET": "windows-x86_64", "KLOGG_CONFIG_ARCH": "x64",
               "KLOGG_LABEL": "fixture", "KLOGG_CONFIG_PACKAGE_TAG": "native",
               "KLOGG_CI_ENVIRONMENT_IDENTITY": "fixture-toolchain",
               "KLOGG_WINDOWS_OBJECT_CACHE": "disabled", "KLOGG_OBJECT_CACHE_TOOL": "sccache",
               # try_compile can reinitialize launchers from the environment
               # even when the parent cache has explicit empty launcher flags.
               "CMAKE_C_COMPILER_LAUNCHER": "", "CMAKE_CXX_COMPILER_LAUNCHER": "",
               "CMAKE_C_LINKER_LAUNCHER": "", "CMAKE_CXX_LINKER_LAUNCHER": "",
               "CC": "", "CXX": ""}
        options = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                   "-DCMAKE_C_COMPILER_LAUNCHER=", "-DCMAKE_CXX_COMPILER_LAUNCHER=",
                   "-DCMAKE_INSTALL_PREFIX=/usr",
                   "-DKLOGG_ADB_HELPER_REQUIRED=ON", "-DKLOGG_ADB_HELPER_TARGET=windows-x86_64",
                   "-DKLOGG_ADB_HELPER_ARTIFACT_ROOT=" + str(artifact)]
        encoded = subprocess.list2cmdline(options)
        self.assertEqual(self.module.parse_options(encoded, windows=True), options)
        options = self.module.parse_options(encoded, windows=True)
        probe = self.root / "prefix-probe.cmake"
        prefix = self.root / "prefix.txt"
        probe.write_text('get_filename_component(value "${VALUE}" ABSOLUTE)\n'
                         'file(WRITE "${OUTPUT}" "${value}")\n')
        subprocess.run(["sh", "-c", 'exec "$@"', "prefix-probe", "cmake", "-DVALUE=/usr",
                        "-DOUTPUT=" + str(prefix), "-P", str(probe)], check=True, capture_output=True)
        inherited = {"CMAKE_C_COMPILER_LAUNCHER": "must-not-run-fixture-cache",
                     "CMAKE_CXX_COMPILER_LAUNCHER": "must-not-run-fixture-cache"}
        # Do not emit fixture run IDs as application metrics in the hosted job log.
        with mock.patch.dict(os.environ, inherited), mock.patch.dict(os.environ, env), \
                contextlib.redirect_stdout(io.StringIO()):
            with mock.patch.object(self.module.subprocess, "run", wraps=subprocess.run) as commands:
                self.module.execute("prepare", self.root, build, options)
            configure = [call.args[0] for call in commands.call_args_list if call.args[0][0] == "sh"]
            self.assertEqual(len(configure), 2)
            self.assertEqual(configure[0][:4], ["sh", "-c", 'exec "$@"', "klogg-native-configure"])
            self.assertEqual(configure[0], configure[1])
            self.assertFalse(any(call.args[0][0] in ("sccache", "ccache") for call in commands.call_args_list))
            cache = (build / "CMakeCache.txt").read_text()
            self.assertIn("CMAKE_INSTALL_PREFIX:PATH=" + prefix.read_text(), cache)
            for language in ("C", "CXX"):
                self.assertRegex(cache, "(?m)^CMAKE_" + language + "_COMPILER_LAUNCHER:[^=]+=\\s*$")
            paths = [Path(path) for path in (build / "fixture-paths.txt").read_text().splitlines()]
            retained = [path.stat().st_mtime_ns for path in paths[:2]]
            self.assertFalse(any(path.exists() for path in paths[2:]))
            database = json.loads((build / "compile_commands.json").read_text())
            resource = next(entry for entry in database if "qrc_app_translations.cpp" in entry["file"])
            resource_object = build / self.module.metrics.command_output(resource, os.name == "nt")
            self.assertFalse(resource_object.exists())
            with self.assertRaisesRegex(ValueError, "receipt"):
                self.module.execute("finalize", self.root, build, options)
            artifact.mkdir()
            (artifact / "verified").write_bytes(b"fresh artifact verification")
            profile = self.module.rendezvous.profile("Windows", "x86_64")
            receipt = self.root / "receipt.json"
            receipt.write_text(json.dumps({"schema_version": 1, "identity": self.module.rendezvous.identity(),
                "artifacts": {name: {"producer_name": producer, "producer_attempt": 1,
                                     "producer_id": index + 1, "artifact_id": index + 10}
                              for index, (name, producer) in enumerate(profile["artifacts"].items())}}))
            with self.assertRaisesRegex(ValueError, "prepared build"):
                self.module.execute("finalize", self.root, build, options + ["-DCMAKE_CXX_FLAGS=-DCHANGED"], receipt)
            with mock.patch.dict(os.environ, {"GITHUB_RUN_ATTEMPT": "3"}), self.assertRaisesRegex(ValueError, "prepared build"):
                self.module.execute("finalize", self.root, build, options, receipt)
            self.module.execute("finalize", self.root, build, options, receipt)
            final_cache = (build / "CMakeCache.txt").read_text()
            self.assertIn("KLOGG_ADB_HELPER_TARGET:STRING=windows-x86_64", final_cache)
            assert_cache_directory(self, final_cache, "KLOGG_ADB_HELPER_ARTIFACT_ROOT", artifact)
            self.assertEqual([path.stat().st_mtime_ns for path in paths[:2]], retained)
            self.assertTrue(all(path.is_file() for path in paths))
            rows = [line.split("\t") for line in (build / ".ninja_log").read_text().splitlines()[1:]]
            for path in paths:
                relative = path.relative_to(build.resolve()).as_posix()
                self.assertEqual(sum(row[3].replace("\\", "/") == relative for row in rows), 1, relative)
            report = json.loads((build / "ci-build-metrics-17-2.json").read_text())
            self.assertEqual(report["object_cache"], "disabled")
            self.assertIsNone(report["cache_tool"])
            self.assertEqual(report["provenance"]["native_platform"], "Windows")
            self.assertEqual(report["provenance"]["cmake_options"]["KLOGG_ENABLE_IOS_NATIVE_STACK"], "OFF")
            with self.assertRaisesRegex(ValueError, "prepared build"):
                self.module.execute("finalize", self.root, build, options, receipt)


class NativeHostedGuardTests(unittest.TestCase):
    def test_native_guard_reports_actual_runtime_clocks_once_per_class(self):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            WindowsNativeOverlapTests.setUpClass()
        lines = output.getvalue().splitlines()
        self.assertEqual(len(lines), 1)
        prefix = "Native overlap runtime clocks: "
        self.assertTrue(lines[0].startswith(prefix))
        clocks = json.loads(lines[0][len(prefix):])
        for name in ("monotonic", "perf_counter"):
            actual = time.get_clock_info(name)
            self.assertEqual(clocks[name], {"implementation": actual.implementation,
                "resolution": actual.resolution, "monotonic": actual.monotonic})

    def test_hosted_windows_missing_tools_fails_instead_of_skipping_native_proof(self):
        with mock.patch.dict(os.environ, {"RUNNER_OS": "Windows"}), \
                mock.patch("shutil.which", return_value=None), mock.patch.object(sys, "path", sys.path[:]):
            spec = importlib.util.spec_from_file_location("native_missing_tools", Path(__file__))
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            case_type = module.WindowsNativeOverlapTests
            self.assertFalse(getattr(case_type, "__unittest_skip__", False))
            case = case_type("test_windows_profile_requires_only_adb_and_current_legal_support")
            with self.assertRaisesRegex(AssertionError, "cmake"):
                case.setUp()


class PhaseObservationTests(unittest.TestCase):
    def reports(self):
        provenance = {"sha": "a" * 40, "run_id": "17", "run_attempt": "2", "arch": "x64",
                      "config": "intel/Release", "environment": "macOS-image", "source_root": "/repo",
                      "build_type": "RelWithDebInfo", "compilers": {"CXX": {"id": "AppleClang", "version": "21"}},
                      "cmake_options": {"KLOGG_ADB_HELPER_REQUIRED": "OFF",
                                        "KLOGG_ENABLE_IOS_NATIVE_STACK": "OFF", "KLOGG_USE_LTO": "ON"}}
        prepare = {"schema_version": 1, "phase": "prepare", "measurement_scope": "phase",
                   "build_wall_ms": 100, "object_cache": "measured", "cache_tool": "ccache",
                   "cache_deltas": {"direct_cache_hit": 3, "preprocessed_cache_hit": 1, "cache_miss": 2},
                   "provenance": provenance, "compilation_state": "measured", "compilation": {"edges": 6}}
        finalize = copy.deepcopy(prepare)
        finalize.update(phase="finalize", build_wall_ms=20, compilation_state="unknown")
        finalize.pop("compilation")
        for key in ("KLOGG_ADB_HELPER_REQUIRED", "KLOGG_ENABLE_IOS_NATIVE_STACK"):
            finalize["provenance"]["cmake_options"][key] = "ON"
        return prepare, finalize

    def test_aggregate_preserves_phase_views_without_inventing_ninja_union(self):
        import ci_build_observation as observation
        self.assertTrue(hasattr(observation, "aggregate_phases"), "whole phased-build observation is missing")
        prepare, finalize = self.reports()
        result = observation.aggregate_phases(prepare, finalize)
        self.assertEqual(result["measurement_scope"], "phased-build")
        self.assertEqual(result["build_wall_ms"], 120)
        self.assertIn("excludes configure", result["build_wall_definition"])
        self.assertEqual(result["cache_deltas"], {"direct_cache_hit": 6, "preprocessed_cache_hit": 2, "cache_miss": 4})
        self.assertEqual(result["provenance"], finalize["provenance"])
        self.assertEqual(result["phases"], {"prepare": prepare, "finalize": finalize})
        self.assertNotIn("compilation", result)
        self.assertNotIn("categories", result)
        self.assertEqual(result["phases"]["finalize"]["compilation_state"], "unknown")

    def test_aggregate_rejects_mixed_identity_and_unexpected_compile_options(self):
        import ci_build_observation as observation
        self.assertTrue(hasattr(observation, "aggregate_phases"), "whole phased-build observation is missing")
        for key, value in (("sha", "b" * 40), ("run_id", "18"), ("run_attempt", "3"),
                           ("environment", "other"), ("source_root", "/other"), ("arch", "arm64"),
                           ("compilers", {"CXX": {"id": "AppleClang", "version": "20"}})):
            with self.subTest(key=key):
                prepare, finalize = self.reports()
                finalize["provenance"][key] = value
                with self.assertRaises(ValueError):
                    observation.aggregate_phases(prepare, finalize)
        for key in ("KLOGG_USE_LTO", "KLOGG_ENABLE_IOS_NATIVE_STACK"):
            prepare, finalize = self.reports()
            finalize["provenance"]["cmake_options"][key] = "OFF"
            with self.assertRaises(ValueError):
                observation.aggregate_phases(prepare, finalize)
        prepare, finalize = self.reports()
        for phase in (prepare, finalize):
            phase["provenance"].pop("source_root")
        with self.assertRaises(ValueError):
            observation.aggregate_phases(prepare, finalize)
        with self.assertRaises(ValueError):
            observation.aggregate_phases(*reversed(self.reports()))
        with self.assertRaises(ValueError):
            observation.aggregate_phases(self.reports()[0], None)

    def test_windows_phase_aggregation_preserves_sccache_and_disabled_modes(self):
        import ci_build_observation as observation
        prepare, finalize = self.reports()
        for report in (prepare, finalize):
            report["cache_tool"] = "sccache"
            report["provenance"]["native_platform"] = "Windows"
            report["provenance"]["cmake_options"]["KLOGG_ENABLE_IOS_NATIVE_STACK"] = "OFF"
        combined = observation.aggregate_phases(prepare, finalize)
        self.assertEqual(combined["cache_tool"], "sccache")
        self.assertEqual(combined["cache_deltas"]["direct_cache_hit"], 6)
        finalize["provenance"]["cmake_options"]["KLOGG_ENABLE_IOS_NATIVE_STACK"] = "ON"
        with self.assertRaises(ValueError):
            observation.aggregate_phases(prepare, finalize)
        finalize["provenance"]["cmake_options"]["KLOGG_ENABLE_IOS_NATIVE_STACK"] = "OFF"
        for report in (prepare, finalize):
            report.update(object_cache="disabled", cache_tool=None, cache_deltas=None)
        combined = observation.aggregate_phases(prepare, finalize)
        self.assertEqual(combined["object_cache"], "disabled")
        self.assertIsNone(combined["cache_tool"])
        self.assertIsNone(combined["cache_deltas"])

    def test_windows_phase_reads_actual_sccache_counters_and_fails_measurement_closed(self):
        import ci_build_observation as observation
        for unavailable in (False, True, "after-invalid"):
            with self.subTest(unavailable=unavailable), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                build = root / "build"
                build.mkdir()
                (build / "compile_commands.json").write_text(json.dumps([
                    {"directory": str(build), "file": str(root / "src/a.cpp"), "output": "a.obj"}]))
                counts = iter([(10, 7, 3), (14, 10, 4)])
                calls = []
                def run(command, **kwargs):
                    calls.append(command)
                    if command[0] == "sccache":
                        if unavailable is True:
                            raise OSError("cache statistics unavailable")
                        if unavailable == "after-invalid" and len(calls) > 1:
                            return subprocess.CompletedProcess(command, 0, stdout="invalid post-build cache response")
                        requests, hits, misses = next(counts)
                        payload = {"stats": {"compile_requests": requests, "cache_hits": {"counts": {"C/C++": hits}},
                                             "cache_misses": {"counts": {"C/C++": misses}}}}
                        return subprocess.CompletedProcess(command, 0, stdout=json.dumps(payload))
                    self.assertEqual(command, ["fixture-build"])
                    (build / ".ninja_log").write_text("# ninja log v5\n0\t10\t0\ta.obj\t123\n")
                    return subprocess.CompletedProcess(command, 0)
                ticks = iter([1.0, 1.01])
                report = observation.run_phase(["fixture-build"], repo_root=root, build_root=build,
                    output=build / "phase.json", phase="prepare", cache_tool="sccache",
                    runner=run, clock=lambda: next(ticks))
                self.assertIsNotNone(report, "post-build counter failures must retain phase evidence")
                self.assertEqual(report["build_wall_ms"], 10)
                self.assertEqual(report["compilation"]["edges"], 1)
                self.assertIn("provenance", report)
                self.assertTrue((build / "phase.ninja-log.txt").is_file())
                self.assertIn(["sccache", "--show-stats", "--stats-format=json"], calls)
                self.assertNotIn(["ccache", "--print-stats"], calls)
                self.assertEqual(report["object_cache"], "unmeasured" if unavailable else "measured")
                self.assertEqual(report["cache_tool"], None if unavailable else "sccache")
                self.assertEqual(report["cache_deltas"], None if unavailable else {
                    "direct_cache_hit": 3, "preprocessed_cache_hit": 0, "cache_miss": 1})

    def test_aggregate_never_sums_partial_or_invalid_cache_counters(self):
        import ci_build_observation as observation
        self.assertTrue(hasattr(observation, "aggregate_phases"), "whole phased-build observation is missing")
        for invalid in (None, {}, {"direct_cache_hit": True, "preprocessed_cache_hit": 0, "cache_miss": 1}):
            prepare, finalize = self.reports()
            finalize["cache_deltas"] = invalid
            self.assertIsNone(observation.aggregate_phases(prepare, finalize)["cache_deltas"])
        prepare, finalize = self.reports()
        finalize["object_cache"] = "unmeasured"
        self.assertIsNone(observation.aggregate_phases(prepare, finalize)["cache_deltas"])

    def test_optional_ninja_analysis_failure_preserves_authoritative_phase_evidence(self):
        import ci_build_observation as observation
        raw = "# ninja log v5\n0\t552\t0\ta.obj\t123\n"
        for scenario in ("clock-mismatch", "malformed-log", "malformed-database", "compacted-log"):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                output = root / "prepare.json"
                database = [{"directory": str(root), "file": str(root / "src/a.cpp"), "output": "a.obj"}]
                (root / "compile_commands.json").write_text(
                    "invalid json" if scenario == "malformed-database" else json.dumps(database))
                if scenario == "compacted-log":
                    (root / ".ninja_log").write_text(raw + "0\t553\t0\ta.obj\t123\n")
                after = "# ninja log v5\ninvalid record\n" if scenario == "malformed-log" else raw
                def run(command, **kwargs):
                    self.assertEqual(command, ["fixture-build"])
                    (root / ".ninja_log").write_text(after)
                    return subprocess.CompletedProcess(command, 0)
                provenance = self.reports()[0]["provenance"]
                configure = {"passes": [{"purpose": "initial", "wall_ms": 7}], "wall_ms": 7}
                before = {"stats_zeroed_timestamp": 0, "direct_cache_hit": 2,
                          "preprocessed_cache_hit": 0, "cache_miss": 1}
                last = dict(before, direct_cache_hit=3, cache_miss=2)
                ticks = iter([1.0, 1.547 if scenario == "clock-mismatch" else 1.6])
                with mock.patch.object(observation, "build_provenance", return_value=copy.deepcopy(provenance)), \
                        mock.patch.object(observation.legacy, "ccache_counters", side_effect=[before, last]), \
                        contextlib.redirect_stderr(io.StringIO()) as diagnostic:
                    report = observation.run_phase(["fixture-build"], repo_root=root, build_root=root,
                        output=output, phase="prepare", runner=run, clock=lambda: next(ticks),
                        provenance={"native_platform": "Windows"}, configure=configure)
                self.assertIsNotNone(report, "optional Ninja analysis must not discard the successful phase")
                self.assertEqual(json.loads(output.read_text()), report)
                self.assertEqual(report["build_wall_ms"], 547 if scenario == "clock-mismatch" else 600)
                self.assertEqual(report["compilation_state"], "unknown")
                self.assertNotIn("compilation", report)
                self.assertNotIn("categories", report)
                self.assertTrue(report["compilation_unknown_reason"])
                self.assertEqual(report["provenance"], dict(provenance, native_platform="Windows"))
                self.assertEqual(report["configure"], configure)
                self.assertEqual(report["cache_deltas"], {"direct_cache_hit": 1,
                    "preprocessed_cache_hit": 0, "cache_miss": 1})
                self.assertEqual(report["object_cache"], "measured")
                self.assertEqual(report["cache_tool"], "ccache")
                finalize = self.reports()[1]
                finalize["provenance"] = copy.deepcopy(report["provenance"])
                finalize["provenance"]["cmake_options"]["KLOGG_ADB_HELPER_REQUIRED"] = "ON"
                combined = observation.aggregate_phases(report, finalize)
                self.assertEqual(combined["build_wall_ms"], report["build_wall_ms"] + finalize["build_wall_ms"])
                self.assertEqual(combined["phases"]["prepare"], report)
                self.assertEqual(output.with_suffix(".ninja-log.txt").read_text(), after)
                self.assertEqual((root / ".ninja_log").read_text(), after)
                if scenario == "clock-mismatch":
                    self.assertIn("build_wall_ms=547, ninja_last_end_ms=552", report["compilation_unknown_reason"])
                    self.assertIn("build_wall_ms=547, ninja_last_end_ms=552", diagnostic.getvalue())

    def test_default_phase_duration_uses_high_resolution_clock(self):
        import ci_build_observation as observation
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "compile_commands.json").write_text(json.dumps([
                {"directory": str(root), "file": str(root / "src/a.cpp"), "output": "a.obj"}]))
            def run(command, **kwargs):
                (root / ".ninja_log").write_text("# ninja log v5\n0\t552\t0\ta.obj\t123\n")
                return subprocess.CompletedProcess(command, 0)
            with mock.patch.object(observation.time, "monotonic", side_effect=[1.0, 1.547]) as coarse, \
                    mock.patch.object(observation.time, "perf_counter", side_effect=[1.0, 1.56]) as precise:
                report = observation.run_phase(["fixture-build"], repo_root=root, build_root=root,
                    output=root / "prepare.json", phase="prepare", object_cache="disabled", runner=run)
            self.assertIsNotNone(report)
            self.assertEqual(report["build_wall_ms"], 560)
            self.assertEqual(report["compilation_state"], "measured")
            self.assertEqual(report["compilation"]["edges"], 1)
            self.assertEqual(precise.call_count, 2)
            coarse.assert_not_called()

    def test_phase_delta_keeps_original_command_log_and_never_claims_whole_build(self):
        import ci_build_observation as observation
        self.assertTrue(hasattr(observation, "run_phase"), "phase-aware observation is missing")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            build.mkdir()
            before = "# ninja log v5\n0\t100\t0\ta.o\t123\n"
            after = before + "0\t5\t0\tb.o\t456\n"
            (build / ".ninja_log").write_text(before)
            (build / "compile_commands.json").write_text(json.dumps([
                {"directory": str(build), "file": str(root / "src/a.cpp"), "output": "a.o"},
                {"directory": str(build), "file": str(root / "src/b.cpp"), "output": "b.o"}]))
            def run(command, **kwargs):
                (build / ".ninja_log").write_text(after)
                return subprocess.CompletedProcess(command, 0)
            ticks = iter([1.0, 1.01])
            output = build / "final.json"
            report = observation.run_phase(["build"], repo_root=root, build_root=build, output=output,
                                           phase="finalize", object_cache="disabled", runner=run,
                                           clock=lambda: next(ticks))
            self.assertEqual(report["compilation"]["edges"], 1)
            self.assertEqual(report["measurement_scope"], "phase")
            self.assertEqual(report["phase"], "finalize")
            self.assertEqual((build / ".ninja_log").read_text(), after)
            self.assertEqual((build / "final.ninja-log.txt").read_text(), after)

    def test_failed_phase_retains_raw_log_but_never_claims_success(self):
        import ci_build_observation as observation
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "failed.json"
            raw = "# ninja log v5\n0\t10\t0\tpartial.obj\t123\n"
            def run(command, **kwargs):
                (root / ".ninja_log").write_text(raw)
                raise subprocess.CalledProcessError(1, command)
            with self.assertRaises(subprocess.CalledProcessError):
                observation.run_phase(["failed-build"], repo_root=root, build_root=root, output=output,
                                      phase="prepare", object_cache="disabled", runner=run)
            self.assertEqual(output.with_suffix(".ninja-log.txt").read_text(), raw)
            self.assertFalse(output.exists())

    def test_compacted_log_reports_unknown_instead_of_old_work_as_current_work(self):
        import ci_build_observation as observation
        self.assertTrue(hasattr(observation, "phase_log_delta"), "phase-aware observation is missing")
        before = "# ninja log v5\n0\t100\t0\ta.o\t123\n0\t200\t0\ta.o\t123\n"
        after = "# ninja log v5\n0\t200\t0\ta.o\t123\n0\t5\t0\tb.o\t456\n"
        self.assertIsNone(observation.phase_log_delta(before, after))
        self.assertEqual(observation.phase_log_delta(before, before), "# ninja log v5\n")


class RendezvousTests(unittest.TestCase):
    def setUp(self):
        self.spec = importlib.util.find_spec("ci_native_rendezvous")
        self.assertIsNotNone(self.spec, "native artifact rendezvous is not implemented")
        import ci_native_rendezvous
        self.module = ci_native_rendezvous
        self.sha = "a" * 40
        self.identity = {"repository": "owner/repo", "run_id": 17, "run_attempt": 2,
                         "sha": "b" * 40, "head_sha": self.sha}
        self.run = {"id": 17, "run_attempt": 2, "head_sha": self.sha,
                    "repository": {"full_name": "owner/repo"}}
        self.job = {"id": 20, "name": "Producer", "run_id": 17, "run_attempt": 1,
                    "head_sha": self.sha, "status": "completed", "conclusion": "success",
                    "started_at": "2026-10-18T01:00:00Z", "completed_at": "2026-10-18T01:05:00Z"}
        self.artifact = {"id": 30, "name": "native", "expired": False,
                         "created_at": "2026-10-18T01:04:00Z",
                         "workflow_run": {"id": 17, "head_sha": self.sha}}

    def evaluate(self, jobs=None, artifacts=None):
        return self.module.evaluate(self.identity, self.run,
                                    [self.job] if jobs is None else jobs,
                                    [self.artifact] if artifacts is None else artifacts,
                                    {"Producer": ["native"]})

    def test_preserved_pr_and_master_native_artifacts_pass_exact_identity_checks(self):
        root = ROOT / "docs/ci/evidence/master-39091756819"
        producers = {"Source-built ADB helper macos-x86_64": ["adb-helper-macos-x86_64"],
                     "iOS native stack x86_64": ["ios-native-x86_64"],
                     "Build ADB helper legal assets": ["adb-helper-package-support"]}
        for label in ("master", "pr"):
            with self.subTest(label=label):
                run = json.loads((root / (label + "-run.json")).read_text())
                expected = dict(self.identity, repository="ZEACENT/klogg", run_id=run["id"],
                                run_attempt=run["run_attempt"], head_sha=run["head_sha"])
                jobs = json.loads((root / (label + "-jobs.json")).read_text())["jobs"]
                artifacts = json.loads((root / (label + "-artifacts.json")).read_text())["artifacts"]
                ready = self.module.evaluate(expected, run, jobs, artifacts, producers)
                self.assertEqual(set(ready), {name for names in producers.values() for name in names})

    def test_api_pagination_must_be_complete(self):
        pages = [{"total_count": 2, "jobs": [self.job]}, {"total_count": 2, "jobs": [dict(self.job, id=21)]}]
        response = subprocess.CompletedProcess([], 0, stdout=json.dumps(pages))
        with mock.patch.object(self.module.subprocess, "run", return_value=response) as run:
            self.assertEqual(len(self.module.github_api("jobs")["jobs"]), 2)
            self.assertIn("--paginate", run.call_args.args[0])
        response.stdout = json.dumps(pages[:1])
        with mock.patch.object(self.module.subprocess, "run", return_value=response), self.assertRaisesRegex(ValueError, "incomplete"):
            self.module.github_api("jobs")

    def test_windows_receipt_rejects_other_targets_missing_legal_and_wrong_identity(self):
        profile = self.module.profile("Windows", "x86_64")
        receipt = {"schema_version": 1, "identity": self.identity,
                   "artifacts": {name: {"producer_name": producer, "producer_attempt": 1,
                                        "producer_id": index + 20, "artifact_id": index + 30}
                                 for index, (name, producer) in enumerate(profile["artifacts"].items())}}
        self.module.validate_receipt(receipt, self.identity, "x86_64", platform="Windows")
        for mutation in ("missing-support", "mac-target", "wrong-producer", "wrong-attempt", "wrong-sha"):
            broken = copy.deepcopy(receipt)
            if mutation == "missing-support":
                broken["artifacts"].pop("adb-helper-package-support")
            elif mutation == "mac-target":
                broken["artifacts"]["adb-helper-macos-x86_64"] = broken["artifacts"].pop("adb-helper-windows-x86_64")
            elif mutation == "wrong-producer":
                broken["artifacts"]["adb-helper-windows-x86_64"]["producer_name"] = "unrelated"
            elif mutation == "wrong-attempt":
                broken["identity"]["run_attempt"] += 1
            else:
                broken["identity"]["sha"] = "c" * 40
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.module.validate_receipt(broken, self.identity, "x86_64", platform="Windows")
        with self.assertRaises(ValueError):
            self.module.validate_receipt(receipt, self.identity, "x86_64")

    def test_partial_rerun_carry_forward_metadata_keeps_original_artifact_interval(self):
        # GitHub can clone a successful job into attempt 2 with a new API job ID
        # while retaining its original execution interval. This is not a rebuild.
        carried = dict(self.job, id=21, run_attempt=2)
        ready = self.evaluate([self.job, carried])
        self.assertEqual(ready["native"], {"artifact_id": 30, "producer_id": 21,
                                           "producer_attempt": 2, "producer_name": "Producer"})
        replacement = dict(carried, started_at="2026-10-18T02:00:00Z", completed_at="2026-10-18T02:05:00Z")
        with self.assertRaisesRegex(ValueError, "producer interval"):
            self.evaluate([self.job, replacement])
        with self.assertRaisesRegex(ValueError, "failure"):
            self.evaluate([self.job, dict(replacement, conclusion="failure")])
        self.assertIsNone(self.evaluate([self.job, dict(replacement, status="in_progress", conclusion=None)]))

    def test_partial_rerun_reuses_latest_successful_producer(self):
        result = self.evaluate()
        self.assertEqual(result["native"]["artifact_id"], 30)
        self.assertEqual(result["native"]["producer_attempt"], 1)

    def test_later_failed_producer_rejects_old_artifact(self):
        for conclusion in ("failure", "cancelled", "timed_out", "skipped", "neutral", "action_required", "stale"):
            with self.subTest(conclusion=conclusion):
                latest = dict(self.job, id=21, run_attempt=2, conclusion=conclusion)
                with self.assertRaisesRegex(ValueError, "Producer.*" + conclusion):
                    self.evaluate([self.job, latest])

    def test_later_running_producer_waits_instead_of_reusing_old_success(self):
        latest = dict(self.job, id=21, run_attempt=2, status="in_progress", conclusion=None)
        self.assertIsNone(self.evaluate([self.job, latest]))

    def test_new_success_cannot_adopt_previous_attempt_artifact(self):
        latest = dict(self.job, id=21, run_attempt=2, started_at="2026-10-18T02:00:00Z",
                      completed_at="2026-10-18T02:05:00Z")
        with self.assertRaisesRegex(ValueError, "producer interval"):
            self.evaluate([self.job, latest])

    def test_successful_rerun_selects_only_its_upload_when_old_name_is_retained(self):
        latest = dict(self.job, id=21, run_attempt=2, started_at="2026-10-18T02:00:00Z",
                      completed_at="2026-10-18T02:05:00Z")
        fresh = dict(self.artifact, id=31, created_at="2026-10-18T02:04:00Z")
        ready = self.evaluate([self.job, latest], [self.artifact, fresh])
        self.assertEqual(ready["native"]["artifact_id"], 31)

    def test_wrong_sha_expired_duplicate_and_missing_ancestry_fail_closed(self):
        for invalid in (dict(self.artifact, expired=True),
                        dict(self.artifact, workflow_run={"id": 18, "head_sha": self.sha}),
                        dict(self.artifact, workflow_run={"id": 17, "head_sha": "c" * 40}),
                        dict(self.artifact, workflow_run=None)):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                self.evaluate(artifacts=[invalid])
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            self.evaluate(artifacts=[self.artifact, dict(self.artifact, id=31)])
        self.run["head_sha"] = "c" * 40
        with self.assertRaisesRegex(ValueError, "run identity"):
            self.evaluate()

    def test_pending_artifact_and_missing_job_wait(self):
        self.assertIsNone(self.evaluate(jobs=[]))
        self.assertIsNone(self.evaluate(artifacts=[]))

    def test_duplicate_latest_job_and_future_attempt_fail_closed(self):
        with self.assertRaises(ValueError):
            self.evaluate([self.job, dict(self.job, id=21)])
        with self.assertRaises(ValueError):
            self.evaluate([dict(self.job, run_attempt=3)])

    def test_poll_has_deadline_and_rechecks_run_after_artifact_snapshot(self):
        calls = []
        values = iter([self.run, {"jobs": [self.job]}, {"artifacts": [self.artifact]}, self.run])
        def api(path):
            calls.append(path)
            return next(values)
        result = self.module.wait(self.identity, {"Producer": ["native"]}, api=api,
                                  clock=lambda: 0, sleep=lambda _: self.fail("unexpected sleep"))
        self.assertEqual(result["native"]["artifact_id"], 30)
        self.assertEqual(calls[0], calls[-1])
        ticks = iter([0, 0, 31])
        with self.assertRaisesRegex(TimeoutError, "native"):
            self.module.wait(self.identity, {"Producer": ["native"]},
                             api=lambda p: self.run if "/jobs" not in p and "/artifacts" not in p
                             else {"jobs": [], "artifacts": []},
                             timeout=30, clock=lambda: next(ticks), sleep=lambda _: None)

    def test_api_failure_is_bounded_and_visible(self):
        ticks = iter([0, 0, 31])
        with self.assertRaises(TimeoutError), mock.patch("sys.stderr") as stderr:
            self.module.wait(self.identity, {"Producer": ["native"]}, timeout=30,
                             api=mock.Mock(side_effect=subprocess.CalledProcessError(1, ["gh", "api"])),
                             clock=lambda: next(ticks), sleep=lambda _: None)
        self.assertTrue(stderr.write.called)

    def test_merge_sha_is_separate_from_run_head_sha(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "event.json"
            path.write_text(json.dumps({"pull_request": {"head": {"sha": self.sha}}}))
            env = {"GITHUB_REPOSITORY": "owner/repo", "GITHUB_RUN_ID": "17", "GITHUB_RUN_ATTEMPT": "2",
                   "GITHUB_SHA": "b" * 40, "GITHUB_EVENT_NAME": "pull_request", "GITHUB_EVENT_PATH": str(path)}
            self.assertEqual(self.module.identity(env), self.identity)
            for event in ("push", "workflow_dispatch"):
                env["GITHUB_EVENT_NAME"] = event
                self.assertEqual(self.module.identity(env)["head_sha"], "b" * 40)
            env["GITHUB_EVENT_NAME"] = "unknown"
            with self.assertRaisesRegex(ValueError, "unsupported"):
                self.module.identity(env)


if __name__ == "__main__":
    unittest.main()
