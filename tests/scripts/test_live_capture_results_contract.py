import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).parents[2]
RESULTS_JSON = ROOT / "docs" / "benchmarks" / "live-capture-results.json"
METHODOLOGY_MD = ROOT / "docs" / "benchmarks" / "live-capture-methodology.md"
RESULTS_MD = ROOT / "docs" / "benchmarks" / "live-capture-results.md"
INVENTORY_MD = ROOT / "docs" / "benchmarks" / "live-capture-dependency-inventory.md"

REQUIRED_METRICS = {
    "process_tree_children_started": "count",
    "maximum_live_children": "count",
    "process_cpu_ns": "nanoseconds",
    "child_cpu_ns": "nanoseconds",
    "peak_rss_bytes": "bytes",
    "voluntary_context_switches": "count",
    "involuntary_context_switches": "count",
    "startup_ns": "nanoseconds",
    "first_byte_latency_ns": "nanoseconds",
    "first_commit_latency_ns": "nanoseconds",
    "throughput_payload_bytes_per_second": "bytes_per_second",
    "teardown_ns": "nanoseconds",
    "queue_high_water_bytes": "bytes",
    "queue_high_water_chunks": "count",
    "queue_backpressure_events": "count",
    "queue_dropped_records": "count",
}
UDID_PATTERNS = (
    re.compile(r"\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}\b"),
    re.compile(r"\b[0-9A-Fa-f]{40}\b"),
)


def walk(value):
    if isinstance(value, dict):
        for key, child in value.items():
            yield key
            yield from walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk(child)
    elif isinstance(value, str):
        yield value


class LiveCaptureResultsContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for path in (RESULTS_JSON, METHODOLOGY_MD, RESULTS_MD, INVENTORY_MD):
            if not path.is_file():
                raise AssertionError(f"missing live-capture artifact: {path}")
        cls.document = json.loads(RESULTS_JSON.read_text(encoding="utf-8"))

    def test_document_and_rows_are_versioned_and_matrix_is_complete(self):
        document = self.document
        self.assertEqual(document["schema_version"], 1)
        self.assertEqual(document["methodology_id"], "synthetic-live-capture-v1")
        self.assertRegex(document["generated_utc"], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")

        matrix = document["synthetic_matrix"]
        self.assertEqual(matrix["arms"], ["process", "integrated"])
        self.assertEqual(matrix["scenarios"], ["steady", "reconnect"])
        self.assertEqual(matrix["balanced_order"], ["process", "integrated", "integrated", "process"])
        self.assertEqual(matrix["records_per_trial"], 128)
        rows = matrix["rows"]
        self.assertEqual(len(rows), 8)

        expected = {
            (scenario, trial, arm)
            for scenario in matrix["scenarios"]
            for trial, arm in enumerate(matrix["balanced_order"])
        }
        actual = {(row["scenario"], row["trial"], row["arm"]) for row in rows}
        self.assertEqual(actual, expected)

        for row in rows:
            self.assertEqual(row["row_schema_version"], 1)
            self.assertEqual(row["status"], "ok")
            self.assertIsNone(row["reason_code"])
            self.assertTrue(row["cleanup_verified"])
            self.assertEqual(set(row["metrics"]), set(REQUIRED_METRICS))
            for name, unit in REQUIRED_METRICS.items():
                metric = row["metrics"][name]
                self.assertEqual(metric["unit"], unit)
                self.assertIn(metric["status"], ("measured", "unavailable"))
                self.assertIsInstance(metric["synthetic"], bool)
                if metric["status"] == "measured":
                    self.assertIsInstance(metric["value"], (int, float))
                    self.assertGreaterEqual(metric["value"], 0)
                    self.assertIsNone(metric["reason_code"])
                else:
                    self.assertIsNone(metric["value"])
                    self.assertRegex(metric["reason_code"], r"^[a-z][a-z0-9_]+$")

            correctness = row["correctness"]
            self.assertEqual(correctness["records_expected"], 128)
            self.assertEqual(correctness["records_committed"], 128)
            self.assertEqual(correctness["fixture_crc_match"], True)
            self.assertEqual(correctness["sequence_gap_count"], 0)
            self.assertEqual(correctness["duplicate_count"], 0)
            self.assertEqual(correctness["crc_error_count"], 0)
            self.assertEqual(
                correctness["segment_transitions_observed"],
                correctness["segment_transitions_expected"],
            )

    def test_acceptance_native_stack_has_verified_source_patch_and_symbol_receipts(self):
        stack = self.document["acceptance_native_stack"]
        self.assertEqual(stack["status"], "verified")
        self.assertEqual(stack["architecture"], "x86_64")
        self.assertRegex(stack["lock_sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(
            stack["required_exported_symbols"],
            {
                "lockdownd_client_new_with_existing_pair": True,
                "ostrace_start_activity_with_error": True,
                "syslog_relay_start_capture_raw_with_error": True,
            },
        )
        self.assertTrue(stack["runtime_closure_verified"])
        self.assertTrue(stack["verifier_passed"])
        self.assertGreaterEqual(len(stack["source_archives"]), 7)
        self.assertEqual(len(stack["patches"]), 4)
        for source in stack["source_archives"]:
            self.assertRegex(source["sha256"], r"^[0-9a-f]{64}$")
            self.assertTrue(source["verified"])
        for patch in stack["patches"]:
            self.assertRegex(patch["patch_sha256"], r"^[0-9a-f]{64}$")
            self.assertRegex(patch["clean_tree_sha256"], r"^[0-9a-f]{64}$")
            self.assertRegex(patch["patched_tree_sha256"], r"^[0-9a-f]{64}$")
            self.assertTrue(patch["verified"])
        self.assertEqual(set(stack["dylib_sha256"]), {
            "libcrypto.3.dylib", "libssl.3.dylib", "libcurl.4.dylib",
            "libplist-2.0.dylib", "libtatsu.0.dylib",
            "libimobiledevice-glue-1.0.dylib", "libusbmuxd-2.0.dylib",
            "libimobiledevice-1.0.dylib",
        })
        for digest in stack["dylib_sha256"].values():
            self.assertRegex(digest, r"^[0-9a-f]{64}$")

    def test_microbenchmark_and_real_device_gates_have_accurate_terminal_rows(self):
        protocol = self.document["ios_ostrace_protocol"]
        self.assertEqual(protocol["status"], "ok")
        self.assertGreater(protocol["iterations"], 0)
        for name in ("decode", "format_plain", "format_ansi"):
            self.assertGreater(protocol["metrics"][name]["operations_per_second"], 0)
            self.assertGreater(protocol["metrics"][name]["elapsed_ns"], 0)

        real_device = self.document["real_device_comparison"]
        self.assertIn(real_device["status"], ("ok", "not_run"))
        self.assertEqual(real_device["duration_ms"], 2000)
        self.assertFalse(real_device["ansi_enabled"])
        self.assertEqual(real_device["capture_config_id"], "volatile-capture-v1")
        expected_block = ["native", "baseline", "baseline", "native"]
        if real_device["status"] == "not_run":
            self.assertEqual(real_device["reason_code"], "packaged_native_abi_incomplete")
            self.assertTrue(real_device["message"])
            self.assertEqual(real_device["balanced_order"], expected_block * 3)
            self.assertEqual(real_device["warmup_rows"], [])
            self.assertEqual(real_device["rows"], [])
            self.assertEqual(
                real_device["missing_native_symbols"],
                [
                    "lockdown_existing_pair_client",
                    "ostrace_error_callback_start",
                    "syslog_raw_error_callback_start",
                ],
            )
            self.assertEqual(real_device["baseline_executable_gate"]["status"], "ok")
            self.assertTrue(real_device["baseline_executable_gate"]["canonical"])
            self.assertRegex(real_device["baseline_executable_gate"]["sha256"], r"^[0-9a-f]{64}$")
            catalog = self.document["native_catalog_gate"]
            self.assertEqual(catalog["status"], "ok")
            self.assertEqual(catalog["exact_usb_endpoint_count"], 1)
            self.assertTrue(catalog["existing_pair_record_present"])
            return

        self.assertIsNone(real_device["reason_code"])
        self.assertEqual(real_device["balanced_order"], expected_block * 3)
        self.assertEqual(len(real_device["warmup_rows"]), 2)
        self.assertEqual(len(real_device["rows"]), 12)
        self.assertEqual([row["arm"] for row in real_device["rows"]], expected_block * 3)
        required_real_metrics = {
            "process_tree_children_started",
            "process_cpu_ns",
            "child_cpu_ns",
            "peak_rss_bytes",
            "voluntary_context_switches",
            "involuntary_context_switches",
            "startup_ns",
            "teardown_ns",
            "bytes_received",
            "lines_committed",
            "throughput_bytes_per_second",
            "ansi_escape_count",
            "format_complete",
            "error_count",
            "queue_high_water_bytes",
            "queue_high_water_chunks",
            "queue_backpressure_events",
            "queue_dropped_records",
        }
        for row in real_device["warmup_rows"] + real_device["rows"]:
            self.assertEqual(row["row_schema_version"], 1)
            self.assertEqual(row["status"], "ok")
            self.assertIsNone(row["reason_code"])
            self.assertEqual(row["duration_ms"], 2000)
            self.assertFalse(row["ansi_enabled"])
            self.assertEqual(row["capture_config_id"], "volatile-capture-v1")
            self.assertTrue(row["cleanup_verified"])
            self.assertEqual(set(row["metrics"]), required_real_metrics)
            self.assertGreater(row["metrics"]["bytes_received"]["value"], 0)
            self.assertGreater(row["metrics"]["lines_committed"]["value"], 0)
            self.assertEqual(row["metrics"]["format_complete"]["value"], 1)
            self.assertEqual(row["metrics"]["error_count"]["value"], 0)
            self.assertEqual(row["metrics"]["queue_dropped_records"]["status"], "unavailable")
            self.assertTrue(row["format_complete"])
            self.assertEqual(row["error_count"], 0)
            self.assertNotIn("event_equality", json.dumps(row).lower())
            self.assertNotIn("zero_drop", json.dumps(row).lower())

        catalog = self.document["native_catalog_gate"]
        self.assertIn(catalog["status"], ("ok", "not_run"))
        self.assertIn(catalog["exact_usb_endpoint_count"], (0, 1))
        self.assertNotIn("udid", json.dumps(catalog).lower())

    def test_production_default_ostrace_acceptance_is_exact_and_sanitized(self):
        acceptance = self.document["production_default_ostrace_acceptance"]
        self.assertEqual(acceptance["status"], "ok")
        self.assertIsNone(acceptance["reason_code"])
        self.assertEqual(acceptance["service"], "os_trace")
        self.assertEqual(
            acceptance["pipeline"],
            "IosNativeTransport -> LiveLogController -> AdbLogcatSource -> StreamingLogData -> CaptureStore",
        )
        self.assertEqual(acceptance["capture_config_id"], "volatile-capture-v1")
        self.assertEqual(acceptance["duration_ms"], 2000)
        self.assertFalse(acceptance["ansi_enabled"])
        self.assertTrue(acceptance["sanitized_diagnostics_only"])
        self.assertEqual(acceptance["pre_fix_error_code"], 7)
        self.assertEqual(acceptance["pre_fix_error_class"], "span_out_of_bounds")
        self.assertEqual(acceptance["root_cause"], "activity_body_misrouted_as_log_text_spans")
        self.assertTrue(acceptance["control_plist_boundary_fixed"])
        self.assertEqual(len(acceptance["warmup_rows"]), 2)
        self.assertEqual(len(acceptance["rows"]), 3)
        self.assertEqual([row["trial"] for row in acceptance["warmup_rows"]], [0, 1])
        self.assertEqual([row["trial"] for row in acceptance["rows"]], [0, 1, 2])
        for row in acceptance["warmup_rows"] + acceptance["rows"]:
            self.assertEqual(row["row_schema_version"], 1)
            self.assertEqual(row["status"], "ok")
            self.assertIsNone(row["reason_code"])
            self.assertEqual(row["duration_ms"], 2000)
            self.assertFalse(row["ansi_enabled"])
            self.assertEqual(row["capture_config_id"], "volatile-capture-v1")
            self.assertTrue(row["cleanup_verified"])
            self.assertTrue(row["format_complete"])
            self.assertEqual(row["error_count"], 0)
            metrics = row["metrics"]
            self.assertGreater(metrics["bytes_received"], 0)
            self.assertGreater(metrics["lines_committed"], 0)
            self.assertEqual(metrics["ansi_escape_count"], 0)
            self.assertEqual(metrics["format_complete"], 1)
            self.assertEqual(metrics["error_count"], 0)
            self.assertEqual(metrics["cleanup_verified"], 1)
            self.assertEqual(metrics["process_tree_children_started"], 0)
            self.assertEqual(metrics["maximum_live_children"], 0)
            self.assertEqual(metrics["queue_backpressure_events"], 0)
            self.assertEqual(metrics["queue_dropped_records_available"], 0)

    def test_artifacts_contain_no_raw_content_or_device_identifier(self):
        serialized = json.dumps(self.document, sort_keys=True)
        lowered = serialized.lower()
        for forbidden in (
            "raw_content",
            "raw_records",
            "captured_lines",
            "captured_payload",
            "udid",
        ):
            self.assertNotIn(forbidden, lowered)
        for value in walk(self.document):
            if isinstance(value, str):
                for pattern in UDID_PATTERNS:
                    self.assertIsNone(pattern.search(value))

        for path in (METHODOLOGY_MD, RESULTS_MD, INVENTORY_MD):
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("UDID", text)
            for pattern in UDID_PATTERNS:
                self.assertIsNone(pattern.search(text))

    def test_methodology_and_inventory_pin_safety_and_measurement_definitions(self):
        methodology = METHODOLOGY_MD.read_text(encoding="utf-8")
        for heading in (
            "# Live Capture Benchmark Methodology",
            "## Synthetic Matrix",
            "## Metric Definitions",
            "## Correctness Gates",
            "## Real-Device Safety Gates",
            "## Cleanup and Privacy",
        ):
            self.assertIn(heading, methodology)
        for requirement in (
            "one stream at a time",
            "passive",
            "exactly one USB endpoint",
            "balanced ABBA",
            "no install",
            "no pair or unpair",
            "no raw log retention",
            "CaptureStore",
        ):
            self.assertIn(requirement, methodology)

        inventory = INVENTORY_MD.read_text(encoding="utf-8")
        for heading in (
            "# Live Capture Dependency Inventory",
            "## Packaged Native Stack",
            "## Baseline Tooling",
            "## Benchmark Binaries",
            "## Environment Limitations",
        ):
            self.assertIn(heading, inventory)


if __name__ == "__main__":
    unittest.main()
