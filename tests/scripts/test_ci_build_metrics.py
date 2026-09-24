import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "ci_build_metrics.py"
SPEC = importlib.util.spec_from_file_location("ci_build_metrics", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def record(start, end, output, command="abc", mtime=0):
    return f"{start}\t{end}\t{mtime}\t{output}\t{command}\n"


def entry(source, output, directory="/repo/build", **extra):
    return {"directory": directory, "file": source, "output": output, **extra}


class CiBuildMetricsTest(unittest.TestCase):
    def invoke(self, records, database, *, repo="/repo", build="/repo/build", extra=(), header="# ninja log v5\n"):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            log = root / ".ninja_log"
            commands = root / "compile_commands.json"
            log.write_text(header + records)
            commands.write_text(json.dumps(database))
            return subprocess.run(
                [sys.executable, str(SCRIPT), "--ninja-log", str(log),
                 "--compile-commands", str(commands), "--repo-root", repo,
                 "--build-root", build, *extra],
                check=False, capture_output=True, text=True,
            )

    def report(self, records, database, **options):
        result = self.invoke(records, database, **options)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        return json.loads(result.stdout)

    def test_parallel_work_is_not_wall_or_union_time(self):
        report = self.report(
            record(0, 100, "a.o", "1") + record(20, 120, "b.o", "2")
            + record(200, 210, "c.o", "3"),
            [entry("/repo/src/a.cpp", "a.o"), entry("/repo/3rdparty/b.c", "b.o"),
             entry("/elsewhere/c.cpp", "c.o")],
            extra=("--build-wall-ms", "300"),
        )
        self.assertEqual(report.get("compilation"), {
            "edges": 3, "parallel_work_ms": 210, "union_active_ms": 130,
            "max_ms": 100, "median_ms": 100, "p95_ms": 100,
        })
        self.assertEqual(report["build_wall_ms"], 300)
        self.assertEqual(report["categories"]["first_party"]["edges"], 1)
        self.assertEqual(report["categories"]["third_party"]["edges"], 1)
        self.assertEqual(report["unknown_outputs"], ["/repo/build/c.o"])

    def test_empty_log_has_no_compilation_or_invented_wall_time(self):
        report = self.report("", [])
        self.assertEqual(report.get("compilation"), {
            "edges": 0, "parallel_work_ms": 0, "union_active_ms": 0,
            "max_ms": None, "median_ms": None, "p95_ms": None,
        })
        self.assertIsNone(report["build_wall_ms"])
        self.assertEqual(report["object_cache"], "unmeasured")

    def test_source_ownership_and_target_owned_qt_generated_units(self):
        sources = [
            ("/repo/src/main.cpp", "CMakeFiles/app.dir/main.o", "first_party"),
            ("/repo/tests/test.cpp", "CMakeFiles/test.dir/test.o", "first_party"),
            ("/repo/benchmarks/bench.cpp", "bench.o", "first_party"),
            ("/repo/build/app_autogen/mocs_compilation.cpp", "CMakeFiles/app.dir/moc.o", "first_party"),
            ("/repo/build/app_autogen/hash/qrc_icons.cpp", "CMakeFiles/app.dir/rcc.o", "first_party"),
            ("/repo/build/app_autogen/ui_window.cpp", "CMakeFiles/app.dir/uic.o", "first_party"),
            ("/repo/build/other_autogen/mocs_compilation.cpp", "CMakeFiles/other.dir/moc.o", "unknown"),
            ("/repo/build/arbitrary.cpp", "CMakeFiles/app.dir/generated.o", "unknown"),
            ("/repo/src/3rdparty/vendor.cpp", "vendor.o", "third_party"),
            ("/cache/cpm_cache/lib/a.cpp", "cpm.o", "third_party"),
            ("/repo/build/_deps/lib-src/b.c", "deps.o", "third_party"),
            ("/repo/srcish/not_ours.cpp", "outside.o", "unknown"),
        ]
        report = self.report(
            "".join(record(i, i + 1, output, f"{i:x}") for i, (_, output, _) in enumerate(sources)),
            [entry(source, output) for source, output, _ in reversed(sources)],
        )
        for category in ("first_party", "third_party", "unknown"):
            self.assertEqual(report["categories"][category]["edges"],
                             sum(owner == category for _, _, owner in sources))

    def test_conflicting_target_ownership_and_same_target_name_stay_unknown(self):
        database = [entry("/repo/src/a.cpp", "one/CMakeFiles/app.dir/a.o"),
                    entry("/repo/3rdparty/b.cpp", "one/CMakeFiles/app.dir/b.o"),
                    entry("/repo/build/app_autogen/mocs_compilation.cpp", "one/CMakeFiles/app.dir/moc.o"),
                    entry("/repo/build/app_autogen/mocs_compilation.cpp", "two/CMakeFiles/app.dir/moc.o")]
        report = self.report(record(0, 10, "one/CMakeFiles/app.dir/moc.o", "1")
                             + record(0, 10, "two/CMakeFiles/app.dir/moc.o", "2"), database)
        self.assertEqual(report["categories"]["unknown"]["edges"], 2)

    def test_multioutput_edges_deduplicate_only_shared_execution_identity(self):
        report = self.report(
            record(0, 10, "a.o", "1") + record(0, 10, "a.pdb", "1", mtime=123)
            + record(0, 10, "b.o", "2") + record(0, 10, "c.o", "3")
            + record(20, 25, "a.o", "1"),
            [entry("/repo/src/a.cpp", "a.o"), entry("/repo/src/b.cpp", "b.o"),
             entry("/repo/src/c.cpp", "c.o")],
        )
        self.assertEqual(report["compilation"]["edges"], 4)
        self.assertEqual(report["compilation"]["parallel_work_ms"], 35)
        self.assertEqual(report["log_records"], 5)
        self.assertEqual(report["unmatched_outputs"], [])

    def test_repeated_output_is_not_collapsed_even_with_same_timing_and_hash(self):
        report = self.report(record(0, 0, "a.o") * 2, [entry("/repo/src/a.cpp", "a.o")])
        self.assertEqual(report["compilation"]["edges"], 2)

    def test_unknown_object_outputs_and_noncompile_edges_are_separate(self):
        report = self.report(record(0, 5, "missing.obj", "1") + record(5, 100, "app", "2"), [])
        self.assertEqual(report["compilation"]["edges"], 1)
        self.assertEqual(report["unknown_outputs"], ["/repo/build/missing.obj"])
        self.assertEqual(report["unmatched_outputs"], ["/repo/build/app"])
        self.assertEqual(report["unmatched_edges"], 1)

    def test_command_output_fallback_handles_posix_spaces_and_arguments(self):
        database = [
            {"directory": "/repo/build", "file": "../src/a.cpp", "command": "c++ -c '../src/a.cpp' -o 'a space.o'"},
            {"directory": "/repo/build", "file": "../tests/b.cpp", "arguments": ["c++", "-c", "../tests/b.cpp", "-ob.o"]},
        ]
        report = self.report(record(0, 1, "a space.o", "1") + record(1, 2, "b.o", "2"), database)
        self.assertEqual(report["categories"]["first_party"]["edges"], 2)

    def test_windows_paths_and_msvc_output_quoting_work_on_any_host(self):
        database = [
            {"directory": "C:\\Repo Name\\build", "file": "..\\src\\main.cpp",
             "command": 'cl.exe /c "..\\src\\main.cpp" /Fo"CMakeFiles\\app.dir\\main space.obj"'},
            entry("C:\\Repo Name\\build\\app_autogen\\mocs_compilation.cpp",
                  "CMakeFiles\\app.dir\\moc.obj", directory="C:\\Repo Name\\build"),
        ]
        report = self.report(record(0, 2, "CMakeFiles/app.dir/main space.obj", "1")
                             + record(2, 3, "CMakeFiles/app.dir/moc.obj", "2"), database,
                             repo="C:\\Repo Name", build="C:\\Repo Name\\build",
                             extra=("--object-cache", "disabled"))
        self.assertEqual(report["categories"]["first_party"]["edges"], 2)
        self.assertEqual(report["object_cache"], "disabled")

    def test_windows_unc_paths_and_separate_fo_argument(self):
        report = self.report(record(0, 1, "a.obj"), [{
            "directory": "\\\\server\\share\\repo\\build", "file": "..\\src\\a.cpp",
            "arguments": ["cl", "/c", "..\\src\\a.cpp", "/Fo", "a.obj"],
        }], repo="\\\\server\\share\\repo", build="\\\\server\\share\\repo\\build")
        self.assertEqual(report["categories"]["first_party"]["edges"], 1)

    def test_conflicting_compile_database_output_is_unknown(self):
        report = self.report(record(0, 1, "a.o"), [entry("/repo/src/a.cpp", "a.o"),
                                                  entry("/repo/3rdparty/b.cpp", "a.o")])
        self.assertEqual(report["categories"]["unknown"]["edges"], 1)

    def test_nearest_rank_percentile_and_median(self):
        report = self.report("".join(record(0, i, f"{i}.o", f"{i:x}") for i in range(1, 21)),
                             [entry(f"/repo/src/{i}.cpp", f"{i}.o") for i in range(1, 21)])
        self.assertEqual(report["compilation"]["p95_ms"], 19)
        self.assertEqual(report["compilation"]["median_ms"], 10.5)
        self.assertEqual(report["compilation"]["union_active_ms"], 20)

    def test_malformed_and_negative_log_records_are_rejected(self):
        for text in ("bad\n", record(-1, 5, "a.o"), record(5, 4, "a.o"),
                     record(0, 1, "a.o", mtime=-1), record(0, 1, ""),
                     record(0, 1, "a.o", command="not-hex"), "0\t1\tx\ta.o\tabc\n"):
            with self.subTest(text=text):
                result = self.invoke(text, [])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("error:", result.stderr)
                self.assertEqual(result.stdout, "")

    def test_rejects_wrong_log_version_and_invalid_database(self):
        for database, header in (([], "# ninja log v4\n"), ({}, "# ninja log v5\n"),
                                 ([None], "# ninja log v5\n"),
                                 ([{"file": "a.cpp", "arguments": "cc -o a.o"}], "# ninja log v5\n")):
            with self.subTest(database=database, header=header):
                result = self.invoke("", database, header=header)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("error:", result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_rejects_negative_or_inconsistent_provided_build_wall_time(self):
        for wall in ("-1", "9"):
            with self.subTest(wall=wall):
                result = self.invoke(record(0, 10, "a.o"), [], extra=("--build-wall-ms", wall))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("error:", result.stderr)


class RunBuildTest(unittest.TestCase):
    """The wrapper measures one unchanged child build; it never alters it."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve()
        self.repo = self.root / "repo"
        self.build = self.root / "build"
        self.build.mkdir(parents=True)
        (self.repo / "src").mkdir(parents=True)
        (self.build / ".ninja_log").write_text(
            "# ninja log v5\n" + record(0, 100, "CMakeFiles/app.dir/main.cpp.o", "1"))
        (self.build / "compile_commands.json").write_text(json.dumps([
            entry(str(self.repo / "src/main.cpp"), "CMakeFiles/app.dir/main.cpp.o",
                  directory=str(self.build))]))
        self.output = self.root / "metrics.json"
        self.calls = []
        self.status = 0

    def stats(self, hits=1, misses=2, stamp=100):
        return ("stats_zeroed_timestamp\t{}\ndirect_cache_hit\t{}\n"
                "preprocessed_cache_hit\t0\ncache_miss\t{}\n".format(stamp, hits, misses))

    def runner(self, command, **kwargs):
        self.calls.append((command, kwargs))
        if command[:2] == ["ccache", "--print-stats"]:
            return subprocess.CompletedProcess(command, 0, self.stats(hits=10 if len(self.calls) == 1 else 11), "")
        self.assertEqual(command, ["cmake", "--build", str(self.build), "--target", "ci_build"])
        return subprocess.CompletedProcess(command, self.status, "build output\n", "")

    def run_build(self, **overrides):
        ticks = iter((0.0, 1.0))
        options = dict(command=["cmake", "--build", str(self.build), "--target", "ci_build"],
                       repo_root=self.repo, build_root=self.build, output=self.output,
                       object_cache="measured", runner=self.runner, clock=lambda: next(ticks))
        options.update(overrides)
        return MODULE.run_build(**options)

    def test_one_child_build_then_fresh_log_report_with_real_wall_and_cache_delta(self):
        report = self.run_build()
        self.assertEqual(report["schema_version"], 1)
        self.assertGreater(report["build_wall_ms"], 0)
        self.assertEqual(report["object_cache"], "measured")
        self.assertEqual(report["cache_deltas"], {"direct_cache_hit": 1, "preprocessed_cache_hit": 0, "cache_miss": 0})
        self.assertEqual([command[0] for command, _ in self.calls],
                         ["ccache", "cmake", "ccache"])
        self.assertTrue(self.output.is_file())
        self.assertEqual(json.loads(self.output.read_text()), report)
        self.assertEqual(report["ninja_log"], ".ninja_log")

    def test_child_failure_propagates_exact_status_without_a_report(self):
        self.status = 42
        with self.assertRaises(subprocess.CalledProcessError) as raised:
            self.run_build()
        self.assertEqual(raised.exception.returncode, 42)
        self.assertFalse(self.output.exists())
        self.assertEqual([command[0] for command, _ in self.calls], ["ccache", "cmake"])

    def test_reset_or_decreasing_cache_counters_fall_back_to_unmeasured(self):
        def reset(command, **kwargs):
            if command[:2] == ["ccache", "--print-stats"]:
                count = reset.count = getattr(reset, "count", 0) + 1
                stamp = 100 if count == 1 else 200
                hits = 10 if count == 1 else 11
                return subprocess.CompletedProcess(command, 0, self.stats(hits=hits, stamp=stamp), "")
            return subprocess.CompletedProcess(command, 0, "", "")
        report = self.run_build(runner=reset)
        self.assertEqual(report["object_cache"], "unmeasured")
        self.assertNotIn("cache_deltas", report)

    def test_missing_cache_tool_or_log_never_fails_a_successful_build(self):
        def without_cache(command, **kwargs):
            if command[0] == "ccache":
                raise FileNotFoundError(command[0])
            return subprocess.CompletedProcess(command, 0, "", "")
        report = self.run_build(runner=without_cache)
        self.assertEqual(report["object_cache"], "unmeasured")
        self.assertTrue(self.output.is_file())
        (self.build / ".ninja_log").unlink()
        with self.assertRaises(MODULE.MetricsError):
            self.run_build(runner=without_cache)

    def test_wall_time_is_monotonic_not_wall_clock(self):
        class Clock:
            def __init__(self):
                self.values = iter((10.0, 11.0))
            def __call__(self):
                return next(self.values)
        self.status = 0
        report = self.run_build(clock=Clock())
        self.assertEqual(report["build_wall_ms"], 1000)

    def test_cli_run_build_uses_child_exit_status(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory).resolve()
            (root / ".ninja_log").write_text("# ninja log v5\n")
            (root / "compile_commands.json").write_text("[]")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "run-build", "--repo-root", str(self.repo),
                 "--build-root", str(root), "--output", str(root / "out.json"),
                 "--object-cache", "disabled", "--", "false"],
                check=False, capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertFalse((root / "out.json").exists())


if __name__ == "__main__":
    unittest.main()
