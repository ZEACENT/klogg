"""Offline CI evidence contracts: no network, builds, or wall-clock budgets."""
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ci_perf_report", ROOT / "scripts/ci_perf_report.py")
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


def stamp(seconds):
    return f"2026-10-17T00:{seconds // 60:02}:{seconds % 60:02}Z"


def job(identifier, start, end, steps=()):
    return {"id": identifier, "name": str(identifier), "head_sha": "a" * 40,
            "run_id": 1, "run_attempt": 1, "conclusion": "success", "status": "completed",
            "started_at": stamp(start), "completed_at": stamp(end), "steps": list(steps)}


def evidence():
    return {"run": {"id": 1, "run_attempt": 1, "head_sha": "a" * 40,
                    "created_at": stamp(0), "run_started_at": stamp(1),
                    "updated_at": stamp(61), "status": "completed", "conclusion": "success"},
            "jobs": [job(1, 3, 13), job(2, 3, 33), job(3, 35, 60)],
            "metrics": [], "errors": []}


def metric(state, duration, run_id):
    return {"build_wall_ms": duration, "object_cache": "measured",
            "cache_deltas": {"direct_cache_hit": 3, "preprocessed_cache_hit": 1, "cache_miss": 1},
            "provenance": {"sha": "a" * 40, "run_id": str(run_id), "run_attempt": "1",
                           "arch": "x64", "config": "Release/asan", "cache_state": state,
                           "environment": "image@sha256:abc", "build_type": "Release",
                           "cmake_options": {"FOO": "ON"}, "compilers": {"CXX": {"id": "Clang", "version": "18"}}}}


def comparable_metrics():
    return [metric("cold", 100, 1)] + [metric("warm", 20, index) for index in (2, 3, 4)]


def metrics_zip(payload, filename="metrics.json"):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        archive.writestr(filename, json.dumps(payload))
    return stream.getvalue()


class ReportTest(unittest.TestCase):
    def test_dag_separates_needs_wait_queue_and_parallel_sum(self):
        result = REPORT.summarize_run(evidence(), {"1": [], "2": [], "3": ["1", "2"]})
        self.assertEqual(result["jobs"][2]["needs_wait_seconds"], 32)
        self.assertEqual(result["initial_queue_seconds"], 1)
        self.assertEqual(result["jobs"][2]["queue_after_ready_seconds"], 2)
        self.assertEqual(result["sum_job_seconds"], 65)
        self.assertEqual(result["critical_path"]["job_ids"], ["2", "3"])
        self.assertEqual(result["critical_path"]["execution_seconds"], 55)
        self.assertEqual(result["observed_wall_seconds"], 60)
        self.assertEqual(result["api_envelope_seconds"], 61)

    def test_unknown_dag_does_not_mislabel_needs_wait_as_queue(self):
        result = REPORT.summarize_run(evidence())
        self.assertIsNone(result["jobs"][2]["needs_wait_seconds"])
        self.assertIsNone(result["jobs"][2]["queue_after_ready_seconds"])
        self.assertIsNone(result["critical_path"])
        self.assertEqual(result["jobs"][2]["pre_start_seconds"], 34)

    def test_phase_mapping_keeps_combined_configure_compile_and_unknowns(self):
        data = evidence()
        data["jobs"][0]["steps"] = [
            {"name": name, "started_at": stamp(start), "completed_at": stamp(end), "conclusion": "success"}
            for name, start, end in [("Run ./.github/actions/agent-build", 3, 8),
                                      ("Run ./.github/actions/agent-run-tests", 8, 10),
                                      ("Upload current-run build metrics", 10, 11),
                                      ("An unfamiliar step", 11, 13)]]
        result = REPORT.summarize_run(data)["jobs"][0]["phases_seconds"]
        self.assertEqual(result["configure_compile"], 5)
        self.assertIsNone(result["configure"])
        self.assertIsNone(result["compile"])
        self.assertEqual(result["test"], 2)
        self.assertEqual(result["upload"], 1)
        self.assertEqual(result["other"], 2)

    def test_cycle_and_future_dependency_are_rejected(self):
        for dag in ({"1": ["3"], "2": [], "3": ["1"]},
                    {"1": ["2"], "2": [], "3": []}):
            with self.subTest(dag=dag), self.assertRaises(ValueError):
                REPORT.summarize_run(evidence(), dag)

    def test_comparable_cold_and_three_warm_use_median_max_and_current_deltas(self):
        metrics = [metric("cold", 100, 1)] + [metric("warm", value, index + 2)
                                                    for index, value in enumerate((80, 20, 50))]
        group = REPORT.compare_metrics(metrics)[0]
        self.assertTrue(group["complete"])
        self.assertEqual(group["warm"], {"samples": 3, "p50_ms": 50, "max_ms": 80})
        self.assertEqual(group["cold"]["samples"], 1)
        self.assertEqual(group["current_run_cache_hit_percent"], [80.0] * 4)

    def test_phase_and_phased_build_samples_never_mix_with_single_build_cohorts(self):
        samples = [metric("cold", 100, 1), metric("warm", 20, 2), metric("warm", 20, 3)]
        samples[1]["measurement_scope"] = "phased-build"
        samples[2]["measurement_scope"] = "phase"
        groups = REPORT.compare_metrics(samples)
        self.assertEqual(len(groups), 3)
        self.assertEqual({group["identity"]["measurement_scope"] for group in groups},
                         {"single-build", "phased-build", "phase"})

    def test_different_native_configure_strategies_do_not_form_one_cohort(self):
        old, stabilized = metric("cold", 100, 1), metric("warm", 20, 2)
        for item in (old, stabilized):
            item["measurement_scope"] = "phased-build"
        stabilized["provenance"]["native_configure_strategy"] = "one-identical-precompile-stabilization-v1"
        self.assertEqual(len(REPORT.compare_metrics([old, stabilized])), 2)

    def test_missing_identity_unknown_cache_and_duplicate_run_never_complete_evidence(self):
        missing = metric("warm", 5, 2)
        missing["provenance"]["compilers"] = {}
        metrics = [metric("cold", 100, 1), missing, metric("warm", 20, 3), metric("warm", 20, 3)]
        groups = REPORT.compare_metrics(metrics)
        self.assertFalse(any(group["complete"] for group in groups))
        self.assertTrue(any(group["issues"] for group in groups))
        changed = metric("warm", 5, 4)
        changed["provenance"]["sha"] = "b" * 40
        self.assertEqual(len(REPORT.compare_metrics([metric("cold", 100, 1), changed])), 2)

    def test_legacy_artifact_reports_current_delta_without_claiming_comparability(self):
        data = evidence()
        data["metrics"] = [{"artifact_id": 4, "artifact_name": "build-metrics-intel",
                            "report": {"build_wall_ms": 100, "object_cache": "measured",
                                       "cache_deltas": {"direct_cache_hit": 0, "preprocessed_cache_hit": 3, "cache_miss": 719}}}]
        result = REPORT.summarize_run(data)["build_metrics"][0]
        self.assertAlmostEqual(result["current_run_cache_hit_percent"], 100 * 3 / 722)
        self.assertEqual(result["artifact_id"], 4)
        self.assertIsNone(result["provenance"])

    def test_missing_or_malformed_cmake_options_never_complete_comparison(self):
        for options in (None, {}, [], "", "-DFOO=ON", False, 1, {"FOO": None}):
            with self.subTest(options=options):
                metrics = comparable_metrics()
                for item in metrics:
                    item["provenance"]["cmake_options"] = options
                group = REPORT.compare_metrics(metrics)[0]
                self.assertFalse(group["complete"])
                self.assertEqual(group["warm"]["samples"], 3)
        metrics = comparable_metrics()
        for item in metrics:
            del item["provenance"]["cmake_options"]
        self.assertFalse(REPORT.compare_metrics(metrics)[0]["complete"])

    def test_measured_comparison_requires_valid_nonzero_request_counters(self):
        valid = metric("warm", 20, 5)["cache_deltas"]
        invalid = [None, [], [1], "", "counters", False, {},
                   {key: 0 for key in valid},
                   {key: value for key, value in valid.items() if key != "cache_miss"}]
        for key in valid:
            invalid.extend({**valid, key: value}
                           for value in (-1, True, False, 1.5, float("nan"), float("inf"), "1"))
        for deltas in invalid:
            with self.subTest(deltas=deltas):
                metrics = comparable_metrics()
                metrics[-1]["cache_deltas"] = deltas
                self.assertIsNone(REPORT.cache_hit_percent(metrics[-1]))
                group = REPORT.compare_metrics(metrics)[0]
                self.assertFalse(group["complete"])
                self.assertIn("cache-hit evidence", " ".join(group["issues"]))
                self.assertEqual(group["warm"]["samples"], 3)
                self.assertEqual(group["current_run_cache_hit_percent"], [80.0] * 3 + [None])
        metrics = comparable_metrics()
        del metrics[-1]["cache_deltas"]
        self.assertFalse(REPORT.compare_metrics(metrics)[0]["complete"])

    def test_cache_rate_bounds_and_disabled_cache_without_counters(self):
        for hits, misses, expected in ((0, 1, 0.0), (1, 0, 100.0)):
            metrics = comparable_metrics()
            for item in metrics:
                item["cache_deltas"] = {"direct_cache_hit": hits, "preprocessed_cache_hit": 0,
                                        "cache_miss": misses}
            group = REPORT.compare_metrics(metrics)[0]
            self.assertTrue(group["complete"])
            self.assertEqual(group["current_run_cache_hit_percent"], [expected] * 4)
        metrics = comparable_metrics()
        for item in metrics:
            item["object_cache"] = "disabled"
            del item["cache_deltas"]
        group = REPORT.compare_metrics(metrics)[0]
        self.assertTrue(group["complete"])
        self.assertEqual(group["current_run_cache_hit_percent"], [None] * 4)

    def test_invalid_duration_cannot_complete_even_with_enough_other_samples(self):
        for duration in (None, True, False, -1, float("nan"), float("inf"), -float("inf"), "20"):
            with self.subTest(duration=duration):
                group = REPORT.compare_metrics(comparable_metrics() + [metric("warm", duration, 5)])[0]
                self.assertFalse(group["complete"])
                self.assertEqual(group["warm"], {"samples": 3, "p50_ms": 20, "max_ms": 20})
        metrics = comparable_metrics()
        metrics[0]["build_wall_ms"] = 0
        metrics[-1]["build_wall_ms"] = 0.5
        self.assertTrue(REPORT.compare_metrics(metrics)[0]["complete"])

    def test_legacy_missing_provenance_remains_readable_and_incomplete(self):
        for provenance in (None, {}):
            with self.subTest(provenance=provenance):
                payload = {"build_wall_ms": 100, "provenance": provenance}
                self.assertEqual(REPORT.read_metrics_zip(metrics_zip(payload), 1, 1), payload)
                self.assertFalse(REPORT.compare_metrics([payload])[0]["complete"])
        payload = {"build_wall_ms": 100}
        self.assertEqual(REPORT.read_metrics_zip(metrics_zip(payload), 1, 1), payload)
        self.assertFalse(REPORT.compare_metrics([payload])[0]["complete"])

    def test_malformed_provenance_in_offline_comparison_is_rejected(self):
        for provenance in ([], [1], "", "bad", False, True, 0, 1):
            payload = metric("warm", 20, 1)
            payload["provenance"] = provenance
            with self.subTest(provenance=provenance), self.assertRaisesRegex(ValueError, "provenance"):
                REPORT.compare_metrics([payload])

    def test_stored_baseline_replays_without_relabeling_legacy_evidence(self):
        directory = ROOT / "docs/ci/evidence/pr79-39027449860"
        result = subprocess.run([sys.executable, "-I", str(ROOT / "scripts/ci_perf_report.py"), "report",
                                 str(directory / "evidence.json"), "--dag", str(directory / "dag.json")],
                                check=True, text=True, capture_output=True)
        report = json.loads(result.stdout)
        expected = json.loads((directory / "report.json").read_text(encoding="utf-8"))
        # Legacy payloads remain single-build evidence; the additive scope field
        # must not change their samples, durations or incomplete-evidence status.
        for group in expected["build_comparisons"]:
            group["identity"]["measurement_scope"] = "single-build"
            group["identity"]["native_configure_strategy"] = None
        for run in expected["runs"]:
            for item in run["build_metrics"]:
                item["measurement_scope"] = "single-build"
        self.assertEqual(report, expected)
        self.assertFalse(any(group["complete"] for group in report["build_comparisons"]))

    def test_failed_jobs_are_reported_not_silently_dropped(self):
        data = evidence()
        data["jobs"][1]["conclusion"] = "failure"
        result = REPORT.summarize_run(data)
        self.assertFalse(result["successful"])
        self.assertEqual(result["jobs"][1]["conclusion"], "failure")


class CollectionTest(unittest.TestCase):
    def test_pagination_and_expected_sha_are_checked(self):
        calls = []
        def api(endpoint):
            calls.append(endpoint)
            if "/jobs?" in endpoint:
                page = int(endpoint.split("page=")[-1])
                return {"total_count": 2, "jobs": [job(page, 3, 13)] if page <= 2 else []}
            if "/artifacts?" in endpoint:
                return {"total_count": 0, "artifacts": []}
            return evidence()["run"]
        with tempfile.TemporaryDirectory() as directory:
            result = REPORT.collect("owner/repo", 1, Path(directory) / "evidence", "a" * 40, api=api)
            self.assertEqual(len(result["jobs"]), 2)
            self.assertTrue(any("page=2" in call for call in calls))
            self.assertTrue((Path(directory) / "evidence" / "run.json").is_file())
            with self.assertRaisesRegex(ValueError, "SHA"):
                REPORT.collect("owner/repo", 1, Path(directory) / "stale", "b" * 40, api=api)

    def test_other_attempt_artifact_names_are_skipped_without_download_or_fake_samples(self):
        artifacts = [{"id": 1, "name": "build-metrics-intel-attempt-1", "expired": True},
                     {"id": 2, "name": "build-metrics-intel-attempt-2",
                      "workflow_run": {"id": 1, "head_sha": "a" * 40}}]
        run = dict(evidence()["run"], run_attempt=2)
        current = metric("warm", 20, 1)
        current["provenance"]["run_attempt"] = "2"
        def api(endpoint):
            if "/jobs?" in endpoint:
                return {"total_count": 1, "jobs": [dict(job(1, 3, 13), run_attempt=2)]}
            if "/artifacts?" in endpoint:
                return {"total_count": len(artifacts), "artifacts": artifacts}
            return run
        def download(command, *, stdout, check):
            self.assertTrue(command[-1].endswith("/2/zip"))
            stdout.write(metrics_zip(current))
            return subprocess.CompletedProcess(command, 0)
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(REPORT.subprocess, "run", side_effect=download) as fetch:
            result = REPORT.collect("owner/repo", 1, Path(directory) / "current", attempt=2, api=api)
            self.assertEqual(result["errors"], [])
            self.assertEqual(len(result["metrics"]), 1)
            self.assertEqual(fetch.call_count, 1)
            artifacts.pop()
            fetch.reset_mock()
            result = REPORT.collect("owner/repo", 1, Path(directory) / "missing", attempt=2, api=api)
            self.assertEqual(result["metrics"], [])
            self.assertEqual(result["errors"], [])
            fetch.assert_not_called()

    def test_current_attempt_name_still_requires_matching_payload_provenance(self):
        artifact = {"id": 2, "name": "build-metrics-intel-attempt-2",
                    "workflow_run": {"id": 1, "head_sha": "a" * 40}}
        def api(endpoint):
            if "/jobs?" in endpoint:
                return {"total_count": 0, "jobs": []}
            if "/artifacts?" in endpoint:
                return {"total_count": 1, "artifacts": [artifact]}
            return dict(evidence()["run"], run_attempt=2)
        def download(command, *, stdout, check):
            stdout.write(metrics_zip(metric("warm", 20, 1)))
            return subprocess.CompletedProcess(command, 0)
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(REPORT.subprocess, "run", side_effect=download):
            result = REPORT.collect("owner/repo", 1, Path(directory) / "current", attempt=2, api=api)
            self.assertEqual(result["metrics"], [])
            self.assertEqual(len(result["errors"]), 1)
            self.assertIn("another run/attempt", result["errors"][0])

    def test_api_failure_is_not_empty_success(self):
        def api(_):
            raise subprocess.CalledProcessError(1, ["gh", "api"])
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(subprocess.CalledProcessError):
                REPORT.collect("owner/repo", 1, Path(directory) / "evidence", api=api)

    def test_malformed_provenance_records_artifact_error_and_keeps_later_metrics(self):
        for provenance in ([], [1], "", "bad", False, True, 0, 1):
            with self.subTest(provenance=provenance):
                invalid = metric("warm", 20, 1)
                invalid["provenance"] = provenance
                valid = metric("cold", 100, 1)
                artifacts = [{"id": identifier, "name": f"build-metrics-{identifier}",
                              "workflow_run": {"id": 1, "head_sha": "a" * 40}}
                             for identifier in (1, 2)]
                def api(endpoint):
                    if "/jobs?" in endpoint:
                        return {"total_count": 1, "jobs": [job(1, 3, 13)]}
                    if "/artifacts?" in endpoint:
                        return {"total_count": 2, "artifacts": artifacts}
                    return evidence()["run"]
                def download(command, *, stdout, check):
                    payload = invalid if command[-1].endswith("/1/zip") else valid
                    stdout.write(metrics_zip(payload))
                    return subprocess.CompletedProcess(command, 0)
                with tempfile.TemporaryDirectory() as directory, mock.patch.object(
                        REPORT.subprocess, "run", side_effect=download):
                    output = Path(directory) / "receipt"
                    result = REPORT.collect("owner/repo", 1, output, api=api)
                    self.assertEqual(len(result["errors"]), 1)
                    self.assertIn("artifact 1", result["errors"][0])
                    self.assertIn("provenance", result["errors"][0])
                    self.assertEqual([item["report"] for item in result["metrics"]], [valid])
                    self.assertEqual(json.loads((output / "evidence.json").read_text()), result)
                    self.assertFalse((output / "metrics-1.json").exists())

    def test_artifact_wall_duration_must_be_finite_nonnegative_number(self):
        for duration in (True, False, -1, float("nan"), float("inf"), -float("inf")):
            with self.subTest(duration=duration), self.assertRaises(ValueError):
                REPORT.read_metrics_zip(metrics_zip(metric("warm", duration, 1)), 1, 1)
        for duration in (0, 0.5):
            payload = metric("warm", duration, 1)
            self.assertEqual(REPORT.read_metrics_zip(metrics_zip(payload), 1, 1), payload)

    def test_artifact_zip_is_read_without_extraction_and_rejects_wrong_run_and_paths(self):
        for filename, payload, valid in (
                ("metrics.json", metric("warm", 5, 1), True),
                ("../metrics.json", metric("warm", 5, 1), False),
                ("metrics.json", metric("warm", 5, 9), False),
                ("metrics.json", {"broken": True}, False)):
            raw = metrics_zip(payload, filename)
            with self.subTest(filename=filename, payload=payload):
                if valid:
                    self.assertEqual(REPORT.read_metrics_zip(raw, 1, 1), payload)
                else:
                    with self.assertRaises(ValueError):
                        REPORT.read_metrics_zip(raw, 1, 1)


if __name__ == "__main__":
    unittest.main()
