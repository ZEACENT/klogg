"""Windows search failures retain diagnostics without altering test outcomes."""

import ast
import hashlib
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import textwrap
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

    def cpp_code(self, source):
        # Ignore comments and literals so documentary text cannot satisfy a
        # source contract or make a harmless observation look like a write.
        trivia = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
        return re.sub(trivia, lambda match: " " * len(match[0]), source)

    def assert_collector_capture_contract(self, collector):
        code = self.cpp_code(collector)
        self.assertEqual(len(re.findall(r"\bMiniDumpWriteDump\s*\(", code)), 1)
        call = (r"\bMiniDumpWriteDump\s*\(\s*capabilities\.parent\s*,\s*targetPid\s*,"
                r"\s*capabilities\.output\s*,\s*MiniDumpNormal\s*,\s*&exception\s*,"
                r"\s*nullptr\s*,\s*nullptr\s*\)")
        self.assertRegex(code, call)
        self.assertRegex(code, r"exception\.ThreadId\s*=\s*static_cast\s*<DWORD>\s*"
                         r"\(\s*request\.threadId\s*\)\s*;")
        self.assertEqual(len(re.findall(r"\.ThreadId\s*=(?!=)", code)), 1)
        self.assertRegex(code, r"exception\.ClientPointers\s*=\s*TRUE\s*;")
        self.assertEqual(len(re.findall(r"\.ClientPointers\s*=(?!=)", code)), 1)
        self.assertRegex(code, r"exception\.ExceptionPointers\s*=\s*"
                         r"reinterpret_cast\s*<\s*EXCEPTION_POINTERS\s*\*\s*>\s*\(\s*"
                         r"static_cast\s*<\s*std::uintptr_t\s*>\s*\(\s*"
                         r"request\.exceptionPointers\s*\)\s*\)\s*;")
        self.assertEqual(len(re.findall(r"\.ExceptionPointers\s*=(?!=)", code)), 1)
        self.assertRegex(code, call + r"\s*;\s*const auto error\s*=\s*captured\s*!=\s*FALSE"
                         r"\s*\?\s*ERROR_SUCCESS\s*:\s*GetLastError\s*\(\s*\)\s*;")
        fields = r"(?:ContextFlags|Rip|Eip|Rsp|Esp|Rbp|Ebp)"
        field = r"(?:\.|->)\s*" + fields
        self.assertNotRegex(code, field + r"\s*(?:[+\-*/&|^%]?=(?!=)|<<=|>>=|\+\+|--)")
        self.assertNotRegex(code, r"(?:\+\+|--)\s*[\w.>\-]+" + field)
        self.assertNotRegex(code, r"&\s*[\w.>\-]+" + field)
        self.assertNotRegex(code, r"\brequest\.(?:exceptionPointers|threadId)\s*[+\-*/&|^%]?=(?!=)")
        self.assertNotRegex(code, r"(?:\.|->)\s*ContextRecord\s*=(?!=)")
        copy_operation = (r"\b(?:memcpy|memmove|CopyMemory|MoveMemory|RtlCopyMemory|memset|"
                          r"ZeroMemory|RtlZeroMemory|SecureZeroMemory)\s*\(")
        flag_alias = (r"\b(?:auto|DWORD|std::uint32_t)\s*&\s*(\w+)\s*=\s*"
                      r"[\w.>\-]+(?:\.|->)ContextFlags\s*;")
        for match in re.finditer(flag_alias, code):
            writes = code[match.end():]
            alias = re.escape(match[1])
            self.assertNotRegex(writes, r"\b" + alias + r"\s*(?:[+\-*/&|^%]?=(?!=)|<<=|>>=|\+\+|--)")
            self.assertNotRegex(writes, r"(?:\+\+|--)\s*" + alias + r"\b")
        self.assertNotRegex(code, copy_operation + r"[^,;]*\bContextRecord\b")
        self.assertNotRegex(code, r"\*\s*\([^;]*\bContextRecord\b[^;]*\)\s*=(?!=)")
        aliases = re.findall(r"\b(?:auto|CONTEXT|PCONTEXT)\s*\*?\s*(\w+)\s*=\s*"
                             r"[^;]*\bContextRecord\b[^;]*;", code)
        for alias in aliases:
            name = re.escape(alias)
            self.assertNotRegex(code, r"\*\s*" + name + r"\s*=(?!=)")
            self.assertNotRegex(code, copy_operation + r"\s*&?\s*" + name + r"\b")
        self.assertNotRegex(code, r"\b(?:WriteProcessMemory|SetThreadContext|OpenProcess|OpenThread|"
                            r"InitializeContext|InitializeContext2|LocateXStateFeature|"
                            r"SetXStateFeaturesMask|LoadLibrary[AW]?|VirtualProtectEx)\s*\(")
        self.assertNotRegex(code, r"sizeof\s*\(\s*CONTEXT\s*\)\s*[+*]")
        self.assertNotRegex(code, r"[+*]\s*sizeof\s*\(\s*CONTEXT\s*\)")
        self.assertNotIn("MiniDumpWithFullMemory", code)

    def collector_fixture(self):
        return """
        exception.ThreadId = static_cast<DWORD>( request.threadId );
        exception.ExceptionPointers = reinterpret_cast<EXCEPTION_POINTERS*>(
            static_cast<std::uintptr_t>( request.exceptionPointers ) );
        exception.ClientPointers = TRUE;
        const auto captured = MiniDumpWriteDump( capabilities.parent, targetPid,
            capabilities.output, MiniDumpNormal, &exception, nullptr, nullptr );
        const auto error = captured != FALSE ? ERROR_SUCCESS : GetLastError();
        """

    def test_collector_guard_allows_readonly_context_flag_observation(self):
        source = self.collector_fixture()
        self.assert_collector_capture_contract(source)
        for observation in ("const auto observedFlags = context.ContextFlags;",
                            "if (context.ContextFlags == expected) { recordFlags(context.ContextFlags); }",
                            "const auto observedXState = context.ContextFlags & CONTEXT_XSTATE;",
                            "const auto& flags = observedContext.ContextFlags; recordFlags(flags);",
                            'const char* text = "context.ContextFlags &= ~CONTEXT_XSTATE;";'):
            with self.subTest(observation=observation):
                self.assert_collector_capture_contract(source + observation)

    def test_collector_guard_rejects_mutation_copyback_and_changed_dump_arguments(self):
        source = self.collector_fixture()
        mutations = (
            source.replace("MiniDumpNormal", "MiniDumpWithThreadInfo"),
            source.replace("&exception, nullptr, nullptr", "&exception, nullptr, &callback"),
            source.replace("&exception, nullptr, nullptr", "&replacement, nullptr, nullptr"),
            source.replace("request.exceptionPointers", "scratchPointers"),
            source.replace("ClientPointers = TRUE", "ClientPointers = FALSE"),
            source + "context.ContextFlags &= ~CONTEXT_XSTATE;",
            source + "context.ContextFlags = CONTEXT_CONTROL;",
            source + "++context.ContextFlags;",
            source + "memcpy( info->ContextRecord, &scratch, sizeof( CONTEXT ) );",
            source + "WriteProcessMemory( parent, remoteContext, &scratch, size, nullptr );",
            source + "SetThreadContext( thread, &scratch );",
            source + "const auto bytes = sizeof( CONTEXT ) + 1024;",
            source + "OpenProcess( PROCESS_VM_READ, FALSE, pid );",
            source.replace("request.threadId", "replacementThread"),
            source + "request.exceptionPointers = scratchPointers;",
            source + "request.threadId = 999;",
            source + "auto* originalContext = info->ContextRecord; *originalContext = scratch;",
            source + "auto* originalContext = info->ContextRecord; memcpy(originalContext, &scratch, bytes);",
            source + "*(info->ContextRecord) = scratch;",
            source + "auto& flags = observedContext.ContextFlags; flags &= ~CONTEXT_XSTATE;",
            source + "memset( info->ContextRecord, 0, sizeof( CONTEXT ) );",
        )
        for index, mutated in enumerate(mutations):
            with self.subTest(mutation=index), self.assertRaises(AssertionError):
                self.assert_collector_capture_contract(mutated)
        spoof = source.replace("MiniDumpNormal", "MiniDumpWithThreadInfo")
        with self.assertRaises(AssertionError):
            self.assert_collector_capture_contract(spoof + "// " + source.replace("\n", " "))

    def test_postfailure_stack_observation_is_bounded_and_labels_actual_coverage(self):
        collector = (ROOT / "tests/helpers/collector.cpp").read_text()
        stack = self.cpp_function_body(collector, "readStack")
        self.assertIn("diagnosticStackPlan( data.input, address, data.result.requestedBytes )", stack)
        self.assertRegex(stack, r"std::array<std::uint8_t, maximumDiagnosticReadBytes> stackScratch")
        self.assertRegex(stack, r"if\s*\(\s*plan.readBytes != 0\s*\)")
        self.assertRegex(stack, r"readDiagnosticObject\(\s*capabilities_, address, stackScratch.data\(\),"
                         r"\s*plan.readBytes, DiagnosticObject::Stack\s*\)")
        self.assertIn("diagnosticTruncated", stack)
        self.assertIn("diagnosticUnavailable", stack)
        self.assert_collector_capture_contract(collector)
        parent = (ROOT / "tests/helpers/crash_trace.cpp").read_text()
        self.assertIn("summarizeDiagnosticStack( data )", parent)
        for field in ("stack-span", "boundsKnown", "desiredBytes", "requestedBytes",
                      "transferredBytes", "complete", "diagnosticTruncated"):
            self.assertIn(field, parent)
        self.assertIn('\\"extendedXstate\\":\\"unknown\\"', parent)

    def test_dump_writer_runs_in_a_separate_restricted_collector(self):
        source = (ROOT / "tests/helpers/crash_trace.cpp").read_text()
        self.assertNotRegex(source, r"\bMiniDumpWriteDump\s*\(",
                            "the faulting process must not write its own dump")
        collector = (ROOT / "tests/helpers/collector.cpp").read_text()
        self.assert_collector_capture_contract(collector)
        self.assertIn("PROC_THREAD_ATTRIBUTE_HANDLE_LIST", source)
        self.assertIn("STARTUPINFOEXW", source)
        self.assertIn("PROCESS_VM_READ", source)
        self.assertNotIn("PROCESS_ALL_ACCESS", source)
        self.assertIn('L"NUL"', source)
        self.assertIn("AcquireSRWLockShared", source)
        self.assertIn("AcquireSRWLockExclusive", source)
        self.assertIn("RemoveVectoredExceptionHandler", source)
        self.assertIn("__finally", source)

    def cpp_function_body(self, source, name):
        code = self.cpp_code(source)
        definitions = list(re.finditer(r"^[ \t]*(?:\[\[noreturn\]\]\s+)?(?:inline\s+)?"
                                       r"(?:void|bool|int|Outcome|DiagnosticObservation)\s+" + re.escape(name)
                                       + r"\s*\([^{};]*\)\s*(?:noexcept\s*)?\{", code,
                                       re.MULTILINE))
        self.assertEqual(len(definitions), 1, "need one executable definition of " + name)
        start = definitions[0].end() - 1
        depth = 0
        for index in range(start, len(code)):
            depth += (code[index] == "{") - (code[index] == "}")
            if depth == 0:
                return code[start:index + 1]
        self.fail("unterminated function " + name)

    def assert_first_error_contract(self, protocol, collector):
        decision = self.cpp_function_body(protocol, "includeOutputCloseFailure")
        self.assertEqual(re.sub(r"\s+", "", decision),
                         "{if(!closed&&outcome.succeeded){return{Stage::Dump,ErrorDomain::Win32,"
                         "closeError,outcome.acknowledged,false};}returnoutcome;}")
        finish = self.cpp_function_body(collector, "finishRequest")
        self.assertRegex(finish, r"const auto closed\s*=\s*CloseHandle\(\s*capabilities.output\s*\)"
                         r"\s*!=\s*FALSE\s*;\s*const auto closeError\s*=\s*closed\s*\?\s*"
                         r"ERROR_SUCCESS\s*:\s*GetLastError\(\s*\)\s*;")
        self.assertRegex(finish, r"const auto finalized\s*=\s*includeOutputCloseFailure\s*\(")
        for field in ("succeeded", "domain"):
            self.assertRegex(finish, field + r"\s*=\s*finalized\." + field + r"\s*;")
        self.assertRegex(finish, r"error\s*=\s*static_cast\s*<DWORD>\s*\(\s*finalized.error\s*\)\s*;")
        self.assertEqual(len(re.findall(r"\berror\s*=(?!=)", finish)), 1)
        self.assertEqual(len(re.findall(r"\bdomain\s*=(?!=)", finish)), 1)
        self.assertLess(finish.index("includeOutputCloseFailure("),
                        finish.index("publishAcknowledgement("))

    def test_output_close_first_error_guard_uses_production_seam_and_rejects_overwrite(self):
        protocol = (ROOT / "tests/helpers/crash_collector_protocol.h").read_text()
        collector = (ROOT / "tests/helpers/collector.cpp").read_text()
        self.assert_first_error_contract(protocol, collector)
        overwritten = protocol.replace("!closed && outcome.succeeded", "!closed")
        self.assertNotEqual(overwritten, protocol)
        with self.assertRaises(AssertionError):
            self.assert_first_error_contract(overwritten, collector)
        detached = collector.replace("const auto finalized = includeOutputCloseFailure(",
                                     "const auto finalized = unusedCloseSimulator(")
        self.assertNotEqual(detached, collector)
        with self.assertRaises(AssertionError):
            self.assert_first_error_contract(protocol, detached + "// includeOutputCloseFailure")

    def test_native_first_error_matrix_covers_same_production_decision(self):
        native = self.cpp_code((ROOT / "tests/unit/crash_collector_protocol_test.cpp").read_text())
        self.assertRegex(native, r"const Outcome failed\s*\{\s*Stage::Dump\s*,\s*"
                         r"ErrorDomain::DumpHResult\s*,\s*0x8007012bU\s*,\s*false\s*,\s*false\s*\}")
        for outcome, closed, error in (("dumped", "true", "0"), ("dumped", "false", "6"),
                                       ("failed", "true", "0"), ("failed", "false", "6")):
            self.assertRegex(native, r"includeOutputCloseFailure\s*\(\s*" + outcome
                             + r"\s*,\s*" + closed + r"\s*,\s*" + error + r"\s*\)")
        for result in ("dumpFailed", "bothFailed"):
            self.assertRegex(native, r"CHECK\s*\(\s*" + result
                             + r"\.domain\s*==\s*ErrorDomain::DumpHResult\s*\)")
            self.assertRegex(native, r"CHECK\s*\(\s*" + result
                             + r"\.error\s*==\s*0x8007012bU\s*\)")
        self.assertRegex(native, r"CHECK_FALSE\s*\(\s*closeFailed.succeeded\s*\)")
        self.assertRegex(native, r"CHECK\s*\(\s*closeFailed.domain\s*==\s*ErrorDomain::Win32\s*\)")

    def assert_diagnostic_bounds_contract(self, protocol, collector):
        wire = self.cpp_code(protocol)
        limits = {"diagnosticBytes": "16U * 1024U", "maximumDiagnosticReads": "16",
                  "maximumDiagnosticReadBytes": "4U * 1024U", "maximumDiagnosticQueries": "32",
                  "maximumExceptionChain": "3", "maximumStackSample": "256",
                  "diagnosticModulesPerProcess": "3", "maximumDiagnosticPath": "512"}
        for name, expression in limits.items():
            self.assertRegex(wire, r"\b" + name + r"\s*=\s*"
                             + r"\s*".join(re.escape(part) for part in expression.split()) + r"\s*;")
        self.assertRegex(wire, r"\bprotocolVersion\s*=\s*1\s*;")
        self.assertRegex(wire, r"sizeof\(\s*SharedPage\s*\)\s*==\s*72\b")
        self.assertRegex(wire, r"sizeof\(\s*SharedEnvelope\s*\)\s*==\s*72\s*\+\s*diagnosticBytes")
        shared = re.search(r"struct alignas\(\s*8\s*\) SharedPage\s*(\{.*?^\};)", wire,
                           re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(shared)
        self.assertEqual(re.sub(r"\s+", "", shared[1]),
                         "{Headerheader{};Requestrequest{};Acknowledgementacknowledgement{};"
                         "alignas(4)std::int32_trequestPublication{};"
                         "alignas(4)std::int32_tacknowledgementPublication{};};")
        outcome = re.search(r"struct Outcome\s*(\{.*?^\};)", wire, re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(outcome)
        self.assertEqual(re.sub(r"\s+", "", outcome[1]),
                         "{Stagestage{Stage::None};ErrorDomaindomain{ErrorDomain::None};"
                         "std::uint32_terror{};boolacknowledged{};boolsucceeded{};};")
        reporting = self.cpp_function_body(protocol, "diagnosticReportingAllowed")
        self.assertRegex(reporting, r"return state.captureRequested\s*&&\s*state.collectorReaped\s*&&\s*"
                         r"validDiagnosticResult\(\s*data,\s*bits,\s*data.resultPublication\s*==")
        read = self.cpp_function_body(collector, "readDiagnosticObject")
        query = self.cpp_function_body(collector, "queryDiagnosticRegion")
        for marker in ("queryDiagnosticRegion(", "readableDiagnosticRegion(", "reserveDiagnosticRead(",
                       "ReadProcessMemory("):
            self.assertIn(marker, read)
        for marker in ("reserveDiagnosticQuery(", "VirtualQueryEx("):
            self.assertIn(marker, query)
        self.assertLess(read.index("queryDiagnosticRegion("), read.index("ReadProcessMemory("))
        self.assertLess(read.index("readableDiagnosticRegion("), read.index("ReadProcessMemory("))
        self.assertLess(read.index("reserveDiagnosticRead("), read.index("ReadProcessMemory("))
        self.assertLess(query.index("reserveDiagnosticQuery("), query.index("VirtualQueryEx("))
        self.assertRegex(read, r"if\s*\(\s*!readableDiagnosticRegion\(\s*record.state,\s*"
                         r"record.protection\s*\)\s*\)\s*\{[^}]*return false;")
        self.assertRegex(read, r"if\s*\(\s*read == FALSE\s*\|\|\s*record.transferred != segment\s*\)"
                         r"\s*\{[^}]*return false;")
        code = self.cpp_code(collector)
        for api in ("ReadProcessMemory", "VirtualQueryEx"):
            self.assertEqual(len(re.findall(r"\b" + api + r"\s*\(", code)), 1)
        failure = self.cpp_function_body(collector, "collectFailureDiagnostics")
        self.assertRegex(failure, r"auto& data\s*=\s*\*capabilities.diagnostics\s*;")
        self.assertRegex(failure, r"NativeDiagnosticProbes probes\s*\{\s*capabilities\s*\};\s*"
                         r"probeDiagnosticObjects\(\s*data,\s*probes\s*\);")
        native_context = self.cpp_function_body(collector, "readContext")
        self.assertRegex(native_context, r"CONTEXT observedContext\s*\{\s*\};")
        self.assertRegex(native_context, r"if\s*\(\s*!readDiagnosticObject\(\s*capabilities_,\s*address,\s*"
                         r"&observedContext,\s*sizeof\(\s*observedContext\s*\),\s*DiagnosticObject::Context\s*"
                         r"\)\s*\)\s*\{\s*return false;\s*\}")
        self.assertRegex(native_context, r"contextFlags\s*=\s*static_cast<std::uint32_t>\(\s*"
                         r"observedContext.ContextFlags\s*\)")
        sequence = self.cpp_function_body(protocol, "probeDiagnosticObjects")
        self.assertRegex(sequence, r"auto exceptionRecord\s*=\s*data.input.exceptionRecord\s*;\s*"
                         r"auto contextRecord\s*=\s*data.input.contextRecord\s*;")
        self.assertRegex(sequence, r"if\s*\(\s*probes.readPointers\([^;{}]*\)\s*\)\s*\{\s*"
                         r"data.result.decoded\s*\|=\s*pointersDecoded\s*;\s*"
                         r"exceptionRecord\s*=\s*observedException\s*;\s*"
                         r"contextRecord\s*=\s*observedContext\s*;\s*\}\s*"
                         r"data.result.exceptionRecord\s*=\s*exceptionRecord\s*;\s*"
                         r"data.result.contextRecord\s*=\s*contextRecord\s*;")
        self.assertIn("maximumExceptionChain", sequence)
        self.assertIn("visitDiagnosticException(", sequence)
        self.assertRegex(sequence, r"if\s*\(\s*probes.readContext\(\s*contextRecord\s*\)\s*\)\s*\{\s*"
                         r"data.result.decoded\s*\|=\s*contextDecoded\s*;\s*\}")
        self.assertRegex(sequence, r"const auto sp\s*=\s*\(\s*data.result.decoded & contextDecoded\s*\)"
                         r"\s*!=\s*0\s*\?\s*data.result.sp\s*:\s*data.input.sp\s*;\s*probes.readStack\(\s*sp\s*\);")
        self.assertIn("maximumStackSample", self.cpp_function_body(collector, "readStack"))
        main = self.cpp_function_body(collector, "wmain")
        self.assertLess(main.index("sizeof( SharedPage )"), main.index("sizeof( SharedEnvelope )"))
        self.assertRegex(main, r"if\s*\(\s*capabilities.diagnosticView != nullptr\s*&&\s*"
                         r"validDiagnosticHeader\([^;{}]*\)\s*\)\s*\{\s*"
                         r"capabilities.diagnostics\s*=\s*&capabilities.diagnosticView->diagnostics.data;")

    def test_diagnostic_bounds_guard_rejects_growth_and_unreserved_remote_reads(self):
        protocol = (ROOT / "tests/helpers/crash_collector_protocol.h").read_text()
        collector = (ROOT / "tests/helpers/collector.cpp").read_text()
        self.assert_diagnostic_bounds_contract(protocol, collector)
        mutations = (
            (protocol.replace("diagnosticBytes = 16U * 1024U", "diagnosticBytes = 32U * 1024U"), collector),
            (protocol.replace("maximumDiagnosticReads = 16", "maximumDiagnosticReads = 160"), collector),
            (protocol.replace("maximumDiagnosticPath = 512", "maximumDiagnosticPath = 1024"), collector),
            (protocol.replace("state.captureRequested && state.collectorReaped", "state.captureRequested"), collector),
            (protocol, collector.replace("!reserveDiagnosticRead( data.result,", "!unreservedRead( data.result,")),
            (protocol, collector.replace("!readableDiagnosticRegion( record.state,", "!uncheckedRegion( record.state,")),
            (protocol, collector.replace("probeDiagnosticObjects( data, probes );",
                                         "unusedProbeSimulator( data, probes );")),
            (protocol.replace("auto contextRecord = data.input.contextRecord;",
                              "auto contextRecord = observedContext;"), collector),
            (protocol, collector.replace("if ( !readDiagnosticObject( capabilities_, address, &observedContext,",
                                         "if ( false && !readDiagnosticObject( capabilities_, address, &observedContext,")),
        )
        for index, (changed_protocol, changed_collector) in enumerate(mutations):
            self.assertNotEqual((changed_protocol, changed_collector), (protocol, collector))
            with self.subTest(mutation=index), self.assertRaises(AssertionError):
                self.assert_diagnostic_bounds_contract(changed_protocol, changed_collector)

    def assert_collector_diagnostic_order(self, protocol, collector):
        main = self.cpp_function_body(collector, "wmain")
        finish = self.cpp_function_body(collector, "finishRequest")
        observation = self.cpp_function_body(protocol, "observeDumpOutcome")
        self.assertRegex(main, r"const Outcome primary\s*\{\s*captured != FALSE\s*\?\s*Stage::None\s*:\s*Stage::Dump,\s*"
                         r"captured != FALSE\s*\?\s*ErrorDomain::None\s*:\s*ErrorDomain::DumpHResult,\s*"
                         r"static_cast<std::uint32_t>\(\s*error\s*\),\s*false,\s*captured != FALSE\s*\};")
        self.assertRegex(main, r"const auto observation\s*=\s*observeDumpOutcome\(\s*capabilities.diagnostics,\s*"
                         r"request,\s*nativePointerBits,\s*inputPublished == static_cast<LONG>\(\s*captureSequence\s*\),\s*"
                         r"primary,\s*\[&capabilities\]\(\s*DiagnosticData&\s*\)\s*noexcept\s*\{\s*"
                         r"collectFailureDiagnostics\(\s*capabilities\s*\);\s*\}\s*\);")
        self.assertRegex(main, r"capabilities.diagnosticRequested\s*=\s*observation.requested\s*;\s*"
                         r"finishRequest\(\s*capabilities,\s*Phase::Complete,\s*observation.primary.succeeded,\s*"
                         r"observation.primary.domain,\s*static_cast<DWORD>\(\s*observation.primary.error\s*\)\s*\);")
        self.assertRegex(observation, r"if\s*\(\s*data == nullptr\s*\|\|\s*!validDiagnosticInput\(\s*"
                         r"\*data,\s*request,\s*bits,\s*inputPublished\s*\)\s*\)\s*\{\s*return\s*"
                         r"\{\s*primary,\s*false\s*\};\s*\}")
        self.assertLess(observation.index("validDiagnosticInput("), observation.index("data->result ="))
        self.assertRegex(observation, r"data->result =\s*\{\s*\};\s*data->result.sequence = request.sequence;\s*"
                         r"data->result.threadId = request.threadId;\s*"
                         r"data->result.dumpSucceeded = primary.succeeded\s*\?\s*1U\s*:\s*0U;\s*"
                         r"data->result.dumpDomain = static_cast<std::uint32_t>\(\s*primary.domain\s*\);\s*"
                         r"data->result.dumpError = primary.error;")
        self.assertRegex(observation, r"if\s*\(\s*!primary.succeeded\s*\)\s*\{\s*collectFailure\(\s*\*data\s*\);\s*\}")
        self.assertEqual(len(re.findall(r"\bcollectFailure\s*\(", observation)), 1)
        self.assertRegex(observation, r"return\s*\{\s*primary,\s*true\s*\};\s*\}$")
        self.assertNotRegex(observation, r"\bprimary(?:\.\w+)?\s*=(?!=)")
        self.assertEqual(len(re.findall(r"\bcollectFailureDiagnostics\s*\(", main)), 1)
        self.assertLess(main.index("const auto error ="), main.index("const Outcome primary"))
        self.assertLess(main.index("const Outcome primary"), main.index("observeDumpOutcome("))
        self.assertLess(main.index("collectFailureDiagnostics("), main.rindex("finishRequest("))
        for marker in ("CloseHandle(", "resultPublication", "publishAcknowledgement("):
            self.assertIn(marker, finish)
        self.assertLess(finish.index("CloseHandle("), finish.index("resultPublication"))
        self.assertLess(finish.index("resultPublication"), finish.index("publishAcknowledgement("))
        after_completion = finish[finish.index("publishAcknowledgement("):]
        self.assertNotRegex(after_completion, r"\b(?:collectFailureDiagnostics|ReadProcessMemory|"
                            r"VirtualQueryEx|readDiagnosticObject|queryDiagnosticRegion)\s*\(")
        self.assertNotIn("capabilities.diagnostics", after_completion)

    def test_collector_order_guard_rejects_success_probes_and_post_completion_activity(self):
        protocol = (ROOT / "tests/helpers/crash_collector_protocol.h").read_text()
        collector = (ROOT / "tests/helpers/collector.cpp").read_text()
        self.assert_collector_diagnostic_order(protocol, collector)
        after_completion = collector.replace(
            "exitCollector( capabilities,\n                   succeeded ?",
            "collectFailureDiagnostics( capabilities );\n"
            "    exitCollector( capabilities,\n                   succeeded ?")
        mutations = (
            (protocol.replace("if ( !primary.succeeded )", "if ( true )"), collector),
            (protocol, after_completion),
            (protocol.replace("!validDiagnosticInput( *data, request, bits, inputPublished )",
                              "!uncheckedInput( *data, request, bits, inputPublished )"), collector),
            (protocol.replace("return { primary, false };", "return { Outcome{}, false };"), collector),
            (protocol, collector.replace("const auto observation = observeDumpOutcome(",
                                         "const auto observation = unusedObservationSimulator(")),
            (protocol, collector.replace("observation.primary.succeeded,", "true,")),
        )
        for index, (changed_protocol, changed_collector) in enumerate(mutations):
            self.assertNotEqual((changed_protocol, changed_collector), (protocol, collector))
            with self.subTest(mutation=index), self.assertRaises(AssertionError):
                self.assert_collector_diagnostic_order(changed_protocol, changed_collector)

    def assert_parent_diagnostic_order(self, parent):
        trace = self.cpp_function_body(parent, "reportFirstChance")
        report = self.cpp_function_body(parent, "reportCollectorDiagnostics")
        inputs = self.cpp_function_body(parent, "publishDiagnosticInput")
        self.assertRegex(trace, r"const auto outcome\s*=\s*captureOnce\([^;]*\)\s*;\s*"
                         r"reportCollectorDiagnostics\(\s*\)\s*;\s*if\s*\(\s*outcome.succeeded\s*\)")
        self.assertLess(trace.index("publishDiagnosticInput("), trace.index("captureOnce("))
        self.assertLess(trace.index("reportCollectorDiagnostics("), trace.index("SymInitialize("))
        self.assertRegex(report, r"if\s*\(\s*!clientState.captureRequested\s*\|\|\s*"
                         r"!clientState.collectorReaped\s*\)\s*\{\s*return;\s*\}")
        self.assertRegex(report, r"channel.diagnostics == nullptr\s*\|\|\s*"
                         r"!diagnosticReportingAllowed\(\s*clientState,\s*\*channel.diagnostics,\s*"
                         r"nativePointerBits\s*\)")
        self.assertLess(report.index("diagnosticReportingAllowed("),
                        report.index("const auto& data = *channel.diagnostics"))
        self.assertNotRegex(report, r"clientState\.\w+\s*=(?!=)")
        self.assertNotRegex(report, r"\b(?:captureOnce|ReadProcessMemory|VirtualQueryEx|"
                            r"publishRequest|MiniDumpWriteDump)\s*\(")
        self.assertRegex(inputs, r"input.contextFlags\s*=\s*static_cast<std::uint32_t>\(\s*"
                         r"info->ContextRecord->ContextFlags\s*\)")
        self.assertNotRegex(inputs, r"(?:\.|->)ContextFlags\s*(?:[+\-*/&|^%]?=(?!=)|<<=|>>=|\+\+|--)")
        self.assertNotRegex(inputs, r"\b(?:memcpy|memmove|memset|WriteProcessMemory|SetThreadContext)\s*\(")
        self.assertLess(inputs.index("input.contextFlags"), inputs.index("inputPublication"))

    def test_parent_report_guard_rejects_early_reap_bypass_and_diagnostic_success_substitution(self):
        parent = (ROOT / "tests/helpers/crash_trace.cpp").read_text()
        self.assert_parent_diagnostic_order(parent)
        early = parent.replace("        reportCollectorDiagnostics();\n", "")
        early = early.replace("        const auto outcome = captureOnce(",
                              "        reportCollectorDiagnostics();\n        const auto outcome = captureOnce(")
        mutations = (
            early,
            parent.replace("!clientState.captureRequested || !clientState.collectorReaped",
                           "!clientState.captureRequested"),
            parent.replace("if ( outcome.succeeded )", "if ( channel.diagnostics != nullptr )"),
            parent.replace("    data.input = {};", "    info->ContextRecord->ContextFlags &= ~CONTEXT_XSTATE;\n    data.input = {};"),
        )
        for index, mutated in enumerate(mutations):
            self.assertNotEqual(mutated, parent)
            with self.subTest(mutation=index), self.assertRaises(AssertionError):
                self.assert_parent_diagnostic_order(mutated)

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

    def test_capture_rejects_symlink_output_before_decoding_external_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            output = root / "enabled"
            output.mkdir()
            external = root / "not-owned.dmp"
            self.write_extended_context_dump(external, "x64")
            (output / "synthetic.dmp").symlink_to(external)
            stderr = "Exception address abc thread 42\nOriginal PC 0xabc SP 0xdef FP 0x123\n" * 2
            stderr += "Test minidump collector ready\nTest minidump captured 1 error 0\n"
            result = subprocess.CompletedProcess([], 67, "", stderr)
            with mock.patch("windows_crash_trace_smoke.subprocess.run", return_value=result), \
                    mock.patch("windows_crash_trace_smoke.exception_from_dump", side_effect=AssertionError(
                        "must not decode external symlink target")):
                with self.assertRaisesRegex(ValueError, "one minimal test dump"):
                    crash_smoke.run_smoke_case(output / "helper.exe", output, "enabled", "x64")

    def test_diagnostic_text_cannot_spoof_capture_or_first_chance_success(self):
        valid = "Exception address abc thread 42\nOriginal PC 0xabc SP 0xdef FP 0x123\n" * 2
        cases = (
            valid + 'Test minidump diagnostic {"text":"Test minidump captured 1 error 0"}\n',
            'Test minidump diagnostic {"text":"Exception address abc thread 42"}\n' * 2
            + "Original PC 0xabc SP 0xdef FP 0x123\nTest minidump captured 1 error 0\n",
            valid + "Test minidump captured 1 error 00\n",
        )
        for stderr in cases:
            with self.subTest(stderr=stderr), tempfile.TemporaryDirectory() as directory:
                output = pathlib.Path(directory)
                self.write_extended_context_dump(output / "synthetic.dmp", "x64")
                result = subprocess.CompletedProcess([], 67, "", "Test minidump collector ready\n" + stderr)
                with mock.patch("windows_crash_trace_smoke.subprocess.run", return_value=result):
                    with self.assertRaises(ValueError):
                        crash_smoke.run_smoke_case(output / "helper.exe", output, "enabled", "x64")

    def test_opaque_diagnostic_marker_strings_do_not_change_valid_capture_outcome(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory)
            self.write_extended_context_dump(output / "synthetic.dmp", "x64")
            stderr = "Exception address abc thread 42\nOriginal PC 0xabc SP 0xdef FP 0x123\n" * 2
            stderr += "Test minidump collector ready\nTest minidump captured 1 error 0\n"
            stderr += 'Test minidump diagnostic {"path":"Exception address Test minidump captured "}\n'
            result = subprocess.CompletedProcess([], 67, "", stderr)
            with mock.patch("windows_crash_trace_smoke.subprocess.run", return_value=result):
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

    def retained_summary(self, output, parent, error=ValueError("DumpHResult 0x8007012b")):
        crash_smoke.retain_failed_smoke(output, str(parent), error)
        retained = list(parent.glob("windows-crash-smoke-*"))
        self.assertEqual(len(retained), 1)
        summary = retained[0] / "failure-summary.json"
        self.assertTrue(summary.is_file(), "failed smoke needs bounded size/hash/provenance metadata")
        self.assertLessEqual(summary.stat().st_size, crash_smoke.MAX_EVIDENCE_SUMMARY_BYTES)
        return retained[0], json.loads(summary.read_bytes())

    def test_failed_smoke_summary_hashes_zero_and_partial_output_without_decoding(self):
        for content in (b"", b"MDMP partial bytes"):
            with self.subTest(size=len(content)), tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                output, parent = root / "enabled", root / "evidence"
                output.mkdir()
                parent.mkdir()
                (output / "synthetic.dmp").write_bytes(content)
                (output / "helper-stderr.txt").write_text("Test minidump captured 0 error 299\n")
                with mock.patch.dict(os.environ, {"GITHUB_SHA": "a" * 40,
                                                 "GITHUB_RUN_ID": "38338818563",
                                                 "GITHUB_RUN_ATTEMPT": "1"}, clear=True), \
                        mock.patch("windows_crash_trace_smoke.exception_from_dump",
                                   side_effect=AssertionError("failed evidence must never be decoded")):
                    retained, record = self.retained_summary(output, parent)
                self.assertEqual(record["schema_version"], 1)
                self.assertEqual(record["case"], "enabled")
                self.assertIn("0x8007012b", record["failure"])
                self.assertEqual(record["provenance"]["github_environment"], {
                    "GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": "38338818563",
                    "GITHUB_RUN_ATTEMPT": "1"})
                dump = record["dumps"][0]
                self.assertEqual(dump["name"], "synthetic.dmp")
                self.assertEqual(dump["observed_size_bytes"], len(content))
                self.assertEqual(dump["retained_size_bytes"], len(content))
                self.assertEqual(dump["sha256"], hashlib.sha256(content).hexdigest())
                self.assertEqual(dump["hash_scope"], "retained-bytes")
                self.assertEqual((retained / dump["name"]).read_bytes(), content)

    def test_retained_diagnostic_is_opaque_and_never_substitutes_for_capture_success(self):
        diagnostic = ("Test minidump diagnostic " + json.dumps({
            "kind": "module", "process": "target", "name": "dbghelp.dll",
            "path": "$(must-not-execute)", "status": "loaded"}) + "\n")
        for content, succeeded in ((b"", False), (b"", True), (b"MDMP partial", True)):
            with self.subTest(size=len(content), reported_success=succeeded), \
                    tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                helper, parent = root / "helper.exe", root / "evidence"
                self.write_pe(helper)
                parent.mkdir()

                def capture(command, **kwargs):
                    output = pathlib.Path(kwargs["cwd"])
                    (output / "synthetic.dmp").write_bytes(content)
                    stderr = "Exception address abc thread 42\nOriginal PC 0xabc SP 0xdef FP 0x123\n" * 2
                    stderr += "Test minidump collector ready\n" + diagnostic
                    stderr += "Test minidump captured " + ("1 error 0\n" if succeeded else "0 error 299\n")
                    return subprocess.CompletedProcess(command, 67, "", stderr)

                with mock.patch.dict(os.environ, {"KLOGG_TEST_MINIDUMP_DIR": str(parent)}, clear=True), \
                        mock.patch("windows_crash_trace_smoke.subprocess.run", side_effect=capture):
                    with self.assertRaises(ValueError):
                        run_smoke(helper)
                retained = list(parent.glob("windows-crash-smoke-*"))
                self.assertEqual(len(retained), 1)
                record = json.loads((retained[0] / "failure-summary.json").read_bytes())
                self.assertEqual(record["diagnostics"], [diagnostic.rstrip("\n")])
                self.assertEqual(record["provenance"]["helper"]["sha256"],
                                 hashlib.sha256(helper.read_bytes()).hexdigest())
                self.assertEqual(record["provenance"]["helper"]["architecture"], "x64")
                self.assertEqual(record["dumps"][0]["observed_size_bytes"], len(content))

    def test_retention_bounds_reads_total_dump_bytes_text_and_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            output, parent = root / "enabled", root / "evidence"
            output.mkdir()
            parent.mkdir()
            (output / "first.dmp").write_bytes(b"a" * 7)
            (output / "second.dmp").write_bytes(b"b" * 7)
            (output / "third.dmp").write_bytes(b"c" * 7)
            diagnostic = "Test minidump diagnostic " + "\x00" * 100000
            (output / "helper-stderr.txt").write_text(diagnostic)
            original_open = pathlib.Path.open
            reads = []

            def bounded_open(path, *args, **kwargs):
                stream = original_open(path, *args, **kwargs)
                if args and args[0] == "rb" and path.parent == output:
                    original_read = stream.read

                    def bounded_read(size=-1):
                        self.assertGreaterEqual(size, 0, "evidence reads must be bounded before allocation")
                        data = original_read(size)
                        reads.append((path.name, size, len(data)))
                        return data

                    stream.read = bounded_read
                return stream

            with mock.patch.object(crash_smoke, "MAX_DUMP_BYTES", 10), \
                    mock.patch.object(pathlib.Path, "open", bounded_open):
                retained, record = self.retained_summary(output, parent, ValueError("\x00" * 100000))
            dump_reads = [entry for entry in reads if entry[0].endswith(".dmp")]
            self.assertLessEqual(sum(size for _, size, _ in dump_reads), 10)
            self.assertLessEqual(sum(p.stat().st_size for p in retained.glob("*.dmp")), 10)
            self.assertEqual(len(record["dumps"]), 2)
            self.assertEqual(sum(dump["retained_size_bytes"] or 0 for dump in record["dumps"]), 7)
            skipped = next(dump for dump in record["dumps"] if dump["sha256"] is None)
            self.assertEqual(skipped["status"], "over-budget")
            self.assertIsNone(skipped["hash_scope"])
            self.assertLessEqual((retained / "helper-stderr.txt").stat().st_size,
                                 crash_smoke.MAX_EVIDENCE_TEXT_BYTES)
            self.assertLessEqual(sum(len(line.encode("utf-8")) for line in record["diagnostics"]),
                                 crash_smoke.MAX_DIAGNOSTIC_TEXT_BYTES)
            self.assertTrue(record["diagnostics_truncated"])

    def test_retention_rejects_external_symlink_dump_and_stderr(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            output, parent = root / "enabled", root / "evidence"
            output.mkdir()
            parent.mkdir()
            external = root / "secret"
            external.write_text("not owned smoke evidence")
            (output / "external.dmp").symlink_to(external)
            (output / "helper-stderr.txt").symlink_to(external)
            retained, record = self.retained_summary(output, parent)
            self.assertFalse((retained / "external.dmp").exists())
            self.assertFalse((retained / "helper-stderr.txt").exists())
            self.assertEqual(record["dumps"][0]["status"], "not-regular")
            self.assertIsNone(record["dumps"][0]["sha256"])

    def test_dump_decoder_bounds_the_actual_read_and_rejects_oversize_before_open(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "synthetic.dmp"
            self.write_extended_context_dump(path, "x64")
            original_open = pathlib.Path.open
            sizes = []

            def bounded_open(opened_path, *args, **kwargs):
                stream = original_open(opened_path, *args, **kwargs)
                original_read = stream.read

                def bounded_read(size=-1):
                    self.assertGreaterEqual(size, 0)
                    self.assertLessEqual(size, crash_smoke.MAX_DUMP_BYTES)
                    sizes.append(size)
                    return original_read(size)

                stream.read = bounded_read
                return stream

            with mock.patch.object(pathlib.Path, "open", bounded_open):
                self.assertEqual(exception_from_dump(path, "x64")["pc"], 0xABC)
            self.assertEqual(sizes, [path.stat().st_size])
            with mock.patch.object(pathlib.Path, "stat", return_value=mock.Mock(
                    st_size=crash_smoke.MAX_DUMP_BYTES + 1)), \
                    mock.patch.object(pathlib.Path, "open", side_effect=AssertionError("oversize dump read")):
                with self.assertRaisesRegex(ValueError, "exceeds smoke-test budget"):
                    exception_from_dump(path, "x64")

    def test_empty_dump_is_rejected_before_any_decode_or_read(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "empty.dmp"
            path.write_bytes(b"")
            with mock.patch.object(pathlib.Path, "open", side_effect=AssertionError("zero-byte dump read")), \
                    mock.patch("windows_crash_trace_smoke.struct.unpack_from",
                               side_effect=AssertionError("zero-byte dump parsed")):
                with self.assertRaisesRegex(ValueError, "header missing"):
                    exception_from_dump(path, "x64")

    def test_windows_dump_probe_runs_in_disposable_process(self):
        helpers = (ROOT / "tests/helpers/CMakeLists.txt").read_text()
        tests = (ROOT / "tests/CMakeLists.txt").read_text()
        self.assertIn("add_executable(crash_trace_test_helper", helpers)
        self.assertIn("NAME windows_crash_trace_smoke", tests)
        self.assertIn("windows_crash_trace_smoke.py", tests)
        self.assertIn("$<TARGET_FILE:crash_trace_test_helper>", tests)

    def windows_identity_script(self):
        source = (ROOT / ".github/workflows/ci-build.yml").read_text()
        step = source.split("      - name: Collect Windows diagnostics on test failure", 1)[1].split(
            "      - name: Upload Windows diagnostics artifact", 1)[0]
        self.assertIn("if: ${{ always() && steps.run-tests.outcome == 'failure' }}", step)
        match = re.search(r"^          \$identityScript = @'\n(.*?)^          '@$", step,
                          re.MULTILINE | re.DOTALL)
        self.assertIsNotNone(match, "bounded identity collector must stay in the failure-only step")
        return step, textwrap.dedent(match[1])

    def identity_ninja_rules(self, text):
        # Execute only the checked-out workflow's bounded, pure rule selection;
        # fixture metadata is data, never Python or a build command.
        _, script = self.windows_identity_script()
        tree = ast.parse(script)
        selection = next(node for node in ast.walk(tree) if isinstance(node, ast.Try)
                         and any(isinstance(item, ast.Assign)
                                 and any(isinstance(target, ast.Name) and target.id == "rules"
                                         for target in item.targets) for item in node.body))
        start = next(index for index, node in enumerate(selection.body)
                     if isinstance(node, ast.Assign)
                     and any(isinstance(target, ast.Name) and target.id == "rules_text"
                             for target in node.targets))
        end = next(index for index, node in enumerate(selection.body)
                   if isinstance(node, ast.Assign)
                   and any(isinstance(target, ast.Name) and target.id == "commands"
                           for target in node.targets))
        statements = selection.body[start + 1:end]
        namespace = {"re": re, "rules_text": text}
        exec(compile(ast.Module(body=statements, type_ignores=[]), "workflow-rule-selection", "exec"),
             namespace)
        return namespace["rules"]

    def test_identity_rules_accept_real_cmake_ninja_windows_text_mode(self):
        with tempfile.TemporaryDirectory(prefix="crash-rule-provenance-") as directory:
            root = pathlib.Path(directory)
            (root / "main.cpp").write_text("int main() { return 0; }\n")
            (root / "trace.cpp").write_text("int trace() { return 0; }\n")
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.14)\n"
                "project(crash_rule_fixture LANGUAGES CXX)\n"
                "add_library(test_crash_trace STATIC trace.cpp)\n"
                "add_executable(crash_trace_test_helper main.cpp)\n"
                "add_executable(crash_trace_collector main.cpp)\n"
                "target_link_libraries(crash_trace_test_helper PRIVATE test_crash_trace)\n"
            )
            configured = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(root / "build"), "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=RelWithDebInfo"],
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(configured.returncode, 0, configured.stdout + configured.stderr)
            # CMake's Ninja generator uses cmGeneratedFileStream text mode.
            # Model its Windows CRLF bytes without pretending this is a native
            # Windows compiler/minidump execution on a non-Windows host.
            generated = (root / "build/CMakeFiles/rules.ninja").read_bytes()
            lf = generated.replace(b"\r\n", b"\n")
            expected = self.identity_ninja_rules(lf.decode("utf-8"))
            self.assertEqual(len(expected), 6)
            windows_rules = lf.replace(b"\n", b"\r\n")
            self.assertEqual(self.identity_ninja_rules(windows_rules.decode("utf-8")), expected)
            (root / "build/CMakeFiles/rules.ninja").write_bytes(windows_rules)
            generated_commands = subprocess.run(
                ["ninja", "-t", "compdb", "-x", *expected], cwd=root / "build",
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(generated_commands.returncode, 0,
                             generated_commands.stdout + generated_commands.stderr)
            commands = json.loads(generated_commands.stdout)
            self.assertEqual(len(commands), 6)
            self.assertTrue(all(all(entry.get(key) for key in ("directory", "command", "file", "output"))
                                for entry in commands))
            for mutation in ("# " + lf.decode("utf-8").replace("\n", "\n# "),
                             lf.decode("utf-8").replace("CXX_COMPILER__crash_trace_collector_",
                                                        "CXX_COMPILER__unrelated_")):
                with self.subTest(mutation=mutation[:80]), self.assertRaises(ValueError):
                    self.identity_ninja_rules(mutation)

    def retained_context_facts(self, records):
        _, script = self.windows_identity_script()
        functions = [node for node in ast.parse(script).body
                     if isinstance(node, ast.FunctionDef) and node.name == "retained_context_facts"]
        self.assertEqual(len(functions), 1, "context facts must derive only from retained diagnostics")
        namespace = {"json": json}
        exec(compile(ast.Module(body=functions, type_ignores=[]), "retained-context-facts", "exec"),
             namespace)
        text = "\n".join(crash_smoke.DIAGNOSTIC_PREFIX + json.dumps(record) for record in records)
        return namespace["retained_context_facts"](text)

    def test_retained_context_facts_report_raw_flags_without_claiming_xstate_validity(self):
        records = [
            {"kind": "input", "pointerBits": 64, "contextFlags": 0x10004f,
             "contextRecord": 0x49f1cfed00},
            {"kind": "read", "object": 3, "requested": 1232,
             "transferred": 1232, "status": "complete", "error": 0},
            {"kind": "summary", "contextFlags": 0x10004f},
        ]
        facts = self.retained_context_facts(records)
        self.assertEqual(facts["status"], "observed-retained-data")
        self.assertEqual(facts["input_context_flags"], 0x10004f)
        self.assertEqual(facts["input_context_flags_hex"], "0x0010004f")
        self.assertTrue(facts["input_xstate_flag_set"])
        self.assertEqual(facts["postfailure_context_flags"], 0x10004f)
        self.assertEqual(facts["context_address_modulo_64"], 0)
        self.assertEqual(facts["context_read_phase"], "postfailure")
        self.assertEqual(facts["context_read_requested_bytes"], 1232)
        self.assertEqual(facts["context_read_transferred_bytes"], 1232)
        self.assertEqual(facts["xstate_extent"], "not-observed")
        self.assertEqual(facts["xstate_validity"], "unknown")
        self.assertEqual(facts["thread_inventory"], "not-observed")
        self.assertEqual(facts["dump_api_owner"], "unavailable-basename-observations-only")
        for mutation in (records + [dict(records[0])],
                         [dict(records[0], contextFlags="0x10004f"), *records[1:]],
                         [dict(records[0], contextRecord=1 << 65), *records[1:]],
                         [records[0], dict(records[1], requested=8192, transferred=8192), records[2]]):
            with self.subTest(mutation=mutation):
                self.assertEqual(self.retained_context_facts(mutation)["status"], "unavailable")

    def test_retained_context_facts_keep_partial_reads_and_flag_differences_visible(self):
        facts = self.retained_context_facts([
            {"kind": "input", "pointerBits": 32, "contextFlags": 0x10007,
             "contextRecord": 0x1000},
            {"kind": "read", "object": 3, "requested": 716,
             "transferred": 128, "status": "partial", "error": 299},
            {"kind": "summary", "contextFlags": 0},
        ])
        self.assertEqual(facts["status"], "observed-retained-data")
        self.assertFalse(facts["input_xstate_flag_set"])
        self.assertEqual(facts["postfailure_context_flags"], 0)
        self.assertEqual(facts["context_read_transferred_bytes"], 128)
        self.assertEqual(facts["context_read_statuses"], ["partial"])
        self.assertEqual(facts["context_read_errors"], [299])
        self.assertEqual(facts["xstate_validity"], "unknown")

    def assert_workflow_identity_contract(self, script):
        try:
            tree = ast.parse(script)
        except SyntaxError as error:
            self.fail("identity metadata script is not valid Python: " + str(error))
        nodes = list(ast.walk(tree))
        expected_ast = {}

        def same_ast(node, expression):
            if expression not in expected_ast:
                expected_ast[expression] = ast.dump(ast.parse(expression, mode="eval").body,
                                                    include_attributes=False)
            return ast.dump(node, include_attributes=False) == expected_ast[expression]

        assignments = {node.targets[0].id: node.value for node in tree.body
                       if isinstance(node, ast.Assign) and len(node.targets) == 1
                       and isinstance(node.targets[0], ast.Name)}
        limits = {"MAX_TEXT_BYTES": "64 * 1024", "MAX_COMMAND_BYTES": "128 * 1024",
                  "MAX_FILE_BYTES": "64 * 1024 * 1024", "MAX_MODULE_BYTES": "256 * 1024 * 1024",
                  "MAX_MODULE_RECORDS": "6", "MAX_MODULE_PATH_UNITS": "512"}
        for name, expression in limits.items():
            self.assertIn(name, assignments)
            self.assertTrue(same_ast(assignments[name], expression), "changed evidence limit " + name)
        for expression in ('len(evidence["modules"]) >= MAX_MODULE_RECORDS',
                           'len(path.encode("utf-16-le")) // 2 > MAX_MODULE_PATH_UNITS',
                           'len(payload) > 256 * 1024'):
            self.assertTrue(any(same_ast(node, expression) for node in nodes
                                if isinstance(node, ast.Compare)),
                            "missing executable bound " + expression)
        calls = [node for node in nodes if isinstance(node, ast.Call)]
        forbidden = {"exec", "eval", "os.system", "os.popen", "os.startfile",
                     "runpy.run_path", "ctypes.CDLL", "ctypes.WinDLL", "ctypes.OleDLL"}
        for expression in forbidden:
            self.assertFalse(any(same_ast(node.func, expression) for node in calls),
                             "metadata is never executable: " + expression)
        subprocess_calls = [node for node in calls if isinstance(node.func, ast.Attribute)
                            and isinstance(node.func.value, ast.Name)
                            and node.func.value.id == "subprocess"]
        self.assertEqual(len(subprocess_calls), 1)
        self.assertTrue(same_ast(subprocess_calls[0].func, "subprocess.Popen"))
        metadata_calls = [node for node in calls if same_ast(node.func, "read_only_command")]
        self.assertEqual(len(metadata_calls), 2)
        for arguments in ('["git", "rev-parse", "HEAD"]',
                          '[selected["CMAKE_MAKE_PROGRAM"], "-t", "compdb", "-x", *rules]'):
            self.assertTrue(any(same_ast(node.args[0], arguments) for node in metadata_calls),
                            "only fixed Git/Ninja metadata commands may execute")
        context_calls = [node for node in calls if same_ast(node.func, "retained_context_facts")]
        self.assertEqual(len(context_calls), 1)
        self.assertTrue(same_ast(context_calls[0], "retained_context_facts(text)"))
        context_appends = [node for node in calls
                           if same_ast(node.func, 'evidence["retained_context_facts"].append')]
        self.assertEqual(len(context_appends), 1)
        self.assertIn(context_calls[0], list(ast.walk(context_appends[0])))
        copies = [node for node in calls if same_ast(node.func, "shutil.copyfile")]
        self.assertEqual(len(copies), 1, "loaded runtime DLLs must never be copied")
        self.assertTrue(same_ast(copies[0].args[0], "path"))
        self.assertTrue(same_ast(copies[0].args[1], "destination / name"))
        project_files = ('("crash_trace_test_helper.exe", "crash_trace_collector.exe", '
                         '"crash_trace_test_helper.pdb", "crash_trace_collector.pdb")')
        self.assertTrue(any(isinstance(node, ast.For) and isinstance(node.target, ast.Name)
                            and node.target.id == "name" and same_ast(node.iter, project_files)
                            for node in nodes))
        strings = {node.value for node in nodes if isinstance(node, ast.Constant)
                   and isinstance(node.value, str)}
        for value in ("GITHUB_SHA", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "VCToolsVersion",
                      "WindowsSDKVersion", "ASAN_OPTIONS", "generated-not-executed",
                      "crash-smoke-identity.json", "kind", "module", "process", "name",
                      "path", "status", "target", "collector", crash_smoke.DIAGNOSTIC_PREFIX):
            self.assertIn(value, strings)
        self.assertTrue(any(same_ast(node.func, "pe_identity.architecture_from_pe") for node in calls))

    def test_workflow_identity_guard_uses_python38_ast_api_floor(self):
        _, script = self.windows_identity_script()
        # Model the Python 3.8 API floor on the current interpreter. This is
        # connected guard coverage, not a claim of executing Python 3.8 itself.
        with mock.patch.dict(vars(ast)):
            vars(ast).pop("unparse", None)
            self.assertFalse(hasattr(ast, "unparse"))
            self.assert_workflow_identity_contract(script)

    def test_ci_identity_guard_rejects_unbounded_module_observations_and_execution(self):
        _, script = self.windows_identity_script()
        self.assert_workflow_identity_contract(script)
        self.assert_workflow_identity_contract(script + '\n# subprocess.run(module["path"])\n')
        mutations = (
            script.replace("MAX_MODULE_RECORDS = 6", "MAX_MODULE_RECORDS = 600"),
            script.replace("MAX_MODULE_PATH_UNITS = 512", "MAX_MODULE_PATH_UNITS = 65536"),
            script + '\nsubprocess.run(module["path"])\n',
            script + '\nos.system(module["path"])\n',
            script + '\nshutil.copyfile(module["path"], destination / "runtime.dll")\n',
            script.replace("**retained_context_facts(text)", '**{"status": "unavailable"}')
            + "\n# retained_context_facts(text)\n",
            script.replace(">= MAX_MODULE_RECORDS", ">= 600"),
            script.replace("> MAX_MODULE_PATH_UNITS", "> 65536"),
            script.replace("MAX_MODULE_RECORDS = 6", "MAX_MODULE_RECORDS = 600")
            + "\n# MAX_MODULE_RECORDS = 6\n",
            script + "\nif (\n",
        )
        for index, mutation in enumerate(mutations):
            self.assertNotEqual(mutation, script)
            with self.subTest(mutation=index), self.assertRaises(AssertionError):
                self.assert_workflow_identity_contract(mutation)

    def test_windows_helper_staging_contract_runs_natively_before_build(self):
        source = (ROOT / ".github/workflows/ci-build.yml").read_text()
        windows = source.split("  WindowsPackages:", 1)[1].split("  WindowsX86:", 1)[0]
        marker = "      - name: Verify Windows helper staging\n"
        self.assertEqual(windows.count(marker), 1)
        step = windows.split(marker, 1)[1].split("      - ", 1)[0]
        lines = [line.strip() for line in step.splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
        self.assertEqual(lines, [
            "shell: pwsh",
            "run: |",
            "python -I tests/scripts/test_adb_helper_staging.py AdbHelperStagingScriptTest",
            "exit $LASTEXITCODE",
        ])
        self.assertLess(windows.index(marker), windows.index("      - name: Build Windows application"))
        self.assertLess(windows.index(marker), windows.index("      - name: Verify Windows tests"))
        for job in ("WindowsX86", "WindowsAsan"):
            inherited = re.split(r"\n  \S", source.split("  " + job + ":", 1)[1], maxsplit=1)[0]
            self.assertIn("steps: *windows_steps", inherited)

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
