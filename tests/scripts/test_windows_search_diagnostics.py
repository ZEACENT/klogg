"""Windows search failures retain diagnostics without altering test outcomes."""

import pathlib
import struct
import tempfile
import unittest

from windows_crash_trace_smoke import exception_from_dump

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
        self.assertIn("ExceptionAddress", handler)
        self.assertIn("ExceptionInformation", handler)
        self.assertIn("MINIDUMP_EXCEPTION_INFORMATION", handler)
        self.assertIn("MiniDumpNormal", handler)
        self.assertNotIn("MiniDumpWithFullMemory", source)
        self.assertIn("InterlockedCompareExchange", handler)
        self.assertIn("InterlockedExchange( &crashTraceActive, 0 )", handler)
        self.assertIn("&dumpCaptureStarted", handler)
        self.assertLess(handler.index("ExceptionAddress"), handler.index("StackWalk64"))
        self.assertLess(handler.index("MiniDumpWriteDump"), handler.index("SymInitialize"))
        self.assertIn("return EXCEPTION_CONTINUE_SEARCH;", handler)
        self.assertIn('L"KLOGG_TEST_MINIDUMP_DIR"', source)
        self.assertIn("CREATE_NEW", source)

    def test_only_unit_and_ui_test_targets_keep_debug_symbols(self):
        for relative, target in (("tests/unit/CMakeLists.txt", "klogg_tests"),
                                 ("tests/ui/CMakeLists.txt", "klogg_itests")):
            with self.subTest(target=target):
                text = (ROOT / relative).read_text()
                self.assertIn(f"klogg_configure_test_target({target} KEEP_DEBUG_SYMBOLS)", text)
        helper = (ROOT / "cmake/TestTargetOptions.cmake").read_text()
        self.assertIn("set(_klogg_debug_option /DEBUG:NONE)", helper)

    def test_dump_decoder_checks_original_exception_and_context_bounds(self):
        for architecture in ("x64", "x86"):
            with self.subTest(architecture=architecture), tempfile.TemporaryDirectory() as directory:
                content = bytearray(1600)
                struct.pack_into("<4sI", content, 0, b"MDMP", 0xA793)
                struct.pack_into("<II", content, 8, 1, 32)
                struct.pack_into("<III", content, 32, 6, 168, 44)
                struct.pack_into("<I4xI", content, 44, 42, 0xC0000005)
                struct.pack_into("<Q", content, 68, 0xABC)
                struct.pack_into("<I", content, 76, 2)
                struct.pack_into("<QQ", content, 84, 0, 0x1234)
                size = 1232 if architecture == "x64" else 716
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
                self.assertEqual(exception_from_dump(dump), {
                    "thread": 42, "code": 0xC0000005, "address": 0xABC,
                    "parameters": 2, "operation": 0, "accessed": 0x1234,
                    "pc": 0xABC, "sp": 0xDEF, "fp": 0xAAA})
                for unsupported_size in (204, 256, 512):
                    struct.pack_into("<II", content, 204, unsupported_size, 220)
                    dump.write_bytes(content)
                    with self.assertRaisesRegex(ValueError, "context"):
                        exception_from_dump(dump)
                struct.pack_into("<II", content, 204, size, 220)
                struct.pack_into("<I", content, 220 + flag_offset, 0x200003)
                dump.write_bytes(content)
                with self.assertRaisesRegex(ValueError, "context"):
                    exception_from_dump(dump)
                struct.pack_into("<II", content, 204, 16, len(content) - 10)
                dump.write_bytes(content)
                with self.assertRaisesRegex(ValueError, "context"):
                    exception_from_dump(dump)

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
