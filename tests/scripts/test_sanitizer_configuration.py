import json
import pathlib
import re
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
SANITIZERS = ROOT / "cmake" / "Sanitizers.cmake"
TEST_TARGET_OPTIONS = ROOT / "cmake" / "TestTargetOptions.cmake"
MSVC_ASAN_DEPENDENCIES = ROOT / "cmake" / "MsvcAsanDependencies.cmake"
CI_BUILD = ROOT / ".github" / "workflows" / "ci-build.yml"
UBUNTU_22_DOCKERFILE = ROOT / "docker" / "ubuntu22.04" / "Dockerfile"
CAPTURESTORE_TEST = ROOT / "tests" / "unit" / "capturestore_test.cpp"


class SanitizerConfigurationTest(unittest.TestCase):
    def configure(self, compiler_id, *options, processor="x86_64"):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.12)\n"
                "project(sanitizer_guard LANGUAGES C CXX)\n"
                "add_library(project_options INTERFACE)\n"
                f'set(CMAKE_CXX_COMPILER_ID "{compiler_id}")\n'
                f'set(CMAKE_SYSTEM_PROCESSOR "{processor}")\n'
                "set(CMAKE_SIZEOF_VOID_P 8)\n"
                f'include("{SANITIZERS}")\n'
                "enable_sanitizers(project_options)\n"
            )
            return subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build"), *options],
                check=False,
                capture_output=True,
                text=True,
            )

    def configure_consumer(
        self,
        compiler_id,
        *options,
        processor="x86_64",
        exercise_legacy_link_options=False,
    ):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            legacy_exercise = (
                "set(MSVC TRUE)\n"
                "function(exercise_legacy_link_options)\n"
                "  klogg_add_legacy_link_options(/INCREMENTAL:NO)\n"
                "endfunction()\n"
                "exercise_legacy_link_options()\n"
                if exercise_legacy_link_options
                else ""
            )
            (root / "consumer.cpp").write_text("int consumer() { return 0; }\n")
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.12)\n"
                "project(sanitizer_consumer LANGUAGES C CXX)\n"
                "add_library(project_options INTERFACE)\n"
                f'set(CMAKE_CXX_COMPILER_ID "{compiler_id}")\n'
                f'set(CMAKE_SYSTEM_PROCESSOR "{processor}")\n'
                "set(CMAKE_SIZEOF_VOID_P 8)\n"
                + f'include("{SANITIZERS}")\n'
                + legacy_exercise
                + "enable_sanitizers(project_options)\n"
                "file(WRITE \"${CMAKE_BINARY_DIR}/legacy_link_flags.txt\" "
                "\"${CMAKE_EXE_LINKER_FLAGS}\")\n"
                "add_library(consumer STATIC consumer.cpp)\n"
                "target_link_libraries(consumer PRIVATE project_options)\n"
            )
            result = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(root),
                    "-B",
                    str(root / "build"),
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                    *options,
                ],
                check=False,
                capture_output=True,
                text=True,
            )

            command = ""
            commands_path = root / "build" / "compile_commands.json"
            if commands_path.exists():
                commands = json.loads(commands_path.read_text())
                command = next(
                    entry["command"]
                    for entry in commands
                    if entry["file"].endswith("consumer.cpp")
                )

            link_flags = ""
            link_flags_path = root / "build" / "legacy_link_flags.txt"
            if link_flags_path.exists():
                link_flags = link_flags_path.read_text()

            return result, command, link_flags

    def test_cmake_312_uses_the_link_option_compatibility_wrapper(self):
        text = SANITIZERS.read_text()
        self.assertIn("if(COMMAND add_link_options)", text)
        self.assertEqual(
            len(re.findall(r"^\s*add_link_options\(", text, re.MULTILINE)), 1
        )
        self.assertEqual(text.count("klogg_add_link_options("), 3)

    def test_supported_clang_address_sanitizer_configures(self):
        result = self.configure("Clang", "-DENABLE_SANITIZER_ADDRESS=ON")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_msvc_vectorscan_diagnostics_keep_symbols_without_relaxing_other_tests(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "test.cpp").write_text("int main() { return 0; }\n")
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.12)\n"
                "project(test_target_options LANGUAGES CXX)\n"
                "set(MSVC TRUE)\n"
                f'include("{TEST_TARGET_OPTIONS}")\n'
                "add_executable(ordinary_test test.cpp)\n"
                "klogg_configure_test_target(ordinary_test)\n"
                "get_target_property(ordinary_flags ordinary_test "
                "LINK_FLAGS_RELWITHDEBINFO)\n"
                "file(WRITE \"${CMAKE_BINARY_DIR}/ordinary_flags.txt\" "
                "\"${ordinary_flags}\")\n"
                "add_executable(vectorscan_test test.cpp)\n"
                "klogg_configure_test_target(vectorscan_test KEEP_DEBUG_SYMBOLS)\n"
                "get_target_property(vectorscan_flags vectorscan_test "
                "LINK_FLAGS_RELWITHDEBINFO)\n"
                "file(WRITE \"${CMAKE_BINARY_DIR}/vectorscan_flags.txt\" "
                "\"${vectorscan_flags}\")\n"
            )
            result = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build")],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            ordinary_flags = (root / "build" / "ordinary_flags.txt").read_text()
            vectorscan_flags = (root / "build" / "vectorscan_flags.txt").read_text()
            self.assertIn("/DEBUG:NONE", ordinary_flags)
            self.assertIn("/INCREMENTAL:NO", ordinary_flags)
            self.assertIn("/DEBUG:FULL", vectorscan_flags)
            self.assertIn("/INCREMENTAL:NO", vectorscan_flags)
            self.assertNotIn("/DEBUG:NONE", vectorscan_flags)

    def test_msvc_asan_stabilizes_only_named_dependency_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "dependency.cpp").write_text("int dependency() { return 0; }\n")
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.12)\n"
                "project(msvc_asan_dependencies LANGUAGES CXX)\n"
                "set(MSVC TRUE)\n"
                "set(ENABLE_SANITIZER_ADDRESS ON)\n"
                f'include("{MSVC_ASAN_DEPENDENCIES}")\n'
                "add_library(hs_exec STATIC dependency.cpp)\n"
                "add_library(hs_compile STATIC dependency.cpp)\n"
                "add_library(unrelated STATIC dependency.cpp)\n"
                "klogg_stabilize_msvc_asan_dependencies(hs_exec hs_compile)\n"
                "set(ENABLE_SANITIZER_ADDRESS OFF)\n"
                "add_library(release_dependency STATIC dependency.cpp)\n"
                "klogg_stabilize_msvc_asan_dependencies(release_dependency)\n"
                "get_target_property(exec_options hs_exec COMPILE_OPTIONS)\n"
                "get_target_property(compile_options hs_compile COMPILE_OPTIONS)\n"
                "get_target_property(unrelated_options unrelated COMPILE_OPTIONS)\n"
                "get_target_property(release_options release_dependency COMPILE_OPTIONS)\n"
                "file(WRITE \"${CMAKE_BINARY_DIR}/options.txt\" "
                "\"exec=${exec_options}\\ncompile=${compile_options}\\nunrelated=${unrelated_options}\\nrelease=${release_options}\")\n"
            )
            result = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build")],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            options = (root / "build" / "options.txt").read_text()
            self.assertIn("exec=/Od;/Ob0", options)
            self.assertIn("compile=/Od;/Ob0", options)
            self.assertIn("unrelated=unrelated_options-NOTFOUND", options)
            self.assertIn("release=release_options-NOTFOUND", options)

    def test_sanitizer_build_contract_reaches_first_party_consumers(self):
        configurations = (
            (
                "Clang ASan+UBSan",
                "Clang",
                (
                    "-DENABLE_SANITIZER_ADDRESS=ON",
                    "-DENABLE_SANITIZER_UNDEFINED_BEHAVIOR=ON",
                ),
                {
                    "KLOGG_SANITIZER_BUILD=1",
                    "KLOGG_ASAN_BUILD=1",
                    "KLOGG_UBSAN_BUILD=1",
                },
            ),
            (
                "Clang TSan",
                "Clang",
                ("-DENABLE_SANITIZER_THREAD=ON",),
                {"KLOGG_SANITIZER_BUILD=1", "KLOGG_TSAN_BUILD=1"},
            ),
            (
                "GNU ASan",
                "GNU",
                ("-DENABLE_SANITIZER_ADDRESS=ON",),
                {"KLOGG_SANITIZER_BUILD=1", "KLOGG_ASAN_BUILD=1"},
            ),
            (
                "MSVC ASan",
                "MSVC",
                ("-DENABLE_SANITIZER_ADDRESS=ON",),
                {
                    "KLOGG_SANITIZER_BUILD=1",
                    "KLOGG_ASAN_BUILD=1",
                    "KLOGG_MSVC_ASAN=1",
                },
            ),
        )

        for name, compiler_id, options, expected_definitions in configurations:
            with self.subTest(name=name):
                result, command, _ = self.configure_consumer(
                    compiler_id, *options
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for definition in expected_definitions:
                    self.assertRegex(
                        command,
                        rf"(?:^|\s)(?:-D|/D){re.escape(definition)}(?:\s|$)",
                    )

    def test_non_sanitizer_build_does_not_relax_first_party_test_budgets(self):
        result, command, _ = self.configure_consumer("Clang")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotRegex(command, r"(?:-D|/D)KLOGG_(?:SANITIZER|A|M|UB|T)SAN_BUILD")
        self.assertNotIn("KLOGG_MSVC_ASAN", command)

    def test_capturestore_budget_uses_the_propagated_contract(self):
        capturestore_test = CAPTURESTORE_TEST.read_text()
        self.assertIn("#if defined( KLOGG_SANITIZER_BUILD )", capturestore_test)
        self.assertNotIn("#if defined( __has_feature )", capturestore_test)

    def test_cmake_312_legacy_msvc_link_flag_path_is_executable(self):
        result, _, link_flags = self.configure_consumer(
            "MSVC",
            "-DENABLE_SANITIZER_ADDRESS=ON",
            exercise_legacy_link_options=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("/INCREMENTAL:NO", link_flags)

    def test_vptr_disable_follows_undefined_sanitizer_for_every_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "first_party.cpp").write_text("int first_party() { return 1; }\n")
            (root / "vendored.cpp").write_text("int vendored() { return 2; }\n")
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.12)\n"
                "project(sanitizer_order LANGUAGES C CXX)\n"
                "add_library(project_options INTERFACE)\n"
                'set(CMAKE_CXX_COMPILER_ID "GNU")\n'
                f'include("{SANITIZERS}")\n'
                "enable_sanitizers(project_options)\n"
                "add_library(vendored STATIC vendored.cpp)\n"
                "add_library(first_party STATIC first_party.cpp)\n"
                "target_link_libraries(first_party PRIVATE project_options)\n"
            )
            result = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(root),
                    "-B",
                    str(root / "build"),
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                    "-DENABLE_SANITIZER_ADDRESS=ON",
                    "-DENABLE_SANITIZER_UNDEFINED_BEHAVIOR=ON",
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            commands = json.loads(
                (root / "build" / "compile_commands.json").read_text()
            )
            for source in ("first_party.cpp", "vendored.cpp"):
                command = next(
                    entry["command"] for entry in commands if source in entry["file"]
                )
                undefined_positions = [
                    match.start()
                    for match in re.finditer(r"-fsanitize=\S*undefined", command)
                ]
                self.assertTrue(undefined_positions, command)
                self.assertGreater(
                    command.rfind("-fno-sanitize=vptr"),
                    max(undefined_positions),
                    command,
                )

    def test_rejects_gcc_tsan_because_mimalloc_requires_clang(self):
        result = self.configure("GNU", "-DENABLE_SANITIZER_THREAD=ON")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ThreadSanitizer builds require Clang", result.stdout + result.stderr)

    def test_linux_tsan_requires_an_installed_explicit_symbolizer(self):
        dockerfile = UBUNTU_22_DOCKERFILE.read_text()
        workflow = CI_BUILD.read_text()
        self.assertRegex(dockerfile, r"\bllvm-14\b")
        self.assertIn("test -x /usr/bin/llvm-symbolizer-14", workflow)
        self.assertIn(
            "TSAN_OPTIONS=halt_on_error=1:external_symbolizer_path=/usr/bin/llvm-symbolizer-14",
            workflow,
        )

    def test_linux_lsan_uses_complete_stacks_for_narrow_suppressions(self):
        workflow = CI_BUILD.read_text()
        self.assertIn(
            "LSAN_OPTIONS=suppressions=/usr/local/tests/sanitizers/lsan_suppressions.txt:fast_unwind_on_malloc=0",
            workflow,
        )

    def test_rejects_unconfigured_compiler_instead_of_false_green(self):
        result = self.configure("IntelLLVM", "-DENABLE_SANITIZER_ADDRESS=ON")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Sanitizer builds are not configured", result.stdout + result.stderr)

    def test_windows_asan_failure_retains_vectorscan_symbols(self):
        workflow = CI_BUILD.read_text()
        self.assertIn("Collect Windows ASan diagnostics", workflow)
        self.assertIn("klogg_vectorscan_tests.pdb", workflow)
        self.assertIn("windows-x64-asan-diagnostics", workflow)
        self.assertIn(
            "matrix.config.sanitizer == 'address' && steps.run-tests.outcome == 'failure'",
            workflow,
        )

    def test_msvc_address_sanitizer_marks_intercepted_dependency_allocations(self):
        text = SANITIZERS.read_text()
        self.assertIn("KLOGG_MSVC_ASAN=1", text)
        allocator_source = (
            ROOT / "src" / "regex" / "src" / "hsregularexpression.cpp"
        ).read_text()
        self.assertIn("hs_set_allocator( std::malloc, std::free )", allocator_source)

    def test_filewatcher_cross_thread_notifications_use_named_slots(self):
        source = (ROOT / "src" / "filewatch" / "src" / "filewatcher.cpp").read_text()
        self.assertNotIn("dispatchToMainThread", source)
        self.assertEqual(source.count('"fileChangedOnDisk", Qt::QueuedConnection'), 2)

    def test_rejects_msvc_arm64_address_sanitizer(self):
        result = self.configure(
            "MSVC", "-DENABLE_SANITIZER_ADDRESS=ON", processor="ARM64"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("supported only for x64 builds", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
