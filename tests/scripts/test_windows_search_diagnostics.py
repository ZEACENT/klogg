"""Windows search failures retain diagnostics without altering test outcomes."""

import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import windows_crash_trace_smoke as crash_smoke
from windows_crash_trace_smoke import exception_from_dump, run_smoke

ROOT = pathlib.Path(__file__).resolve().parents[2]


class WindowsSearchDiagnosticsTest(unittest.TestCase):
    def test_unit_and_ui_enable_terminal_trace_before_running_tests(self):
        for relative, terminal in (("tests/unit/tests_main.cpp", "Catch::Session().run"),
                                   ("tests/ui/qtests_main.cpp", "runner.process()")):
            with self.subTest(path=relative):
                source = (ROOT / relative).read_text()
                opt_in = 'qputenv( "KLOGG_TEST_TRACE_SEARCH_TERMINALS", "1" );'
                self.assertIn(opt_in, source)
                self.assertLess(source.index(opt_in), source.index(terminal))

    def test_search_start_trace_is_default_off_and_covers_pre_run_phases(self):
        source = (ROOT / "src/logdata/src/logfiltereddataworker.cpp").read_text()
        self.assertIn('qEnvironmentVariableIsSet( "KLOGG_TEST_TRACE_SEARCH_STARTS" )', source)
        for phase in ("compile.begin", "compile.end", "enqueue.request", "dispatch.selected",
                      "worker.entry", "operation.run"):
            with self.subTest(phase=phase):
                self.assertIn('"' + phase + '"', source)
        self.assertNotIn('qputenv( "KLOGG_TEST_TRACE_SEARCH_STARTS"', source)

    def test_search_start_writer_is_typed_checked_immediate_and_exception_contained(self):
        source = (ROOT / "src/logdata/src/logfiltereddataworker.cpp").read_text()
        start = source.index("void traceSearchStart(")
        trace = source[start:source.index("\nstruct PartialSearchResults", start)]
        self.assertRegex(trace, r"static const bool enabled\s*=\s*qEnvironmentVariableIsSet")
        self.assertRegex(trace, r"if\s*\(\s*!enabled\s*\)\s*\{\s*return;\s*\}")
        disabled_return = trace.index("return;")
        formatting = trace.index("std::ostringstream")
        locking = trace.index("std::lock_guard<std::mutex>")
        self.assertLess(disabled_return, formatting)
        self.assertLess(disabled_return, locking)
        self.assertLess(trace.index("try {"), formatting)
        self.assertIn("std::locale::classic()", trace)
        self.assertRegex(trace, r'<<\s*"Search start "\s*<<\s*phase\s*<<\s*" generation "\s*<<\s*generation')
        self.assertRegex(trace, r'<<\s*" operation "\s*<<\s*operationId\s*<<\s*" type "\s*<<\s*type\s*<<\s*\'\\n\'')
        self.assertIn("static std::mutex", trace)
        self.assertIn("std::cerr.rdbuf()", trace)
        self.assertRegex(trace, r"if\s*\(\s*!buffer\s*\)\s*\{\s*return;\s*\}")
        self.assertRegex(trace, r"if\s*\(\s*buffer->sputn\([^;]+\)\s*!=\s*size\s*\)\s*\{\s*return;\s*\}")
        self.assertRegex(trace, r"if\s*\(\s*buffer->pubsync\(\)\s*!=\s*0\s*\)\s*\{\s*return;\s*\}")
        self.assertEqual(trace.count("sputn("), 1)
        self.assertEqual(trace.count("pubsync()"), 1)
        self.assertLess(locking, trace.index("sputn("))
        self.assertRegex(trace, r"catch\s*\(\s*\.\.\.\s*\)\s*\{[^}]*\}")
        self.assertNotRegex(trace, r"\b(?:fprintf|fflush|printf|fputs|sync_with_stdio)\s*\(")
        self.assertNotRegex(trace, r"std::cerr\s*<<|std::cerr\.(?:flags|imbue|exceptions|tie|setf)\s*\(")
        self.assertNotIn("NOLINT", trace)

    def test_search_start_behavior_has_isolated_fresh_process_native_coverage(self):
        units = (ROOT / "tests/unit/CMakeLists.txt").read_text()
        self.assertIn("foreach(_klogg_trace_mode enabled empty absent)", units)
        self.assertIn('"--unset=KLOGG_TEST_TRACE_SEARCH_STARTS"', units)
        self.assertFalse('"KLOGG_TEST_TRACE_SEARCH_STARTS="' in units,
                         "empty mode must not use the old CMake Windows _wputenv projection")
        self.assertIn('"KLOGG_TEST_TRACE_SEARCH_STARTS=0"', units)
        self.assertIn('"[.search-start-enabled]"', units)
        self.assertIn('"[.search-start-absent]"', units)
        self.assertIn("klogg_set_test_isolation(${_klogg_trace_test})", units)
        native = (ROOT / "tests/unit/logfiltereddataworker_test.cpp").read_text()
        self.assertIn("std::numeric_limits<quint64>::max()", native)
        self.assertIn("worker.shutdownAndWait()", native)
        self.assertIn("worker.search(", native)
        self.assertIn("flushedBeforeReturn", native)
        self.assertIn("hasExpectedFullRequest", native)
        for failure in ("ShortWrite", "RejectWrite", "FailedSync", "ThrowWrite", "ThrowSync"):
            with self.subTest(failure=failure):
                self.assertIn("SearchTraceBuffer::Failure::" + failure, native)

    def test_empty_search_start_environment_uses_explicit_qt_child_block(self):
        units = (ROOT / "tests/unit/CMakeLists.txt").read_text()
        # Windows CMake before 4.2 removes NAME= through _wputenv. This contract
        # checks the source-based defect; it is not an executed Windows ABI test.
        self.assertFalse('"KLOGG_TEST_TRACE_SEARCH_STARTS="' in units,
                         "empty mode must not use the old CMake Windows _wputenv projection")
        empty_branch = units.split('  if(_klogg_trace_mode STREQUAL "empty")', 1)[1].split(
            "  else()", 1)[0]
        self.assertIn("$<TARGET_FILE:search_start_empty_env_launcher>", empty_branch)
        self.assertIn("$<TARGET_FILE:klogg_tests>", empty_branch)
        self.assertIn("${_klogg_trace_runtime_env}", empty_branch)
        self.assertIn('"[.search-start-enabled],[.search-start-empty]"', units)
        helpers = (ROOT / "tests/helpers/CMakeLists.txt").read_text()
        helper_start = helpers.index("add_executable(search_start_empty_env_launcher")
        self.assertLess(helper_start, helpers.index("if(WIN32)"))
        self.assertIn("Qt${QT_VERSION_MAJOR}::Core project_options project_warnings", helpers)
        self.assertIn("klogg_configure_test_target(search_start_empty_env_launcher)", helpers)
        self.assertIn("add_dependencies(klogg_tests search_start_empty_env_launcher)", units)
        launcher = (ROOT / "tests/helpers/search_start_empty_env_launcher.cpp").read_text()
        self.assertIn("QProcessEnvironment::systemEnvironment()", launcher)
        self.assertRegex(launcher, r'environment\.insert\(\s*QStringLiteral\(\s*"KLOGG_TEST_TRACE_SEARCH_STARTS"\s*\),\s*QStringLiteral\(\s*""\s*\)\s*\)')
        self.assertEqual(launcher.count("environment.insert("), 1)
        self.assertIn("child.setProcessEnvironment( environment )", launcher)
        self.assertIn("QProcess::ForwardedChannels", launcher)
        self.assertIn("child.setWorkingDirectory( QDir::currentPath() )", launcher)
        self.assertIn("constexpr int StartTimeoutMs = 5000;", launcher)
        self.assertIn("constexpr int RunTimeoutMs = 60000;", launcher)
        self.assertIn("constexpr int CleanupTimeoutMs = 2000;", launcher)
        self.assertIn("child.waitForStarted( StartTimeoutMs )", launcher)
        self.assertIn("child.waitForFinished( RunTimeoutMs )", launcher)
        self.assertIn("child.terminate()", launcher)
        self.assertIn("child.kill()", launcher)
        self.assertIn("child.waitForFinished( CleanupTimeoutMs )", launcher)
        self.assertIn("child.exitStatus() != QProcess::NormalExit", launcher)
        self.assertIn("return child.exitCode()", launcher)
        self.assertNotRegex(launcher, r"\b(?:qputenv|setenv|_wputenv|SetEnvironmentVariableW)\s*\(")
        self.assertNotIn("QT_VERSION", launcher)
        native = (ROOT / "tests/unit/logfiltereddataworker_test.cpp").read_text()
        precondition = native.split('"[.search-start-empty]"', 1)[1].split("\n}", 1)[0]
        self.assertIn('REQUIRE( qEnvironmentVariableIsSet( "KLOGG_TEST_TRACE_SEARCH_STARTS" ) )', precondition)
        self.assertIn('REQUIRE( qgetenv( "KLOGG_TEST_TRACE_SEARCH_STARTS" ).isEmpty() )', precondition)

    def test_trace_runtime_path_escapes_the_complete_cmake_list_item(self):
        units = (ROOT / "tests/unit/CMakeLists.txt").read_text()
        wiring = units[units.index("foreach(_klogg_trace_mode enabled empty absent)"):]
        escape = 'string(REPLACE ";" "\\\\;" _klogg_trace_runtime_path "${_klogg_trace_runtime_path}")'
        self.assertTrue(escape in wiring,
                        "escape every inherited PATH semicolon before unquoted list expansion")
        self.assertIn('list(APPEND _klogg_trace_runtime_env "${_klogg_trace_runtime_path}")', wiring)
        self.assertIn('$<TARGET_FILE_DIR:klogg_tests>$<SEMICOLON>', wiring)
        self.assertIn('$<TARGET_FILE_DIR:Qt${QT_VERSION_MAJOR}::Core>$<SEMICOLON>$ENV{PATH}', wiring)
        self.assertNotIn('"${_klogg_trace_runtime_env}"', wiring,
                         "the empty POSIX environment list must disappear, not become an empty argument")

    def test_trace_cmake_projection_preserves_windows_path_and_child_argv(self):
        tools = {name: shutil.which(name) for name in ("cmake", "ctest", "ninja")}
        if not all(tools.values()):
            self.skipTest("CMake, CTest, and Ninja are required for argv projection")
        units = (ROOT / "tests/unit/CMakeLists.txt").read_text()
        wiring = units[units.index("foreach(_klogg_trace_mode enabled empty absent)"):]
        inherited_path = (r"C:\Program Files\Git\ucrt64\bin;C:\Program Files\Git\usr\bin;"
                          r"C:\Users\runneradmin\bin;C:\Program Files\CMake\bin;"
                          r"C:\Program Files\Git\cmd;C:\Windows\system32")
        observer = ("import json,os,sys; print(json.dumps({"
                    "'argv':sys.argv[1:], 'path':os.environ.get('PATH'),"
                    "'git':os.environ.get('GIT_CONFIG_PARAMETERS'),"
                    "'present':'KLOGG_TEST_TRACE_SEARCH_STARTS' in os.environ,"
                    "'value':os.environ.get('KLOGG_TEST_TRACE_SEARCH_STARTS')}))")
        native = pathlib.Path(sys.executable).resolve().as_posix()
        launcher = pathlib.Path(tools["cmake"]).resolve().as_posix()
        modes = {
            "enabled": ["-platform", "offscreen", "[.search-start-enabled]"],
            "empty": [native, "[.search-start-enabled],[.search-start-empty]"],
            "absent": ["-platform", "offscreen", "[.search-start-absent]"],
        }
        for windows_branch in (True, False):
            with tempfile.TemporaryDirectory(prefix="trace-path-projection-") as directory:
                source = pathlib.Path(directory) / "source with spaces"
                build = pathlib.Path(directory) / "generated"
                source.mkdir()
                qt_core = source / "Qt SDK with spaces" / "bin" / "QtCore.dll"
                project = (
                    "cmake_minimum_required(VERSION 3.14)\n"
                    "project(trace_argv_projection LANGUAGES NONE)\n"
                    "enable_testing()\n"
                    "set(WIN32 " + ("TRUE" if windows_branch else "FALSE") + ")\n"
                    "set(QT_VERSION_MAJOR 6)\n"
                    "add_executable(klogg_tests IMPORTED GLOBAL)\n"
                    'set_target_properties(klogg_tests PROPERTIES IMPORTED_LOCATION "' + native + '")\n'
                    "add_executable(search_start_empty_env_launcher IMPORTED GLOBAL)\n"
                    'set_target_properties(search_start_empty_env_launcher PROPERTIES IMPORTED_LOCATION "'
                    + launcher + '")\n'
                    "add_library(Qt6::Core SHARED IMPORTED GLOBAL)\n"
                    'set_target_properties(Qt6::Core PROPERTIES IMPORTED_LOCATION "'
                    + qt_core.as_posix() + '")\n'
                    "function(klogg_set_test_isolation test_name)\n"
                    "endfunction()\n"
                    + wiring
                )
                (source / "CMakeLists.txt").write_text(project)
                environment = os.environ.copy()
                environment["PATH"] = inherited_path
                environment["GIT_CONFIG_PARAMETERS"] = "'probe.marker'='caller value with spaces'"
                environment["KLOGG_TEST_TRACE_SEARCH_STARTS"] = "inherited-marker"
                configured = subprocess.run(
                    [tools["cmake"], "-S", str(source), "-B", str(build), "-G", "Ninja",
                     "-DCMAKE_MAKE_PROGRAM=" + tools["ninja"]],
                    env=environment, capture_output=True, text=True, timeout=30)
                self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
                listed = subprocess.run(
                    [tools["ctest"], "--show-only=json-v1"], cwd=build,
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(listed.returncode, 0, listed.stdout + listed.stderr)
                tests = json.loads(listed.stdout)["tests"]
                self.assertEqual(len(tests), 3)
                expected_path = inherited_path
                if windows_branch:
                    expected_path = (pathlib.Path(native).parent.as_posix() + ";"
                                     + qt_core.parent.as_posix() + ";" + inherited_path)
                for test in tests:
                    mode = test["name"].rsplit("_", 1)[1]
                    with self.subTest(windows_branch=windows_branch, mode=mode):
                        command = test["command"]
                        program = launcher if mode == "empty" else native
                        program_index = command.index(program, 3)
                        # Execute the generated cmake -E env prefix unchanged;
                        # replace only the intended target with a portable observer.
                        observed = subprocess.run(
                            command[:program_index] + [sys.executable, "-c", observer]
                            + command[program_index + 1:],
                            cwd=build, env=environment, capture_output=True, text=True, timeout=30)
                        self.assertEqual(observed.returncode, 0,
                                         "generated argv: " + repr(command) + "\n"
                                         + observed.stdout + observed.stderr)
                        record = json.loads(observed.stdout)
                        self.assertEqual(record["path"], expected_path)
                        self.assertEqual(record["git"], environment["GIT_CONFIG_PARAMETERS"])
                        self.assertEqual(record["argv"], modes[mode])
                        path_arguments = [arg for arg in command if arg.startswith("PATH=")]
                        self.assertEqual(path_arguments,
                                         ["PATH=" + expected_path] if windows_branch else [])
                        self.assertNotIn("", command)
                        self.assertEqual(record["present"], mode != "absent")
                        self.assertEqual(record["value"],
                                         None if mode == "absent" else
                                         "0" if mode == "enabled" else "inherited-marker")

    def test_first_chance_evidence_precedes_stackwalk_and_preserves_exception(self):
        source = (ROOT / "tests/helpers/crash_trace.cpp").read_text()
        handler = source[source.index("LONG CALLBACK firstChanceCrashTrace"):]
        trace = source[source.index("void reportFirstChance"):source.index("LONG CALLBACK firstChanceCrashTrace")]
        self.assertRegex(handler, r"reportFirstChance\s*\(\s*info\s*\)")
        self.assertIn("ExceptionAddress", trace)
        self.assertIn("ExceptionInformation", trace)
        self.assertNotIn("MiniDumpWithFullMemory", source)
        self.assertIn("InterlockedCompareExchange", handler)
        self.assertRegex(handler, r"InterlockedExchange\s*\(\s*&crashTraceActive\s*,\s*0\s*\)")
        self.assertIn("&dumpCaptureStarted", trace)
        self.assertLess(trace.index("ExceptionAddress"), trace.index("captureOnce"))
        self.assertLess(trace.index("captureOnce"), trace.index("SymInitialize"))
        self.assertIn("return EXCEPTION_CONTINUE_SEARCH;", handler)
        self.assertIn('L"KLOGG_TEST_MINIDUMP_DIR"', source)
        self.assertIn("CREATE_NEW", source)

    def test_dump_writer_runs_in_a_separate_restricted_collector(self):
        source = (ROOT / "tests/helpers/crash_trace.cpp").read_text()
        self.assertNotRegex(source, r"\bMiniDumpWriteDump\s*\(",
                            "the faulting process must not write its own dump")
        collector = (ROOT / "tests/helpers/collector.cpp").read_text()
        self.assertEqual(len(re.findall(r"\bMiniDumpWriteDump\s*\(", collector)), 1)
        self.assertRegex(collector, r"\.ClientPointers\s*=\s*TRUE\b")
        self.assertIn("MiniDumpNormal", collector)
        self.assertNotIn("MiniDumpWithFullMemory", collector)
        self.assertNotIn("ContextFlags", collector)
        self.assertIn("PROC_THREAD_ATTRIBUTE_HANDLE_LIST", source)
        self.assertIn("STARTUPINFOEXW", source)
        self.assertIn("PROCESS_VM_READ", source)
        self.assertNotIn("PROCESS_ALL_ACCESS", source)
        self.assertIn('L"NUL"', source)
        self.assertIn("AcquireSRWLockShared", source)
        self.assertIn("AcquireSRWLockExclusive", source)
        self.assertIn("RemoveVectoredExceptionHandler", source)
        self.assertIn("__finally", source)

    def test_collector_and_shared_protocol_are_in_the_test_build(self):
        helpers = (ROOT / "tests/helpers/CMakeLists.txt").read_text()
        self.assertIn("add_executable(crash_trace_collector", helpers)
        self.assertIn("add_dependencies(test_crash_trace crash_trace_collector)", helpers)
        self.assertIn("collector.cpp", helpers)
        units = (ROOT / "tests/unit/CMakeLists.txt").read_text()
        self.assertIn("crash_collector_protocol_test.cpp", units)
        self.assertIn("klogg_configure_test_target(klogg_crash_collector_protocol_tests)", units)
        self.assertIn("klogg_set_test_isolation(klogg_crash_collector_protocol_tests)", units)
        target = units.split("  klogg_crash_collector_protocol_tests\n", 1)[1].split("\n)", 1)[0]
        self.assertIn("pure_catch_main.cpp", target)
        protocol_test = (ROOT / "tests/unit/crash_collector_protocol_test.cpp").read_text()
        self.assertNotRegex(protocol_test, r"#\s*define\s+CATCH_CONFIG_(?:MAIN|RUNNER)\b")

    def test_only_unit_and_ui_test_targets_keep_debug_symbols(self):
        for relative, target in (("tests/unit/CMakeLists.txt", "klogg_tests"),
                                 ("tests/ui/CMakeLists.txt", "klogg_itests")):
            with self.subTest(target=target):
                text = (ROOT / relative).read_text()
                self.assertIn(f"klogg_configure_test_target({target} KEEP_DEBUG_SYMBOLS)", text)
        helper = (ROOT / "cmake/TestTargetOptions.cmake").read_text()
        self.assertIn("set(_klogg_debug_option /DEBUG:NONE)", helper)

    def test_dump_decoder_checks_original_exception_and_context_bounds(self):
        layouts = (("x64", 1232), ("x64", 1663), ("x64", 3263),
                   ("x86", 716), ("x86", 1147), ("x86", 1663))
        for architecture, size in layouts:
            with self.subTest(architecture=architecture, size=size), tempfile.TemporaryDirectory() as directory:
                content = bytearray(4096)
                struct.pack_into("<4sI", content, 0, b"MDMP", 0xA793)
                struct.pack_into("<II", content, 8, 1, 32)
                struct.pack_into("<III", content, 32, 6, 168, 44)
                struct.pack_into("<I4xI", content, 44, 42, 0xC0000005)
                struct.pack_into("<Q", content, 68, 0xABC)
                struct.pack_into("<I", content, 76, 2)
                struct.pack_into("<QQ", content, 84, 0, 0x1234)
                struct.pack_into("<II", content, 204, size, 220)
                flag_offset, flags = (48, 0x100003) if architecture == "x64" else (0, 0x10003)
                struct.pack_into("<I", content, 220 + flag_offset, flags)
                alias_offset, alias_value = (0, 0x10003) if architecture == "x64" else (48, 0x100000)
                struct.pack_into("<I", content, 220 + alias_offset, alias_value)
                register_offsets = (248, 152, 160) if architecture == "x64" else (184, 196, 180)
                width = "<Q" if architecture == "x64" else "<I"
                for offset, value in zip(register_offsets, (0xABC, 0xDEF, 0xAAA)):
                    struct.pack_into(width, content, 220 + offset, value)
                dump = pathlib.Path(directory) / "synthetic.dmp"
                dump.write_bytes(content)
                self.assertEqual(exception_from_dump(dump, architecture), {
                    "thread": 42, "code": 0xC0000005, "address": 0xABC,
                    "parameters": 2, "operation": 0, "accessed": 0x1234,
                    "pc": 0xABC, "sp": 0xDEF, "fp": 0xAAA})
                for unsupported_size in (204, 256, 512):
                    struct.pack_into("<II", content, 204, unsupported_size, 220)
                    dump.write_bytes(content)
                    with self.assertRaisesRegex(ValueError, "context"):
                        exception_from_dump(dump, architecture)
                struct.pack_into("<II", content, 204, size, 220)
                struct.pack_into("<I", content, 220 + flag_offset, 0x200003)
                dump.write_bytes(content)
                with self.assertRaisesRegex(ValueError, "context"):
                    exception_from_dump(dump, architecture)
                struct.pack_into("<II", content, 204, 16, len(content) - 10)
                dump.write_bytes(content)
                with self.assertRaisesRegex(ValueError, "context"):
                    exception_from_dump(dump, architecture)

    def write_pe(self, path, architecture="x64"):
        machine, magic = (0x8664, 0x20B) if architecture == "x64" else (0x14C, 0x10B)
        content = bytearray(256)
        content[:2] = b"MZ"
        struct.pack_into("<I", content, 60, 128)
        content[128:132] = b"PE\0\0"
        struct.pack_into("<HH", content, 132, machine, 1)
        struct.pack_into("<H", content, 148, 2)
        struct.pack_into("<H", content, 152, magic)
        path.write_bytes(content)
        return content

    def test_process_architecture_comes_from_bounded_pe_machine_and_magic(self):
        with tempfile.TemporaryDirectory() as directory:
            helper = pathlib.Path(directory) / "helper.exe"
            for architecture in ("x86", "x64"):
                content = self.write_pe(helper, architecture)
                self.assertEqual(crash_smoke.architecture_from_pe(helper), architecture)
                self.assertEqual(crash_smoke.pe_identity.architecture_from_pe(helper), architecture)
                mutations = ("short", "dos", "offset", "signature", "machine", "magic")
                for mutation in mutations:
                    changed = bytearray(content)
                    if mutation == "short":
                        changed = changed[:32]
                    elif mutation == "dos":
                        changed[:2] = b"NO"
                    elif mutation == "offset":
                        struct.pack_into("<I", changed, 60, 0xFFFFFFFF)
                    elif mutation == "signature":
                        changed[128:132] = b"NOPE"
                    elif mutation == "machine":
                        struct.pack_into("<H", changed, 132, 0xAA64)
                    else:
                        struct.pack_into("<H", changed, 152, 0x10B if architecture == "x64" else 0x20B)
                    helper.write_bytes(changed)
                    with self.subTest(architecture=architecture, mutation=mutation):
                        with self.assertRaisesRegex(ValueError, "^test helper PE"):
                            crash_smoke.architecture_from_pe(helper)
                        with self.assertRaisesRegex(ValueError, "^PE"):
                            crash_smoke.pe_identity.architecture_from_pe(helper)

    def test_native_context_requires_control_and_x64_integer_register_flags(self):
        for architecture, size, flag_offset, flags in (("x86", 716, 0, 0x10000),
                                                       ("x64", 1232, 48, 0x100001)):
            with self.subTest(architecture=architecture), tempfile.TemporaryDirectory() as directory:
                content = bytearray(2048)
                struct.pack_into("<4sI", content, 0, b"MDMP", 0xA793)
                struct.pack_into("<II", content, 8, 1, 32)
                struct.pack_into("<III", content, 32, 6, 168, 44)
                struct.pack_into("<II", content, 204, size, 220)
                struct.pack_into("<I", content, 220 + flag_offset, flags)
                dump = pathlib.Path(directory) / "synthetic.dmp"
                dump.write_bytes(content)
                with self.assertRaisesRegex(ValueError, "register flags"):
                    exception_from_dump(dump, architecture)

    def write_extended_context_dump(self, path, architecture):
        content = bytearray(4096)
        struct.pack_into("<4sI", content, 0, b"MDMP", 0xA793)
        struct.pack_into("<II", content, 8, 1, 32)
        struct.pack_into("<III", content, 32, 6, 168, 44)
        struct.pack_into("<I4xI", content, 44, 42, 0xC0000005)
        struct.pack_into("<Q", content, 68, 0xABC)
        struct.pack_into("<I", content, 76, 2)
        struct.pack_into("<QQ", content, 84, 0, 0x1234)
        size, flags, flag_offset = (3263, 0x10004F, 48) if architecture == "x64" else (1147, 0x1007F, 0)
        struct.pack_into("<II", content, 204, size, 220)
        struct.pack_into("<I", content, 220 + flag_offset, flags)
        width, offsets = ("<Q", (248, 152, 160)) if architecture == "x64" else ("<I", (184, 196, 180))
        for offset, value in zip(offsets, (0xABC, 0xDEF, 0x123)):
            struct.pack_into(width, content, 220 + offset, value)
        path.write_bytes(content)

    def test_complete_smoke_checks_extended_context_and_all_disabled_cases(self):
        for architecture in ("x86", "x64"):
            with self.subTest(architecture=architecture), tempfile.TemporaryDirectory() as directory:
                helper = pathlib.Path(directory) / "helper.exe"
                self.write_pe(helper, architecture)
                cases = []

                def capture(command, **kwargs):
                    output = pathlib.Path(kwargs["cwd"])
                    cases.append(output.name)
                    if output.name == "idle":
                        self.assertEqual(command, [str(helper), "--idle"])
                        self.assertEqual(kwargs["env"]["KLOGG_TEST_MINIDUMP_DIR"], str(output))
                        return subprocess.CompletedProcess(command, 0, "", "Test minidump collector ready\nTest minidump collector stopped\n")
                    self.assertEqual(command, [str(helper)])
                    if output.name == "non-directory":
                        self.assertTrue(pathlib.Path(kwargs["env"]["KLOGG_TEST_MINIDUMP_DIR"]).is_file())
                    stderr = ("Exception address abc thread 42\nOriginal PC 0xabc SP 0xdef FP 0x123\n" * 2)
                    if output.name == "enabled":
                        self.write_extended_context_dump(output / "synthetic.dmp", architecture)
                        stderr += "Test minidump collector ready\nTest minidump captured 1 error 0\n"
                    return subprocess.CompletedProcess(command, 67, "", stderr)

                with mock.patch.dict(os.environ, {}, clear=True), \
                        mock.patch("windows_crash_trace_smoke.subprocess.run", side_effect=capture):
                    run_smoke(helper)
                self.assertEqual(cases, ["enabled", "absent", "invalid", "non-directory", "idle"])

    def test_disabled_capture_rejects_collector_launch_without_a_dump(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            result = subprocess.CompletedProcess([], 67, "", "Exception address abc thread 42\n" * 2 + "Test minidump collector ready\n")
            with mock.patch("windows_crash_trace_smoke.subprocess.run", return_value=result):
                with self.assertRaisesRegex(ValueError, "disabled capture.*collector"):
                    crash_smoke.run_smoke_case(output / "helper.exe", output, "absent", "x64")

    def test_idle_collector_rejects_output_and_incomplete_cleanup(self):
        for failure in ("dump", "ready", "stopped", "captured", "returncode", "exception"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                output = pathlib.Path(directory)
                stderr = "Test minidump collector ready\nTest minidump collector stopped\n"
                if failure == "dump":
                    (output / "empty.dmp").write_bytes(b"")
                elif failure in ("ready", "stopped"):
                    stderr = stderr.replace("Test minidump collector " + failure + "\n", "")
                elif failure == "captured":
                    stderr += "Test minidump captured 1 error 0\n"
                elif failure == "exception":
                    stderr += "Exception address abc thread 42\n"
                result = subprocess.CompletedProcess([], 67 if failure == "returncode" else 0, "", stderr)
                with mock.patch("windows_crash_trace_smoke.subprocess.run", return_value=result):
                    with self.assertRaisesRegex(ValueError, "idle collector"):
                        crash_smoke.run_smoke_case(output / "helper.exe", output, "idle", "x64")

    def test_capture_rejects_duplicate_success_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            self.write_extended_context_dump(output / "synthetic.dmp", "x64")
            stderr = "Exception address abc thread 42\nOriginal PC 0xabc SP 0xdef FP 0x123\n" * 2
            stderr += "Test minidump collector ready\n" + "Test minidump captured 1 error 0\n" * 2
            result = subprocess.CompletedProcess([], 67, "", stderr)
            with mock.patch("windows_crash_trace_smoke.subprocess.run", return_value=result):
                with self.assertRaisesRegex(ValueError, "one minimal test dump"):
                    crash_smoke.run_smoke_case(output / "helper.exe", output, "enabled", "x64")

    def test_extended_context_does_not_weaken_original_exception_or_register_parity(self):
        valid = "Exception address abc thread 42\nOriginal PC 0xabc SP 0xdef FP 0x123\n"
        mutations = (("address abc", "address abd"), ("thread 42", "thread 43"),
                     ("PC 0xabc", "PC 0xabd"), ("SP 0xdef", "SP 0xdee"), ("FP 0x123", "FP 0x124"))
        for original, altered in mutations:
            with self.subTest(field=original), tempfile.TemporaryDirectory() as directory:
                helper = pathlib.Path(directory) / "helper.exe"
                self.write_pe(helper)

                def capture(command, **kwargs):
                    self.write_extended_context_dump(pathlib.Path(kwargs["cwd"]) / "synthetic.dmp", "x64")
                    stderr = valid.replace(original, altered) + valid + "Test minidump collector ready\nTest minidump captured 1 error 0\n"
                    return subprocess.CompletedProcess(command, 67, "", stderr)

                with mock.patch.dict(os.environ, {}, clear=True), \
                        mock.patch("windows_crash_trace_smoke.subprocess.run", side_effect=capture):
                    with self.assertRaisesRegex(ValueError, "differs from original exception context"):
                        run_smoke(helper)

    def write_unsupported_context_dump(self, path):
        content = bytearray(1024)
        struct.pack_into("<4sI", content, 0, b"MDMP", 0xA793)
        struct.pack_into("<II", content, 8, 1, 32)
        struct.pack_into("<III", content, 32, 6, 168, 44)
        struct.pack_into("<II", content, 204, 512, 220)
        struct.pack_into("<I", content, 220, 0x10003)
        struct.pack_into("<I", content, 268, 0x100003)
        path.write_bytes(content)
        return bytes(content)

    def test_unsupported_context_reports_actual_size_rva_and_candidate_flags(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "synthetic.dmp"
            self.write_unsupported_context_dump(path)
            with self.assertRaisesRegex(
                    ValueError, r"size 512 RVA 220 flags0 0x00010003 flags48 0x00100003"):
                exception_from_dump(path, "x64")

    def test_failed_synthetic_dump_survives_only_in_explicit_existing_parent(self):
        stderr = ("Exception address abc thread 42\n" * 2
                  + "Test minidump collector ready\nTest minidump captured 1 error 0\n")
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self.write_pe(root / "helper.exe")
            evidence = root / "evidence"
            evidence.mkdir()
            outputs = []
            expected = []

            def capture(command, **kwargs):
                output = pathlib.Path(kwargs["cwd"])
                outputs.append(output)
                expected.append(self.write_unsupported_context_dump(output / "synthetic.dmp"))
                return subprocess.CompletedProcess(command, 67, "", stderr)

            with mock.patch.dict(os.environ, {"KLOGG_TEST_MINIDUMP_DIR": str(evidence)}, clear=True), \
                    mock.patch("windows_crash_trace_smoke.subprocess.run", side_effect=capture):
                with self.assertRaisesRegex(ValueError, "context layout unsupported"):
                    run_smoke(root / "helper.exe")
            self.assertFalse(outputs[0].exists())
            retained = list(evidence.glob("windows-crash-smoke-*"))
            self.assertEqual(len(retained), 1)
            self.assertEqual((retained[0] / "synthetic.dmp").read_bytes(), expected[0])
            self.assertEqual((retained[0] / "helper-stderr.txt").read_text(), stderr)

    def test_failed_smoke_does_not_create_missing_evidence_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self.write_pe(root / "helper.exe")
            missing = root / "missing"

            def capture(command, **kwargs):
                self.write_unsupported_context_dump(pathlib.Path(kwargs["cwd"]) / "synthetic.dmp")
                return subprocess.CompletedProcess(
                    command, 67, "", "Exception address abc thread 42\n" * 2
                    + "Test minidump collector ready\nTest minidump captured 1 error 0\n")

            for environment in ({}, {"KLOGG_TEST_MINIDUMP_DIR": str(missing)}):
                with mock.patch.dict(os.environ, environment, clear=True), \
                        mock.patch("windows_crash_trace_smoke.subprocess.run", side_effect=capture):
                    with self.assertRaisesRegex(ValueError, "context layout unsupported"):
                        run_smoke(root / "helper.exe")
                self.assertFalse(missing.exists())

    def test_windows_dump_probe_runs_in_disposable_process(self):
        helpers = (ROOT / "tests/helpers/CMakeLists.txt").read_text()
        tests = (ROOT / "tests/CMakeLists.txt").read_text()
        self.assertIn("add_executable(crash_trace_test_helper", helpers)
        self.assertIn("NAME windows_crash_trace_smoke", tests)
        self.assertIn("windows_crash_trace_smoke.py", tests)
        self.assertIn("$<TARGET_FILE:crash_trace_test_helper>", tests)

    def test_ci_exports_explicit_dump_and_collects_both_exe_symbol_pairs(self):
        source = (ROOT / ".github/workflows/ci-build.yml").read_text()
        setup = source.split("      - name: Configure crash dumps for Windows test binaries", 1)[1].split(
            "      - name: Stage MSVC AddressSanitizer runtime", 1)[0]
        self.assertIn("KLOGG_TEST_MINIDUMP_DIR=$dumpDir", setup)
        self.assertIn("KLOGG_TEST_TRACE_SEARCH_STARTS=1", setup)
        self.assertIn("$env:GITHUB_ENV", setup)
        collector = source.split("      - name: Collect Windows diagnostics on test failure", 1)[1].split(
            "      - name: Upload Windows diagnostics artifact", 1)[0]
        for name in ("klogg_itests.exe", "klogg_itests.pdb", "klogg_tests.exe", "klogg_tests.pdb"):
            self.assertIn('"' + name + '"', collector)
        self.assertIn("exit 1", source.split("      - name: Fail when tests fail", 1)[1].split(
            "      - uses:", 1)[0])


if __name__ == "__main__":
    unittest.main()
