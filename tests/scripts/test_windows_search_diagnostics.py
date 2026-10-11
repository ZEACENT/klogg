"""Windows search failures retain diagnostics without altering test outcomes."""

import os
import pathlib
import re
import struct
import subprocess
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
