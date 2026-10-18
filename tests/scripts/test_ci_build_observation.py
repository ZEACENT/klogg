"""Observation wrapper stays outside immutable environment policy inputs."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

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

    def run_build(self, *, environment=None, **kwargs):
        ticks = iter((0, 1))
        options = dict(command=["cmake", "--build", str(self.build)], repo_root=self.build,
                       build_root=self.build, output=self.output, object_cache="disabled",
                       clock=lambda: next(ticks), runner=lambda command, **kw: subprocess.CompletedProcess(command, 0, "", ""))
        options.update(kwargs)
        # Build and bind must use the same test identity, not the runner's SHA.
        with mock.patch.dict(os.environ, environment or {}, clear=True):
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

    def test_restore_identity_does_not_claim_a_warm_cache(self):
        env = {"KLOGG_CI_CACHE_RESTORE_KEY": "intel-qt6-ccache-v2-2026-10-17",
               "CCACHE_COMPILERCHECK": "content"}
        provenance = self.run_build(environment=env)["provenance"]
        self.assertEqual(provenance["cache_restore_key"], env["KLOGG_CI_CACHE_RESTORE_KEY"])
        self.assertEqual(provenance["cache_compiler_check"], "content")
        self.assertEqual(provenance["cache_state"], "unknown")

    def test_verified_artifact_is_effective_transport_not_superseded_legacy_key(self):
        env = {"KLOGG_CI_CACHE_RESTORE_KEY": "legacy-key", "KLOGG_CI_CACHE_ARTIFACT_ID": "456"}
        provenance = self.run_build(environment=env)["provenance"]
        self.assertEqual(provenance["cache_artifact_id"], "456")
        self.assertEqual(provenance["cache_transport"], "verified-artifact")
        self.assertIsNone(provenance["cache_restore_key"])
        self.assertEqual(provenance["superseded_cache_restore_key"], "legacy-key")
        self.assertEqual(provenance["cache_state"], "unknown")
        self.assertEqual(MODULE.build_provenance(self.build, {"KLOGG_CI_CACHE_RESTORE_KEY": "legacy"})["cache_transport"], "actions-cache")
        self.assertEqual(MODULE.build_provenance(self.build, {})["cache_transport"], "none")

    def test_source_root_comes_from_generated_cmake_cache(self):
        (self.build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=/verified/source\n")
        self.assertEqual(MODULE.build_provenance(self.build, {})["source_root"], "/verified/source")

    def test_cli_retains_complete_current_report_in_logs_before_artifact_transport(self):
        report = self.run_build(provenance={"sha": "a" * 40, "config": "escaped\nvalue"})
        for mode, function, arguments in (
            ("run-build", "run_build", ["--repo-root", str(self.build), "--build-root", str(self.build),
                                       "--output", str(self.output), "--", "cmake", "--build", str(self.build)]),
            ("bind-environment", "bind_environment", ["--metrics", str(self.output), "--identity", "digest"]),
        ):
            for value in (report, None):
                with self.subTest(mode=mode, available=value is not None):
                    output = io.StringIO()
                    with mock.patch.object(MODULE, function, return_value=value), contextlib.redirect_stdout(output):
                        self.assertEqual(MODULE.main([mode, *arguments]), 0)
                    lines = output.getvalue().splitlines()
                    if value is None:
                        self.assertEqual(lines, [])
                    else:
                        self.assertEqual(len(lines), 1)
                        self.assertTrue(lines[0].startswith("KLOGG_BUILD_METRICS_JSON="))
                        self.assertEqual(json.loads(lines[0].split("=", 1)[1]), report)

    def test_missing_identity_is_unknown(self):
        report = self.run_build()
        self.assertEqual(report["provenance"]["compilers"], {})
        self.assertEqual(report["provenance"]["cache_state"], "unknown")

    def test_build_fixture_does_not_inherit_runner_provenance(self):
        # Reproduce the Actions environment even when this suite runs locally.
        ambient = {"GITHUB_SHA": "b" * 40, "GITHUB_RUN_ID": "999", "GITHUB_RUN_ATTEMPT": "9",
                   "KLOGG_CONFIG_ARCH": "runner-arch", "KLOGG_LABEL": "runner-label",
                   "KLOGG_CONFIG_PACKAGE_TAG": "runner-package", "KLOGG_CI_CACHE_STATE": "warm",
                   "KLOGG_CI_ENVIRONMENT_IDENTITY": "runner-image", "ImageOS": "runner-os",
                   "ImageVersion": "runner-version"}
        with mock.patch.dict(os.environ, ambient):
            provenance = self.run_build()["provenance"]
        for key in ("sha", "arch"):
            self.assertIsNone(provenance[key])
        self.assertEqual(provenance["run_id"], "123")
        self.assertEqual(provenance["run_attempt"], "2")
        self.assertEqual(provenance["config"], "")
        self.assertEqual(provenance["environment"], "")
        self.assertEqual(provenance["cache_state"], "unknown")

    def test_host_binding_preserves_measurement_and_rejects_stale_identity(self):
        env = {"GITHUB_SHA": "a" * 40, "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "2",
               "KLOGG_CONFIG_ARCH": "x64", "KLOGG_LABEL": "ubuntu", "KLOGG_CONFIG_PACKAGE_TAG": "vs-gen"}
        report = self.run_build(environment=env)
        self.assertEqual(report["provenance"]["sha"], env["GITHUB_SHA"])
        self.assertEqual(report["provenance"]["arch"], env["KLOGG_CONFIG_ARCH"])
        bound = MODULE.bind_environment(self.output, "image@sha256:abc", env)
        self.assertEqual(bound["provenance"]["compilers"], report["provenance"]["compilers"])
        self.assertEqual(bound["provenance"]["environment"], "image@sha256:abc")
        self.assertEqual(bound["provenance"]["config"], "ubuntu/vs-gen")
        original = self.output.read_bytes()
        for change in ({"GITHUB_SHA": "b" * 40}, {"GITHUB_RUN_ID": "999"},
                       {"GITHUB_RUN_ATTEMPT": "3"}, {"KLOGG_CONFIG_ARCH": "arm64"},
                       {"KLOGG_LABEL": "other"}, {"KLOGG_CONFIG_PACKAGE_TAG": "other"}):
            with self.subTest(change=change):
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

    def test_execution_diagnostics_cli_is_explicit_and_default_off(self):
        arguments = ["run-build", "--repo-root", str(self.build), "--build-root", str(self.build),
                     "--output", str(self.output)]
        for mode in (None, "macos-tsan"):
            with self.subTest(mode=mode), mock.patch.object(MODULE, "run_build", return_value=None) as run:
                extra = ["--execution-diagnostics", mode] if mode else []
                self.assertEqual(MODULE.main([*arguments, *extra, "--", "cmake", "--build", str(self.build)]), 0)
                self.assertEqual(run.call_args.kwargs.get("execution_diagnostics"), mode)

    def test_diagnostics_never_replace_the_build_runner_and_stop_on_failure(self):
        for failure in (None, subprocess.CalledProcessError(42, ["cmake"]), KeyboardInterrupt()):
            with self.subTest(failure=type(failure).__name__):
                self.output.unlink(missing_ok=True)
                session = mock.Mock()
                command = ["cmake", "--build", str(self.build), "--parallel", "4"]
                runner = mock.Mock(side_effect=failure) if failure else mock.Mock(
                    return_value=subprocess.CompletedProcess(command, 0, "", ""))
                with mock.patch.object(MODULE, "start_diagnostics", return_value=session, create=True) as start:
                    if failure:
                        with self.assertRaises(type(failure)):
                            self.run_build(command=command, runner=runner, execution_diagnostics="macos-tsan")
                        self.assertFalse(self.output.exists())
                    else:
                        self.run_build(command=command, runner=runner, execution_diagnostics="macos-tsan")
                    start.assert_called_once_with(self.build, "macos-tsan")
                    session.stop.assert_called_once_with()
                    runner.assert_called_once_with(command, check=False)

    def test_diagnostic_start_or_stop_failure_is_nonfatal_and_private(self):
        for stage in ("start", "stop"):
            with self.subTest(stage=stage):
                self.output.unlink(missing_ok=True)
                session = mock.Mock()
                if stage == "stop":
                    session.stop.side_effect = RuntimeError("PRIVATE_TOKEN=secret")
                with mock.patch.object(MODULE, "start_diagnostics", return_value=session,
                                       side_effect=RuntimeError("PRIVATE_TOKEN=secret") if stage == "start" else None,
                                       create=True), contextlib.redirect_stderr(io.StringIO()) as errors:
                    self.assertIsNotNone(self.run_build(execution_diagnostics="macos-tsan"))
                self.assertNotIn("secret", errors.getvalue())

    def test_build_failure_remains_authoritative(self):
        with self.assertRaises(subprocess.CalledProcessError) as raised:
            self.run_build(runner=lambda command, **kw: subprocess.CompletedProcess(command, 42, "", ""))
        self.assertEqual(raised.exception.returncode, 42)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
