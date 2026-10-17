"""Observation wrapper stays outside immutable environment policy inputs."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ci_build_observation", ROOT / "scripts/ci_build_observation.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ObservationTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.build = Path(temporary.name).resolve()
        (self.build / ".ninja_log").write_text("# ninja log v5\n0\t1\t0\tfoo.o\t1\n")
        (self.build / "compile_commands.json").write_text("[]")
        self.output = self.build / "ci-build-metrics-123-2.json"

    def run_build(self, **kwargs):
        ticks = iter((0, 1))
        options = dict(command=["cmake", "--build", str(self.build)], repo_root=self.build,
                       build_root=self.build, output=self.output, object_cache="disabled",
                       clock=lambda: next(ticks), runner=lambda command, **kw: subprocess.CompletedProcess(command, 0, "", ""))
        options.update(kwargs)
        return MODULE.run_build(**options)

    def test_provenance_reads_generated_compiler_identity_without_executing_cmake(self):
        compiler = self.build / "CMakeFiles/3.30/CMakeCXXCompiler.cmake"
        compiler.parent.mkdir(parents=True)
        compiler.write_text('set(CMAKE_CXX_COMPILER_ID "AppleClang")\nset(CMAKE_CXX_COMPILER_VERSION "17.0.0")\n')
        (self.build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\nENABLE_SANITIZER:BOOL=ON\n")
        report = self.run_build(provenance={"sha": "a" * 40, "cache_state": "warm"})
        self.assertEqual(report["provenance"]["compilers"]["CXX"], {"id": "AppleClang", "version": "17.0.0"})
        self.assertEqual(report["provenance"]["build_type"], "Release")
        self.assertEqual(report["provenance"]["cache_state"], "warm")
        self.assertEqual(report["provenance"]["run_id"], "123")

    def test_missing_identity_is_unknown(self):
        report = self.run_build()
        self.assertEqual(report["provenance"]["compilers"], {})
        self.assertEqual(report["provenance"]["cache_state"], "unknown")

    def test_host_binding_preserves_measurement_and_rejects_stale_identity(self):
        report = self.run_build()
        env = {"GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "2",
               "KLOGG_CONFIG_ARCH": "x64", "KLOGG_LABEL": "ubuntu", "KLOGG_CONFIG_PACKAGE_TAG": "vs-gen"}
        bound = MODULE.bind_environment(self.output, "image@sha256:abc", env)
        self.assertEqual(bound["provenance"]["compilers"], report["provenance"]["compilers"])
        self.assertEqual(bound["provenance"]["environment"], "image@sha256:abc")
        self.assertEqual(bound["provenance"]["config"], "ubuntu/vs-gen")
        original = self.output.read_bytes()
        for change in ({"GITHUB_SHA": "b" * 40}, {"GITHUB_RUN_ID": "999"}):
            with self.assertRaisesRegex(MODULE.MetricsError, "mismatch"):
                MODULE.bind_environment(self.output, "image@sha256:abc", dict(env, **change))
            self.assertEqual(self.output.read_bytes(), original)

    def test_host_binding_enriches_legacy_linux_report_from_local_cmake_files(self):
        self.output.write_text(json.dumps({"build_wall_ms": 10, "object_cache": "measured"}))
        (self.build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
        env = {"GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "2",
               "KLOGG_CONFIG_ARCH": "x64", "KLOGG_LABEL": "ubuntu", "KLOGG_CONFIG_PACKAGE_TAG": "vs-gen"}
        report = MODULE.bind_environment(self.output, "image@sha256:abc", env)
        self.assertEqual(report["provenance"]["build_type"], "Release")

    def test_missing_metrics_is_non_gating(self):
        self.assertIsNone(MODULE.bind_environment(self.output, "image@sha256:abc", {}))

    def test_sccache_delta_ignores_overlapping_advanced_counts_and_detects_reset(self):
        for reset in (False, True):
            with self.subTest(reset=reset):
                calls = []
                self.output.unlink(missing_ok=True)
                def runner(command, **kwargs):
                    calls.append(command)
                    if command[0] != "sccache":
                        return subprocess.CompletedProcess(command, 0, "", "")
                    hits = 10 if len(calls) == 1 else 14
                    payload = {"stats": {"compile_requests": 1 if reset and len(calls) > 1 else hits + 20,
                                         "cache_hits": {"counts": {"C/C++": hits}, "adv_counts": {"C/C++": hits}},
                                         "cache_misses": {"counts": {"C/C++": 2}}}}
                    return subprocess.CompletedProcess(command, 0, json.dumps(payload), "")
                report = self.run_build(object_cache="measured", cache_tool="sccache", runner=runner)
                self.assertEqual([command[0] for command in calls], ["sccache", "cmake", "sccache"])
                self.assertEqual(report["cache_tool"], "sccache")
                if reset:
                    self.assertEqual(report["object_cache"], "unmeasured")
                else:
                    self.assertEqual(report["cache_deltas"], {"direct_cache_hit": 4, "preprocessed_cache_hit": 0, "cache_miss": 0})

    def test_build_failure_remains_authoritative(self):
        with self.assertRaises(subprocess.CalledProcessError) as raised:
            self.run_build(runner=lambda command, **kw: subprocess.CompletedProcess(command, 42, "", ""))
        self.assertEqual(raised.exception.returncode, 42)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
