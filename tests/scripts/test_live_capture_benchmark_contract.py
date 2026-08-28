import importlib.util
import io
import json
import os
import pathlib
import re
import sys
import tempfile
import unittest
import zlib


ROOT = pathlib.Path(__file__).parents[2]
BENCHMARK_SCRIPT = ROOT / "scripts" / "run_live_capture_benchmarks.py"


class LiveCaptureBenchmarkContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BENCHMARK_SCRIPT.is_file():
            raise AssertionError(
                f"missing live-capture benchmark runner: {BENCHMARK_SCRIPT}"
            )

        spec = importlib.util.spec_from_file_location(
            "run_live_capture_benchmarks", BENCHMARK_SCRIPT
        )
        if spec is None or spec.loader is None:
            raise AssertionError(
                f"cannot load live-capture benchmark runner: {BENCHMARK_SCRIPT}"
            )
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        cls.module = module

    @staticmethod
    def record(
        sequence: int,
        *,
        generation: int = 7,
        trial: int = 3,
        reconnect: int = 1,
        payload=None,
    ) -> bytes:
        if payload is None:
            payload = f"fixture-{sequence}"
        document = {
            "generation": generation,
            "trial": trial,
            "reconnect": reconnect,
            "sequence": sequence,
            "payload": payload,
            "crc32": f"{zlib.crc32(payload.encode('utf-8')) & 0xFFFFFFFF:08x}",
        }
        return (json.dumps(document, sort_keys=True) + "\n").encode("utf-8")

    def validate(self, records, *, max_record_bytes=256):
        return self.module.validate_records(
            records,
            expected_generation=7,
            expected_trial=3,
            expected_reconnect=1,
            max_record_bytes=max_record_bytes,
        )

    def test_record_validation_accepts_contiguous_bound_records(self):
        summary = self.validate([self.record(0), self.record(1), self.record(2)])

        self.assertEqual(
            summary,
            {
                "first_sequence": 0,
                "last_sequence": 2,
                "payload_bytes": 27,
                "record_count": 3,
            },
        )

    def test_record_validation_rejects_gap_duplicate_and_crc_mismatch(self):
        invalid_crc = json.loads(self.record(1))
        invalid_crc["crc32"] = "00000000"
        invalid_crc_record = (json.dumps(invalid_crc) + "\n").encode("utf-8")
        scenarios = (
            ("gap", [self.record(0), self.record(2)], "gap"),
            (
                "duplicate",
                [self.record(0), self.record(1), self.record(1)],
                "duplicate",
            ),
            ("crc", [self.record(0), invalid_crc_record], "crc"),
        )

        for name, records, diagnostic in scenarios:
            with self.subTest(name=name):
                with self.assertRaisesRegex(
                    self.module.RecordValidationError, diagnostic
                ):
                    self.validate(records)

    def test_record_validation_rejects_generation_trial_and_reconnect_mismatch(self):
        scenarios = (
            ("generation", self.record(0, generation=8)),
            ("trial", self.record(0, trial=4)),
            ("reconnect", self.record(0, reconnect=2)),
        )

        for field, record in scenarios:
            with self.subTest(field=field):
                with self.assertRaisesRegex(
                    self.module.RecordValidationError, field
                ):
                    self.validate([record])

    def test_record_validation_rejects_malformed_and_oversized_records(self):
        scenarios = (
            ("malformed", b'{"sequence":\n', 256, "malformed"),
            ("oversized", b"x" * 257, 256, "oversized|size"),
        )

        for name, record, limit, diagnostic in scenarios:
            with self.subTest(name=name):
                with self.assertRaisesRegex(
                    self.module.RecordValidationError, diagnostic
                ):
                    self.validate([record], max_record_bytes=limit)

    def test_record_validation_rejects_duplicate_keys_and_noncanonical_crc(self):
        duplicate_key = self.record(0).decode("utf-8").replace(
            '"generation": 7', '"generation": 7, "generation": 7', 1
        ).encode("utf-8")
        uppercase_crc = json.loads(self.record(0, payload="canonical-crc"))
        uppercase_crc["crc32"] = uppercase_crc["crc32"].upper()
        self.assertNotEqual(uppercase_crc["crc32"], uppercase_crc["crc32"].lower())
        uppercase_crc_record = (json.dumps(uppercase_crc) + "\n").encode("utf-8")

        scenarios = (
            ("duplicate-key", duplicate_key, "duplicate"),
            ("noncanonical-crc", uppercase_crc_record, "canonical|lowercase"),
        )
        for name, record, diagnostic in scenarios:
            with self.subTest(name=name):
                with self.assertRaisesRegex(
                    self.module.RecordValidationError, diagnostic
                ):
                    self.validate([record])

    def test_record_validation_contains_deep_json_and_stream_iterator_failures(self):
        deeply_nested = b"[" * 1500 + b"0" + b"]" * 1500

        with self.assertRaisesRegex(
            self.module.RecordValidationError, "malformed|nesting"
        ):
            self.validate([deeply_nested], max_record_bytes=4096)

        def broken_stream():
            yield self.record(0)
            raise RuntimeError("fixture iterator failure")

        with self.assertRaisesRegex(
            self.module.RecordValidationError, "stream|iterator"
        ):
            self.validate(broken_stream())

    def test_balanced_order_is_deterministic_abba(self):
        expected = [
            "native",
            "subprocess",
            "subprocess",
            "native",
            "native",
            "subprocess",
            "subprocess",
            "native",
        ]

        first = self.module.balanced_abba_order(
            ("native", "subprocess"), trial_count=8
        )
        second = self.module.balanced_abba_order(
            ("native", "subprocess"), trial_count=8
        )

        self.assertEqual(first, expected)
        self.assertEqual(second, expected)
        self.assertEqual(first.count("native"), first.count("subprocess"))

    def test_usb_selection_uses_only_the_exact_requested_udid(self):
        devices = [
            {"udid": "FIRST-USB", "connection_type": "usb"},
            {"udid": "TARGET", "connection_type": "network"},
            {"udid": "TARGET", "connection_type": "usb"},
        ]

        selected = self.module.select_exact_usb_device(devices, "TARGET")

        self.assertIs(selected, devices[2])
        self.assertNotEqual(selected["udid"], devices[0]["udid"])

    def test_usb_selection_rejects_empty_network_only_missing_and_ambiguous(self):
        scenarios = (
            (
                "empty",
                [{"udid": "FIRST", "connection_type": "usb"}],
                "",
                "udid|empty|required",
            ),
            (
                "network-only",
                [{"udid": "TARGET", "connection_type": "network"}],
                "TARGET",
                "usb|network",
            ),
            (
                "missing",
                [{"udid": "OTHER", "connection_type": "usb"}],
                "TARGET",
                "missing|not found",
            ),
            (
                "ambiguous",
                [
                    {"udid": "TARGET", "connection_type": "usb", "id": 1},
                    {"udid": "TARGET", "connection_type": "usb", "id": 2},
                ],
                "TARGET",
                "ambiguous|multiple",
            ),
        )

        for name, devices, requested_udid, diagnostic in scenarios:
            with self.subTest(name=name):
                with self.assertRaisesRegex(
                    self.module.DeviceSelectionError, diagnostic
                ):
                    self.module.select_exact_usb_device(devices, requested_udid)

    def test_real_device_mode_is_disabled_by_default_and_requires_explicit_opt_in(self):
        self.assertIsNone(
            self.module.validate_real_device_options(
                enable_real_device=False,
                udid=None,
                native_stack_root=None,
            )
        )

        with tempfile.TemporaryDirectory() as temporary:
            absolute_root = pathlib.Path(temporary).resolve()
            scenarios = (
                (False, "TARGET", absolute_root, "opt-in|enable"),
                (True, None, absolute_root, "udid"),
                (True, "TARGET", None, "stack|root"),
                (True, "TARGET", pathlib.Path("relative/stack"), "absolute"),
            )
            for enabled, udid, stack_root, diagnostic in scenarios:
                with self.subTest(
                    enabled=enabled, udid=udid, stack_root=stack_root
                ):
                    with self.assertRaisesRegex(
                        self.module.ConfigurationError, diagnostic
                    ):
                        self.module.validate_real_device_options(
                            enable_real_device=enabled,
                            udid=udid,
                            native_stack_root=stack_root,
                        )

            self.assertEqual(
                self.module.validate_real_device_options(
                    enable_real_device=True,
                    udid="TARGET",
                    native_stack_root=absolute_root,
                ),
                {
                    "enabled": True,
                    "native_stack_root": absolute_root,
                    "udid": "TARGET",
                },
            )

    def test_dry_run_and_list_only_never_create_a_transport(self):
        for option in ("--dry-run", "--list-only"):
            with self.subTest(option=option):
                transports = []

                def transport_factory(*args, **kwargs):
                    transports.append((args, kwargs))
                    raise AssertionError("transport creation is forbidden")

                output = io.StringIO()
                exit_code = self.module.run_cli(
                    [option],
                    transport_factory=transport_factory,
                    output=output,
                )

                self.assertEqual(exit_code, 0, output.getvalue())
                self.assertEqual(transports, [])

    def test_cli_configuration_errors_are_versioned_results_with_consistent_exit_codes(self):
        scenarios = (
            ("partial dry-run options", ["--dry-run", "--udid", "TARGET"]),
            ("partial execution options", ["--udid", "TARGET"]),
            ("conflicting modes", ["--dry-run", "--list-only"]),
        )

        for name, argv in scenarios:
            with self.subTest(name=name):
                transports = []
                output = io.StringIO()

                def transport_factory(*args, **kwargs):
                    transports.append((args, kwargs))
                    raise AssertionError("transport creation is forbidden")

                exit_code = self.module.run_cli(
                    argv,
                    transport_factory=transport_factory,
                    output=output,
                )
                result = json.loads(output.getvalue())

                self.assertEqual(exit_code, 1)
                self.assertEqual(result["schema_version"], 1)
                self.assertEqual(result["status"], "failed")
                self.assertEqual(result["reason_code"], "configuration_error")
                self.assertEqual(transports, [])

    def test_result_schema_is_versioned_and_supports_all_terminal_statuses(self):
        scenarios = (
            (
                "ok",
                None,
                "completed",
                {"record_count": 10, "payload_bytes": 90},
            ),
            ("failed", "validation_failed", "crc mismatch", {}),
            ("not_run", "disabled", "real-device benchmarks disabled", {}),
        )

        for status, reason_code, message, metrics in scenarios:
            with self.subTest(status=status):
                result = self.module.build_result(
                    benchmark="ios-usb-live-capture",
                    status=status,
                    reason_code=reason_code,
                    message=message,
                    metrics=metrics,
                )
                self.module.validate_result(result)
                self.assertEqual(
                    set(result),
                    {
                        "schema_version",
                        "benchmark",
                        "status",
                        "reason_code",
                        "message",
                        "metrics",
                    },
                )
                self.assertEqual(result["schema_version"], 1)
                self.assertEqual(result["status"], status)
                self.assertEqual(
                    json.loads(self.module.serialize_result(result)), result
                )

    def test_unavailable_result_has_a_nonzero_exit_code(self):
        unavailable = self.module.build_result(
            benchmark="ios-usb-live-capture",
            status="not_run",
            reason_code="unavailable",
            message="requested USB device is unavailable",
            metrics={},
        )
        ok = self.module.build_result(
            benchmark="ios-usb-live-capture",
            status="ok",
            reason_code=None,
            message="completed",
            metrics={"record_count": 1},
        )

        self.assertEqual(self.module.result_exit_code(ok), 0)
        self.assertNotEqual(self.module.result_exit_code(unavailable), 0)

    def test_cleanup_root_must_exist_and_be_empty(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = pathlib.Path(temporary).resolve()
            empty = parent / "empty"
            empty.mkdir()
            self.module.require_empty_cleanup_root(empty)

            missing = parent / "missing"
            with self.assertRaisesRegex(
                self.module.CleanupError, "missing|exist"
            ):
                self.module.require_empty_cleanup_root(missing)

            nonempty = parent / "nonempty"
            nonempty.mkdir()
            (nonempty / ".leftover").write_text("fixture", encoding="utf-8")
            with self.assertRaisesRegex(
                self.module.CleanupError, "empty|leftover"
            ):
                self.module.require_empty_cleanup_root(nonempty)

    def test_cleanup_root_rejects_relative_and_symlinked_parent_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = pathlib.Path(temporary).resolve()
            relative_root = parent / "relative-empty"
            relative_root.mkdir()
            previous_cwd = pathlib.Path.cwd()
            os.chdir(parent)
            try:
                with self.assertRaisesRegex(self.module.CleanupError, "absolute"):
                    self.module.require_empty_cleanup_root(pathlib.Path("relative-empty"))
            finally:
                os.chdir(previous_cwd)

            real_parent = parent / "real-parent"
            real_parent.mkdir()
            (real_parent / "empty").mkdir()
            alias_parent = parent / "alias-parent"
            try:
                alias_parent.symlink_to(real_parent, target_is_directory=True)
            except OSError as error:
                self.skipTest(f"directory symlinks are unavailable: {error}")
            with self.assertRaisesRegex(self.module.CleanupError, "symlink"):
                self.module.require_empty_cleanup_root(alias_parent / "empty")

    def test_cleanup_root_nonempty_diagnostic_is_deterministic(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary).resolve()
            (root / "z-leftover").write_text("z", encoding="utf-8")
            (root / "a-leftover").write_text("a", encoding="utf-8")

            with self.assertRaises(self.module.CleanupError) as raised:
                self.module.require_empty_cleanup_root(root)

            self.assertEqual(str(raised.exception), "cleanup root must be empty")

    def test_result_privacy_and_synthetic_target_release_isolation(self):
        tests_cmake = (ROOT / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
        unit_tests_cmake = (ROOT / "tests" / "unit" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        benchmarks_cmake = (ROOT / "benchmarks" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        core_source = (
            ROOT / "benchmarks" / "live_capture_benchmark_core.cpp"
        ).read_text(encoding="utf-8")
        producer_source = (
            ROOT / "benchmarks" / "live_capture_fixture_producer.cpp"
        ).read_text(encoding="utf-8")

        for token in (
            "public ProcessLiveSourceTransport",
            "IosNativeTransport",
            "AdbLogcatSource",
            "LiveLogController",
            "StreamingLogData",
        ):
            self.assertIn(token, core_source)
        for token in ("defined( _WIN32 )", "_setmode", "_O_BINARY"):
            self.assertIn(token, producer_source)

        self.assertRegex(
            benchmarks_cmake,
            r"live_capture_fixture_producer\s+PRIVATE\s+klogg_live_capture_benchmark_protocol",
        )
        self.assertIn("add_dependencies(ci_build klogg_test_build)", tests_cmake)
        self.assertIn(
            "klogg_configure_test_target(klogg_live_capture_benchmark_contract_tests)",
            unit_tests_cmake,
        )
        ci_dependencies = "\n".join(
            match.group(0)
            for match in re.finditer(
                r"add_dependencies\(\s*ci_build\b.*?\)",
                tests_cmake + "\n" + benchmarks_cmake + "\n" + root_cmake,
                flags=re.DOTALL,
            )
        )
        self.assertNotIn("live_capture_process_integrated_benchmark", ci_dependencies)
        self.assertNotIn("live_capture_fixture_producer", ci_dependencies)

        install_calls = "\n".join(
            match.group(0)
            for match in re.finditer(
                r"install\s*\(.*?\)", benchmarks_cmake + "\n" + root_cmake, flags=re.DOTALL
            )
        )
        self.assertNotIn("live_capture_process_integrated_benchmark", install_calls)
        self.assertNotIn("live_capture_fixture_producer", install_calls)

        base = self.module.build_result(
            benchmark="ios-usb-live-capture",
            status="ok",
            reason_code=None,
            message="completed",
            metrics={"record_count": 1, "payload_bytes": 9},
        )
        mutations = (
            ("top-level raw content", {**base, "raw_content": "SECRET-LOG-LINE"}),
            ("captured records", {**base, "records": ["SECRET-LOG-LINE"]}),
            (
                "string payload metric",
                {**base, "metrics": {**base["metrics"], "payload": "SECRET-LOG-LINE"}},
            ),
            (
                "byte payload metric",
                {**base, "metrics": {**base["metrics"], "sample": b"SECRET"}},
            ),
            (
                "nested raw content",
                {
                    **base,
                    "metrics": {
                        **base["metrics"],
                        "diagnostics": {"raw_content": "SECRET-LOG-LINE"},
                    },
                },
            ),
        )

        for name, result in mutations:
            with self.subTest(name=name):
                with self.assertRaises(self.module.ResultSchemaError):
                    self.module.serialize_result(result)

        serialized = self.module.serialize_result(base)
        self.assertNotIn("SECRET", serialized)
        self.assertNotIn("raw_content", serialized)
        self.assertNotIn('"payload"', serialized)
        self.assertNotIn('"records"', serialized)


if __name__ == "__main__":
    unittest.main()
