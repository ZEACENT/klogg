import json
import os
import pathlib
import re
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
PROJECT_CMAKE = ROOT / "CMakeLists.txt"
CPM_CMAKE = ROOT / "cmake" / "CPM.cmake"
SANITIZERS = ROOT / "cmake" / "Sanitizers.cmake"
TEST_TARGET_OPTIONS = ROOT / "cmake" / "TestTargetOptions.cmake"
MSVC_ASAN_DEPENDENCIES = ROOT / "cmake" / "MsvcAsanDependencies.cmake"
VERIFY_TSAN_QT = ROOT / "cmake" / "VerifyTsanQt.cmake"
CI_BUILD = ROOT / ".github" / "workflows" / "ci-build.yml"
DOCKER_BUILD_ACTION = ROOT / ".github" / "actions" / "docker-build" / "action.yml"
RESTORE_CPM_CACHE_ACTION = (
    ROOT / ".github" / "actions" / "restore-cpm-cache" / "action.yml"
)
RESTORE_CPM_CACHE_SCRIPT = ROOT / "scripts" / "restore_cpm_cache.sh"
CPM_CACHE_CONTRACT_SCRIPT = ROOT / "scripts" / "check_cpm_cache_contract.sh"
THIRD_PARTY_CMAKE = ROOT / "3rdparty" / "CMakeLists.txt"
CPM_PREFETCH_CMAKE = ROOT / "cmake" / "prefetch_cpm" / "CMakeLists.txt"
UBUNTU_22_DOCKERFILE = ROOT / "docker" / "ubuntu22.04" / "Dockerfile"
UBUNTU_22_TSAN_DOCKERFILE = ROOT / "docker" / "ubuntu22.04-tsan" / "Dockerfile"
QT_QOBJECT_TSAN_PATCH = (
    ROOT
    / "docker"
    / "ubuntu22.04-tsan"
    / "patches"
    / "fix_qt5_qobject_tsan_publication.patch"
)
TSAN_RUNTIME_PREFLIGHT = ROOT / "scripts" / "verify_tsan_qt_runtime.sh"
TSAN_ELF_RUNTIME_CLOSURE = (
    ROOT / "docker" / "ubuntu22.04-tsan" / "verify_elf_runtime_closure.sh"
)
CAPTURESTORE_TEST = ROOT / "tests" / "unit" / "capturestore_test.cpp"


class SanitizerConfigurationTest(unittest.TestCase):
    def test_project_minimum_matches_cpm_and_restores_policy_after_ucm(self):
        version_pattern = r"cmake_minimum_required\(VERSION\s+([0-9.]+)"
        versions = []
        for cmake_file in (PROJECT_CMAKE, CPM_CMAKE):
            with self.subTest(cmake_file=cmake_file):
                match = re.search(version_pattern, cmake_file.read_text())
                self.assertIsNotNone(match)
                versions.append(match.group(1))
        self.assertEqual(len(set(versions)), 1)
        project_version_tuple = tuple(map(int, versions[0].split(".")))
        self.assertGreaterEqual(project_version_tuple, (3, 14))
        self.assertRegex(
            PROJECT_CMAKE.read_text(),
            r"include\(ucm\)\s*#.*?\n#.*?\ncmake_policy\(VERSION 3\.14\)",
        )

    def configure(self, compiler_id, *options, processor="x86_64"):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.14)\n"
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
                "cmake_minimum_required(VERSION 3.14)\n"
                "project(sanitizer_consumer LANGUAGES C CXX)\n"
                "add_library(project_options INTERFACE)\n"
                f'set(CMAKE_CXX_COMPILER_ID "{compiler_id}")\n'
                f'set(CMAKE_SYSTEM_PROCESSOR "{processor}")\n'
                "set(CMAKE_SIZEOF_VOID_P 8)\n"
                + f'include("{SANITIZERS}")\n'
                + legacy_exercise
                + "enable_sanitizers(project_options)\n"
                "get_target_property(project_link_libraries project_options "
                "INTERFACE_LINK_LIBRARIES)\n"
                "get_directory_property(directory_link_options LINK_OPTIONS)\n"
                "file(WRITE \"${CMAKE_BINARY_DIR}/legacy_link_flags.txt\" "
                "\"legacy=${CMAKE_EXE_LINKER_FLAGS}\\n"
                "interface=${project_link_libraries}\\n"
                "directory=${directory_link_options}\\n\")\n"
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

    def configure_tsan_qt_consumer(
        self,
        *,
        qt_version="5.15.19",
        core_under_prefix=True,
        annotations=("__tsan_acquire", "__tsan_release"),
        missing_instrumentation_target=None,
        missing_debug_instrumentation_target=None,
    ):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            prefix = root / "qt5-tsan"
            lib_dir = prefix / "lib"
            lib_dir.mkdir(parents=True)

            target_names = (
                "Core",
                "Gui",
                "Widgets",
                "Concurrent",
                "Network",
                "Xml",
                "Svg",
                "Test",
            )
            locations = {}
            for name in target_names:
                location = lib_dir / f"libQt5{name}.so.5"
                location.touch()
                locations[name] = location

            if not core_under_prefix:
                system_core = root / "system-qt" / "libQt5Core.so.5"
                system_core.parent.mkdir()
                system_core.touch()
                locations["Core"] = system_core

            nm = root / "fake-nm.sh"
            annotation_output = " ".join(
                f"'                 U {symbol}'" for symbol in annotations
            )
            instrumented_output = (
                f"{annotation_output} '                 U __tsan_func_entry'"
            )
            debug_location = None
            if missing_debug_instrumentation_target is not None:
                debug_location = (
                    lib_dir
                    / f"libQt5{missing_debug_instrumentation_target}Debug.so.5"
                )
                debug_location.touch()

            if missing_instrumentation_target is not None:
                missing_filename = f"libQt5{missing_instrumentation_target}.so.5"
            elif debug_location is not None:
                missing_filename = debug_location.name
            else:
                missing_filename = None
            nm.write_text(
                "#!/bin/sh\n"
                + (
                    f'if [ "${{3##*/}}" = "{missing_filename}" ]; then\n'
                    f"  printf '%s\\n' {annotation_output}\n"
                    "else\n"
                    f"  printf '%s\\n' {instrumented_output}\n"
                    "fi\n"
                    if missing_filename is not None
                    else f"printf '%s\\n' {instrumented_output}\n"
                )
            )
            nm.chmod(0o755)

            imported_targets = []
            for index, name in enumerate(target_names):
                if name == missing_debug_instrumentation_target:
                    imported_targets.append(
                        f"add_library(Qt5::{name} SHARED IMPORTED)\n"
                        f"set_target_properties(Qt5::{name} PROPERTIES "
                        'IMPORTED_CONFIGURATIONS "RELEASE;DEBUG" '
                        f'IMPORTED_LOCATION_RELEASE "{locations[name]}" '
                        f'IMPORTED_LOCATION_DEBUG "{debug_location}")\n'
                    )
                else:
                    property_name = (
                        "IMPORTED_LOCATION_RELEASE"
                        if index % 2
                        else "IMPORTED_LOCATION"
                    )
                    imported_targets.append(
                        f"add_library(Qt5::{name} SHARED IMPORTED)\n"
                        f"set_target_properties(Qt5::{name} PROPERTIES "
                        f'{property_name} "{locations[name]}")\n'
                    )

            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.14)\n"
                "project(tsan_qt_consumer NONE)\n"
                "set(ENABLE_SANITIZER_THREAD ON)\n"
                f'set(KLOGG_TSAN_QT_PREFIX "{prefix}")\n'
                'set(KLOGG_TSAN_QT_VERSION "5.15.19")\n'
                "set(QT_VERSION_MAJOR 5)\n"
                f'set(KLOGG_QT_VERSION "{qt_version}")\n'
                f'set(CMAKE_NM "{nm}")\n'
                + "".join(imported_targets)
                + f'include("{VERIFY_TSAN_QT}")\n'
                + "klogg_verify_tsan_qt(\n"
                + "  Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Concurrent\n"
                + "  Qt5::Network Qt5::Xml Qt5::Svg Qt5::Test\n"
                + ")\n"
            )

            return subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build")],
                check=False,
                capture_output=True,
                text=True,
            )

    def run_tsan_runtime_preflight(
        self,
        *,
        qt_under_prefix,
        plugin_mode="expected",
        missing_instrumentation_library=None,
    ):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            prefix = root / "qt5-tsan"
            plugin = prefix / "plugins" / "platforms" / "libqoffscreen.so"
            plugin.parent.mkdir(parents=True)
            plugin.touch()
            (prefix / "lib").mkdir()

            plugin_diagnostics = {
                "expected": f'loaded library "{plugin}"',
                "metadata-only": f'Found metadata in lib "{plugin}"',
                "fallback": (
                    f'Found metadata in lib "{plugin}"\n'
                    'loaded library "/tmp/fallback/platforms/libqoffscreen.so"'
                ),
            }[plugin_mode]
            executable = root / "qt-app"
            executable.write_text(
                "#!/bin/sh\n"
                f"printf '%s\\n' '{plugin_diagnostics}' >&2\n"
            )
            executable.chmod(0o755)

            fake_bin = root / "bin"
            fake_bin.mkdir()
            ldd = fake_bin / "ldd"
            core_library = (
                prefix / "lib" / "libQt5Core.so.5"
                if qt_under_prefix
                else pathlib.Path("/usr/lib/x86_64-linux-gnu/libQt5Core.so.5")
            )
            gui_library = prefix / "lib" / "libQt5Gui.so.5"
            ldd.write_text(
                "#!/bin/sh\n"
                f"printf '%s\\n' 'libQt5Core.so.5 => {core_library} (0x0000)'\n"
                f"printf '%s\\n' 'libQt5Gui.so.5 => {gui_library} (0x0000)'\n"
            )
            ldd.chmod(0o755)

            nm = fake_bin / "llvm-nm-14"
            missing_filename = missing_instrumentation_library or ""
            nm.write_text(
                "#!/bin/sh\n"
                f'if [ "${{3##*/}}" = "{missing_filename}" ]; then\n'
                "  exit 0\n"
                "fi\n"
                "printf '%s\\n' '                 U __tsan_func_entry'\n"
            )
            nm.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = f"{fake_bin}{os.pathsep}{env['PATH']}"
            return subprocess.run(
                [str(TSAN_RUNTIME_PREFLIGHT), str(executable), str(prefix)],
                check=False,
                capture_output=True,
                text=True,
                env=env,
            )

    def test_sanitizers_use_the_link_option_compatibility_wrapper(self):
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
                "cmake_minimum_required(VERSION 3.14)\n"
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
                "cmake_minimum_required(VERSION 3.14)\n"
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
                "-fsanitize=address,undefined",
                "-fsanitize=address,undefined",
            ),
            (
                "Clang TSan",
                "Clang",
                ("-DENABLE_SANITIZER_THREAD=ON",),
                {"KLOGG_SANITIZER_BUILD=1", "KLOGG_TSAN_BUILD=1"},
                "-fsanitize=thread",
                "-fsanitize=thread",
            ),
            (
                "GNU ASan",
                "GNU",
                ("-DENABLE_SANITIZER_ADDRESS=ON",),
                {"KLOGG_SANITIZER_BUILD=1", "KLOGG_ASAN_BUILD=1"},
                "-fsanitize=address",
                "-fsanitize=address",
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
                "/fsanitize=address",
                "/INCREMENTAL:NO",
            ),
        )

        for (
            name,
            compiler_id,
            options,
            expected_definitions,
            expected_compile_flag,
            expected_link_flag,
        ) in configurations:
            with self.subTest(name=name):
                result, command, link_flags = self.configure_consumer(
                    compiler_id, *options
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                for definition in expected_definitions:
                    self.assertRegex(
                        command,
                        rf"(?:^|\s)(?:-D|/D){re.escape(definition)}(?:\s|$)",
                    )
                self.assertIn(expected_compile_flag, command)
                self.assertIn(expected_link_flag, link_flags)

    def test_host_first_party_executable_is_compiled_and_linked_with_asan(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            build = root / "build"
            (root / "consumer.cpp").write_text("int main() { return 0; }\n")
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.14)\n"
                "project(sanitizer_executable LANGUAGES C CXX)\n"
                "add_library(project_options INTERFACE)\n"
                f'include("{SANITIZERS}")\n'
                "enable_sanitizers(project_options)\n"
                "add_executable(consumer consumer.cpp)\n"
                "target_link_libraries(consumer PRIVATE project_options)\n"
            )
            configure = subprocess.run(
                [
                    "cmake",
                    "-S",
                    str(root),
                    "-B",
                    str(build),
                    "-G",
                    "Ninja",
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                    "-DENABLE_SANITIZER_ADDRESS=ON",
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                configure.returncode, 0, configure.stdout + configure.stderr
            )

            commands = json.loads((build / "compile_commands.json").read_text())
            command = next(
                entry["command"]
                for entry in commands
                if entry["file"].endswith("consumer.cpp")
            )
            sanitizer_flag = (
                "/fsanitize=address" if os.name == "nt" else "-fsanitize=address"
            )
            self.assertIn(sanitizer_flag, command)

            result = subprocess.run(
                ["cmake", "--build", str(build), "--verbose"],
                check=False,
                capture_output=True,
                text=True,
            )
            build_output = result.stdout + result.stderr
            self.assertEqual(result.returncode, 0, build_output)
            if os.name != "nt":
                self.assertGreaterEqual(build_output.count(sanitizer_flag), 2)

    def test_non_sanitizer_build_does_not_relax_first_party_test_budgets(self):
        result, command, _ = self.configure_consumer("Clang")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotRegex(command, r"(?:-D|/D)KLOGG_(?:SANITIZER|A|M|UB|T)SAN_BUILD")
        self.assertNotIn("KLOGG_MSVC_ASAN", command)

    def test_capturestore_performance_budgets_exclude_instrumented_builds(self):
        capturestore_test = CAPTURESTORE_TEST.read_text()
        release_only_guard = (
            "#if !defined( KLOGG_SANITIZER_BUILD ) && defined( NDEBUG )"
        )
        self.assertGreaterEqual(capturestore_test.count(release_only_guard), 2)
        self.assertNotIn("#if defined( __has_feature )", capturestore_test)

    def test_legacy_msvc_link_flag_path_is_executable(self):
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
                "cmake_minimum_required(VERSION 3.14)\n"
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

    def test_mimalloc_is_pinned_to_an_immutable_commit(self):
        expected_commit = "8c532c32c3c96e5ba1f2283e032f69ead8add00f"
        expected_tree_hash = (
            "f08b3cb3f1dfb1e37558e9f9461913bc69b2062998a42f5365c95fd1e3807db1"
        )
        for cmake_file in (THIRD_PARTY_CMAKE, CPM_PREFETCH_CMAKE):
            with self.subTest(cmake_file=cmake_file):
                contents = cmake_file.read_text()
                package = re.search(
                    r"cpmaddpackage\(\s*NAME\s+mimalloc\b.*?\n\)",
                    contents,
                    re.DOTALL,
                )
                self.assertIsNotNone(package, contents)
                declaration = package.group(0)
                self.assertRegex(
                    declaration, rf"\bGIT_TAG\s+{expected_commit}\b"
                )
                self.assertNotRegex(declaration, r"\bVERSION\s+2\.1\.7\b")
                self.assertRegex(declaration, r"\bDOWNLOAD_ONLY\s+YES\b")
                verifier = (
                    "klogg_add_verified_subdirectory"
                    if cmake_file == THIRD_PARTY_CMAKE
                    else "klogg_require_pinned_source"
                )
                binary_dir = (
                    r"\s+\$\{mimalloc_BINARY_DIR\}"
                    if cmake_file == THIRD_PARTY_CMAKE
                    else ""
                )
                self.assertRegex(
                    contents,
                    rf"{verifier}\(\s*mimalloc\s+"
                    rf"\$\{{mimalloc_SOURCE_DIR\}}{binary_dir}\s+"
                    rf"{expected_commit}\s+{expected_tree_hash}\s*\)",
                )

    def test_cpm_cache_artifact_is_atomic_and_validated(self):
        workflow = CI_BUILD.read_text()
        self.assertIn(
            'tar -czf "$KLOGG_WORKSPACE/cpm-cache.tar.gz"',
            workflow,
        )
        self.assertRegex(
            workflow,
            r"name: cpm-cache\s+path: cpm-cache\.tar\.gz\s+"
            r"if-no-files-found: error\s+compression-level: 0",
        )
        self.assertEqual(
            workflow.count("uses: ./.github/actions/restore-cpm-cache"),
            3,
        )
        self.assertNotRegex(
            workflow,
            r"name: cpm-cache\s+path: \$\{\{ github\.workspace \}\}/cpm_cache",
        )

        restore_action = RESTORE_CPM_CACHE_ACTION.read_text()
        self.assertIn(
            'run: "$GITHUB_WORKSPACE/scripts/restore_cpm_cache.sh"',
            restore_action,
        )

        restore_script = RESTORE_CPM_CACHE_SCRIPT.read_text()
        for guard in (
            'workspace=$( cygpath -u "${workspace}" )',
            'runner_temp=$( cygpath -u "${runner_temp}" )',
            'gzip -t "${archive}"',
            'tar -tzf "${archive}"',
            'tar -xzf "${archive}" -C "${staging}"',
            'mv "${staging}/cpm_cache" "${workspace}/cpm_cache"',
            "check_cpm_cache_contract.sh",
        ):
            self.assertIn(guard, restore_script)
        contract_script = CPM_CACHE_CONTRACT_SCRIPT.read_text()
        for guard in ("roaring.pc.in", "tests/config.h.in", "src/CMakeLists.txt"):
            self.assertIn(guard, contract_script)

    def test_linux_tsan_requires_an_installed_explicit_symbolizer(self):
        dockerfile = UBUNTU_22_TSAN_DOCKERFILE.read_text()
        workflow = CI_BUILD.read_text()
        self.assertRegex(dockerfile, r"\bllvm-14\b")
        self.assertIn("test -x /usr/bin/llvm-symbolizer-14", workflow)
        self.assertIn(
            "TSAN_OPTIONS=halt_on_error=1:external_symbolizer_path=/usr/bin/llvm-symbolizer-14",
            workflow,
        )

    def test_linux_tsan_uses_a_dedicated_instrumented_qt_runtime(self):
        workflow = CI_BUILD.read_text()
        self.assertIn("container_root: docker/ubuntu22.04-tsan", workflow)
        self.assertTrue(
            "container_suffix: _ubuntu22.04-tsan" in workflow,
            "TSan matrix leg must use the unified _ubuntu22.04-tsan image suffix",
        )
        self.assertTrue(
            "${{ env.KLOGG_CI_IMAGE_PREFIX }}${{ matrix.config.container_suffix }}"
            in workflow,
            "TSan image consumers must resolve KLOGG_CI_IMAGE_PREFIX plus container_suffix",
        )
        self.assertIn("-DCMAKE_C_COMPILER=clang-14", workflow)
        self.assertIn("-DCMAKE_CXX_COMPILER=clang++-14", workflow)
        self.assertIn("-DCMAKE_PREFIX_PATH=/opt/qt5-tsan", workflow)
        self.assertIn("-DKLOGG_TSAN_QT_PREFIX=/opt/qt5-tsan", workflow)
        self.assertIn("-DKLOGG_TSAN_QT_VERSION=5.15.19", workflow)
        self.assertIn("timeout-minutes: ${{ matrix.config.timeout_minutes || 60 }}", workflow)
        self.assertNotIn("ignore_noninstrumented_modules", workflow)
        docker_build_action = DOCKER_BUILD_ACTION.read_text()
        self.assertEqual(docker_build_action.count("--env TSAN_OPTIONS"), 2)
        tsan_options = re.findall(r"TSAN_OPTIONS=([^\"\n]+)", workflow)
        self.assertTrue(tsan_options)
        for options in tsan_options:
            self.assertNotIn("suppressions=", options)
        self.assertRegex(
            workflow,
            r"label: ubuntu-22\.04-tsan[\s\S]*?timeout_minutes: 120",
        )

    def test_container_build_forwards_all_strict_sanitizer_options(self):
        docker_build_action = DOCKER_BUILD_ACTION.read_text()
        for option in (
            "ASAN_OPTIONS",
            "UBSAN_OPTIONS",
            "LSAN_OPTIONS",
            "TSAN_OPTIONS",
        ):
            with self.subTest(option=option):
                self.assertEqual(docker_build_action.count(f"--env {option}"), 2)

    def test_macos_tsan_scope_is_explicitly_first_party_only(self):
        workflow = CI_BUILD.read_text()
        project = PROJECT_CMAKE.read_text()
        self.assertIn("label: intel-qt6-first-party-tsan", workflow)
        self.assertIn("prebuilt Qt 6 is not TSan-instrumented", workflow)
        self.assertIn("Linux source-built Qt TSan leg", workflow)
        self.assertIn(
            "macOS TSan covers first-party and source-built dependencies",
            project,
        )

    def test_linux_tsan_image_pins_qt_sources_and_proves_futex_annotations(self):
        dockerfile = UBUNTU_22_TSAN_DOCKERFILE.read_text()
        expected_hashes = (
            "51e91c73abacab81e64efd01bf95794e79c0e605ad80947a024769f0dd620a32",
            "cabc95fc12fa84c92e95f08660742a0ce297e6db3deadbefbe7fbec4769b8c13",
            "2694af886130d63b8a42bcd138f4035fd8108273ebd16781f87300da352f1aa6",
            "967e0d259af13f47e853f183cffc9671cc054380188fee28402f6425132a2c7b",
        )
        for digest in expected_hashes:
            self.assertIn(digest, dockerfile)
        self.assertIn("-sanitize thread", dockerfile)
        self.assertRegex(dockerfile, r"\bxz-utils\b")
        self.assertIn("clang++-14 -fsanitize=thread", dockerfile)
        self.assertIn("ln -s /usr/bin/clang++-14 /usr/local/bin/clang++", dockerfile)
        self.assertIn('archive="${module}-everywhere-opensource-src-${QT_VERSION}.tar.xz"', dockerfile)
        self.assertIn('cd "qtbase-everywhere-src-${QT_VERSION}"', dockerfile)
        self.assertIn("/opt/qt5-tsan", dockerfile)
        self.assertIn("__tsan_acquire", dockerfile)
        self.assertIn("__tsan_release", dockerfile)
        self.assertIn("Qt artifact is not compiler-instrumented", dockerfile)
        self.assertIn('"${QT_TSAN_PREFIX}/lib/libQt5Gui.so.5"', dockerfile)
        self.assertNotRegex(dockerfile, r"apt(?:-get)? install[^\n]*qtbase5-dev")

    def test_linux_tsan_pins_base_and_apt_snapshot(self):
        dockerfile = UBUNTU_22_TSAN_DOCKERFILE.read_text()
        self.assertIn(
            "public.ecr.aws/ubuntu/ubuntu:22.04@sha256:"
            "0199853f6d6b20b0424f3c5694a72a62764f01e6a771b1eb48a4197848986c7e",
            dockerfile,
        )
        self.assertIn("ARG UBUNTU_SNAPSHOT=20260731T000000Z", dockerfile)
        self.assertEqual(
            dockerfile.count(
                "https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/"
            ),
            3,
        )
        self.assertIn('Acquire::Check-Valid-Until "false";', dockerfile)
        self.assertIn(
            "rm /etc/apt/apt.conf.d/99snapshot-bootstrap",
            dockerfile,
        )
        self.assertEqual(dockerfile.count("FROM jammy-snapshot"), 3)
        self.assertNotIn("FROM ubuntu:jammy", dockerfile)

    def test_linux_tsan_qt_backports_atomic_signal_publication(self):
        dockerfile = UBUNTU_22_TSAN_DOCKERFILE.read_text()
        patch = QT_QOBJECT_TSAN_PATCH.read_text()

        for upstream_commit in (
            "d96da673e1e15a44b8dce8a0accc9f9bc194f4b5",
            "2bdad17a2cf1475819057e2eb564e4904152daa5",
            "b2daed079984952d1617b6b696d110efc288b3b3",
            "be31faf39a824f79a1ad8e676512365b59448bea",
            "b295841f70fd4313fc19464681ace39c1d9303ba",
        ):
            self.assertIn(upstream_commit, patch)
        for publication_guard in (
            "connections.storeRelease(cd)",
            "signalVector.storeRelease(newVector)",
            "connectionList.first.storeRelease(c)",
            "list->first.loadAcquire()",
            "connections->signalVector.loadAcquire()",
            "nextConnectionList.loadAcquire()",
            "QAtomicInteger<uint> id",
            "ConnectionDataPointer connections(sp->connections.loadAcquire())",
            "if (!connections || !sp->maybeSignalConnected(signal_index))",
            "receiver.storeRelaxed(nullptr)",
            "receiverThreadData.storeRelaxed(nullptr)",
            "new (&newVector->at(i)) ConnectionList{",
            "vector->at(i).first.loadRelaxed()",
            "vector->at(i).last.loadRelaxed()",
            "const int *argumentTypes = c->argumentTypes.loadAcquire()",
        ):
            self.assertIn(publication_guard, patch)

        self.assertIn(QT_QOBJECT_TSAN_PATCH.name, dockerfile)
        self.assertIn("patch --batch --forward --fuzz=0", dockerfile)

    def test_tsan_qt_tools_build_only_the_required_cli_linguist_package(self):
        dockerfile = UBUNTU_22_TSAN_DOCKERFILE.read_text()
        self.assertIn('"${QT_TSAN_PREFIX}/bin/qmake" qttools.pro', dockerfile)
        self.assertIn("for tool in lconvert lrelease lupdate; do", dockerfile)
        self.assertIn('"${QT_TSAN_PREFIX}/bin/qmake" "${tool}.pro"', dockerfile)
        self.assertIn("make install_cmake_linguist_tools_files", dockerfile)
        for tool in ("lconvert", "lrelease", "lupdate"):
            self.assertIn(f'test -x "${{QT_TSAN_PREFIX}}/bin/{tool}"', dockerfile)
        self.assertIn(
            'test -f "${QT_TSAN_PREFIX}/lib/cmake/Qt5LinguistTools/Qt5LinguistToolsConfig.cmake"',
            dockerfile,
        )

    def test_linux_tsan_image_audits_the_complete_qt_runtime_closure(self):
        dockerfile = UBUNTU_22_TSAN_DOCKERFILE.read_text()
        self.assertIn("libpcre2-16-0", dockerfile)
        self.assertIn(
            "COPY --chmod=0755 verify_elf_runtime_closure.sh "
            "/usr/local/bin/verify_elf_runtime_closure.sh",
            dockerfile,
        )
        self.assertIn(
            '/usr/local/bin/verify_elf_runtime_closure.sh "${QT_TSAN_PREFIX}"',
            dockerfile,
        )

    def test_linux_tsan_elf_runtime_closure_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            artifact_root = root / "qt"
            artifact_root.mkdir()
            (artifact_root / "libQt5Core.so.5").write_text("placeholder")

            fake_file = fake_bin / "file"
            fake_file.write_text(
                "#!/bin/sh\n"
                'printf "%s: ELF 64-bit shared object\\n" "$1"\n'
            )
            fake_file.chmod(0o755)

            fake_ldd = fake_bin / "ldd"
            fake_ldd.write_text(
                "#!/bin/sh\n"
                'printf "libmissing.so => not found\\n"\n'
            )
            fake_ldd.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = f"{fake_bin}{os.pathsep}{env['PATH']}"
            result = subprocess.run(
                ["bash", str(TSAN_ELF_RUNTIME_CLOSURE), str(artifact_root)],
                check=False,
                capture_output=True,
                text=True,
                env=env,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing runtime dependencies", result.stderr)
        self.assertIn("libmissing.so => not found", result.stderr)

    def test_linux_tsan_elf_runtime_closure_propagates_ldd_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            artifact_root = root / "qt"
            artifact_root.mkdir()
            (artifact_root / "libQt5Core.so.5").write_text("placeholder")

            fake_file = fake_bin / "file"
            fake_file.write_text(
                "#!/bin/sh\n"
                'printf "%s: ELF 64-bit shared object\\n" "$1"\n'
            )
            fake_file.chmod(0o755)
            fake_ldd = fake_bin / "ldd"
            fake_ldd.write_text(
                "#!/bin/sh\n"
                'printf "synthetic ldd failure\\n" >&2\n'
                "exit 9\n"
            )
            fake_ldd.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = f"{fake_bin}{os.pathsep}{env['PATH']}"
            result = subprocess.run(
                ["bash", str(TSAN_ELF_RUNTIME_CLOSURE), str(artifact_root)],
                check=False,
                capture_output=True,
                text=True,
                env=env,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("failed to inspect runtime dependencies", result.stderr)
        self.assertIn("synthetic ldd failure", result.stderr)

    def test_linux_tsan_elf_runtime_closure_propagates_find_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            fake_bin = root / "bin"
            fake_bin.mkdir()
            artifact_root = root / "qt"
            artifact_root.mkdir()

            fake_find = fake_bin / "find"
            fake_find.write_text(
                "#!/bin/sh\n"
                'printf "synthetic find failure\\n" >&2\n'
                "exit 9\n"
            )
            fake_find.chmod(0o755)

            env = os.environ.copy()
            env["PATH"] = f"{fake_bin}{os.pathsep}{env['PATH']}"
            result = subprocess.run(
                ["bash", str(TSAN_ELF_RUNTIME_CLOSURE), str(artifact_root)],
                check=False,
                capture_output=True,
                text=True,
                env=env,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("failed to enumerate runtime artifacts", result.stderr)
        self.assertIn("synthetic find failure", result.stderr)

    def test_cmake_installer_is_hash_verified_before_root_execution(self):
        digest = "ea497b4658816010e5850a3ed53845e430654640aabbe10d93fe67def9503e4d"
        workflow = CI_BUILD.read_text()
        dockerfile = UBUNTU_22_TSAN_DOCKERFILE.read_text()
        self.assertIn(digest, workflow)
        self.assertIn("sha256sum --check --strict", workflow)
        self.assertIn(digest, dockerfile)
        self.assertIn("CMAKE_INSTALLER_URL", dockerfile)
        self.assertNotIn("COPY cmake-3.20.2-linux-x86_64.sh", dockerfile)

    def test_tsan_qt_verifier_accepts_only_the_pinned_instrumented_runtime(self):
        result = self.configure_tsan_qt_consumer()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_tsan_qt_verifier_rejects_a_system_qt_target(self):
        result = self.configure_tsan_qt_consumer(core_under_prefix=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outside KLOGG_TSAN_QT_PREFIX", result.stdout + result.stderr)

    def test_tsan_qt_verifier_rejects_an_unexpected_qt_version(self):
        result = self.configure_tsan_qt_consumer(qt_version="5.15.18")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires Qt 5.15.19", result.stdout + result.stderr)

    def test_tsan_qt_verifier_requires_futex_annotations(self):
        result = self.configure_tsan_qt_consumer(annotations=("__tsan_acquire",))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("__tsan_release", result.stdout + result.stderr)

    def test_tsan_qt_verifier_requires_compiler_instrumentation_for_every_target(self):
        for target in ("Gui", "Widgets"):
            with self.subTest(target=target):
                result = self.configure_tsan_qt_consumer(
                    missing_instrumentation_target=target
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    f"Qt5::{target} is not compiler-instrumented",
                    result.stdout + result.stderr,
                )

    def test_tsan_qt_verifier_checks_every_imported_configuration(self):
        result = self.configure_tsan_qt_consumer(
            missing_debug_instrumentation_target="Gui"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "Qt5::Gui is not compiler-instrumented",
            result.stdout + result.stderr,
        )

    def test_tsan_runtime_preflight_rejects_system_qt_and_plugin_fallbacks(self):
        script = TSAN_RUNTIME_PREFLIGHT.read_text()
        self.assertIn("ldd", script)
        self.assertIn('qt_lib_prefix="${qt_prefix}/lib/"', script)
        self.assertIn("libqoffscreen.so", script)
        self.assertIn("QT_DEBUG_PLUGINS=1", script)
        self.assertIn('"${executable}" --help', script)
        self.assertNotIn('"${executable}" --list-tests', script)
        self.assertIn("outside the expected TSan prefix", script)
        self.assertIn("llvm-nm-14", script)
        self.assertIn("Qt library is not compiler-instrumented", script)

    def test_tsan_runtime_preflight_accepts_only_prefix_qt_libraries(self):
        accepted = self.run_tsan_runtime_preflight(qt_under_prefix=True)
        self.assertEqual(accepted.returncode, 0, accepted.stdout + accepted.stderr)

        rejected = self.run_tsan_runtime_preflight(qt_under_prefix=False)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("outside the expected TSan prefix", rejected.stderr)

    def test_tsan_runtime_preflight_requires_the_expected_plugin_to_load(self):
        for plugin_mode in ("metadata-only", "fallback"):
            with self.subTest(plugin_mode=plugin_mode):
                result = self.run_tsan_runtime_preflight(
                    qt_under_prefix=True, plugin_mode=plugin_mode
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("did not load the expected", result.stderr)

    def test_tsan_runtime_preflight_rejects_uninstrumented_qt_gui(self):
        result = self.run_tsan_runtime_preflight(
            qt_under_prefix=True,
            missing_instrumentation_library="libQt5Gui.so.5",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libQt5Gui.so.5", result.stderr)
        self.assertIn("not compiler-instrumented", result.stderr)

    def test_linux_tsan_uses_buildkit_cache_for_the_qt_builder(self):
        workflow = CI_BUILD.read_text()
        self.assertIn(
            "docker/setup-buildx-action@bb05f3f5519dd87d3ba754cc423b652a5edd6d2c",
            workflow,
        )
        self.assertIn(
            "docker/build-push-action@53b7df96c91f9c12dcc8a07bcb9ccacbed38856a",
            workflow,
        )
        self.assertIn("cache-from: type=gha,scope=klogg-qt5-tsan", workflow)
        self.assertIn(
            "cache-to: ${{ github.event_name == 'push' && matrix.config.cache_write && 'type=gha,mode=max,scope=klogg-qt5-tsan' || '' }}",
            workflow,
        )

    def test_linux_lsan_uses_complete_stacks_for_narrow_suppressions(self):
        workflow = CI_BUILD.read_text()
        self.assertIn(
            "LSAN_OPTIONS=suppressions=/usr/local/tests/sanitizers/lsan_suppressions.txt:fast_unwind_on_malloc=0",
            workflow,
        )

    def test_qt_object_graph_roots_have_explicit_owners(self):
        encodings = (ROOT / "src" / "ui" / "include" / "encodings.h").read_text()
        mainwindow = (ROOT / "src" / "ui" / "src" / "mainwindow.cpp").read_text()
        abstract_log_view = (
            ROOT / "src" / "ui" / "src" / "abstractlogview.cpp"
        ).read_text()
        scratchpad = (ROOT / "src" / "ui" / "src" / "scratchpad.cpp").read_text()
        qtests_main = (ROOT / "tests" / "ui" / "qtests_main.cpp").read_text()

        self.assertIn(
            "static QMenu* generate( QActionGroup* actionGroup, QWidget* parent )",
            encodings,
        )
        self.assertIn("menu::encodingTitle ), parent );", encodings)
        self.assertIn("menuBar()->addMenu( EncodingMenu::generate( encodingGroup, menuBar() ) )", mainwindow)
        self.assertIn(
            'new HighlightersMenu( tr( "Highlighters" ), popupMenu_ )',
            abstract_log_view,
        )
        self.assertEqual(scratchpad.count('toolBar->addAction( "'), 7)
        for action_name in (
            "decodeBase64Action",
            "encodeBase64Action",
            "decodeHexAction",
            "encodeHexAction",
            "decodeUrlAction",
            "formatJsonAction",
            "formatXmlAction",
        ):
            self.assertNotIn(f"{action_name}.release()", scratchpad)
        self.assertIn("TestRunner runner( argc, argv );", qtests_main)
        self.assertNotIn("TestRunner* runner = new TestRunner", qtests_main)

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
