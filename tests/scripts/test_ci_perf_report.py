"""Offline CI evidence contracts: no network, builds, or wall-clock budgets."""
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
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
                           "cmake_options": "-DFOO=ON", "compilers": {"CXX": {"id": "Clang", "version": "18"}}}}


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

    def test_api_failure_is_not_empty_success(self):
        def api(_):
            raise subprocess.CalledProcessError(1, ["gh", "api"])
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(subprocess.CalledProcessError):
                REPORT.collect("owner/repo", 1, Path(directory) / "evidence", api=api)

    def test_artifact_zip_is_read_without_extraction_and_rejects_wrong_run_and_paths(self):
        for filename, payload, valid in (
                ("metrics.json", metric("warm", 5, 1), True),
                ("../metrics.json", metric("warm", 5, 1), False),
                ("metrics.json", metric("warm", 5, 9), False),
                ("metrics.json", {"broken": True}, False)):
            stream = io.BytesIO()
            with zipfile.ZipFile(stream, "w") as archive:
                archive.writestr(filename, json.dumps(payload))
            with self.subTest(filename=filename, payload=payload):
                if valid:
                    self.assertEqual(REPORT.read_metrics_zip(stream.getvalue(), 1, 1), payload)
                else:
                    with self.assertRaises(ValueError):
                        REPORT.read_metrics_zip(stream.getvalue(), 1, 1)


if __name__ == "__main__":
    unittest.main()
