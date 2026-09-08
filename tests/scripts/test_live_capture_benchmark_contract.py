import importlib.util
import io
import json
import os
import pathlib
import re
import sys
import tempfile
import threading
import unittest
from unittest import mock
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

    def fixture_command(self, root, name="fixture with spaces ; no shell", mode="valid"):
        """Independent executable implementing the current C++ synthetic CLI only."""
        path = root / (name + ".py")
        source = r'''
import argparse, json, pathlib, struct, sys, time, zlib
p = argparse.ArgumentParser()
p.add_argument('--arm', choices=['process', 'integrated'], required=True)
for name in ('records', 'segments', 'generation', 'trial'):
    p.add_argument('--' + name, type=int, required=True)
a = p.parse_args()
pathlib.Path('invocation.json').write_text(json.dumps(sys.argv[1:]))
MODE = __MODE__
if (MODE == 'cwd_outcome' or MODE == 'artifact_collision:outcome.json'
        or (MODE.startswith('artifact_collision:') and pathlib.Path.cwd().name == 'work')):
    pathlib.Path('outcome.json').write_bytes(b'child-owned sentinel')
if MODE.startswith('artifact_collision:') and a.trial == 0:
    # Find the immutable plan independently of the runner's child-cwd layout.
    root = next(parent for parent in pathlib.Path.cwd().parents
                if (parent / 'plan.json').is_file())
    plan = json.loads((root / 'plan.json').read_text())
    target = MODE.split(':', 1)[1]
    directory = root / plan['trials'][0]['artifact_dir']
    if target == 'future':
        directory = root / plan['trials'][1]['artifact_dir']
        directory.mkdir()
        target = 'outcome.json'
    if target == 'summary.json':
        directory = root
    sentinel = directory / target
    if not sentinel.exists():
        sentinel.write_bytes(b'child-owned sentinel')
if MODE == 'timeout':
    time.sleep(60)
if MODE == 'exit':
    sys.stderr.write('private fixture diagnostic\n')
    sys.exit(7)
if MODE in ('overflow', 'stderr_overflow'):
    stream = sys.stderr if MODE == 'stderr_overflow' else sys.stdout
    stream.write('x' * 200000)
    stream.flush()
    sys.exit(0)
crc = size = 0
for segment in range(a.segments):
    for sequence in range(a.records // a.segments + (segment < a.records % a.segments)):
        payload = f'synthetic-segment-{segment}-record-{sequence}'.encode()
        frame = struct.pack('>4sHHIQIIQII', b'KLCB', 1, 44, 44 + len(payload),
                            a.generation, a.trial, segment, sequence, len(payload),
                            zlib.crc32(payload)) + payload
        crc = zlib.crc32(frame, crc)
        size += len(payload)
m = dict(arm_process=int(a.arm == 'process'), frame_version=1,
         generation=a.generation, trial=a.trial, fixture_crc32=crc,
         committed_records=a.records, committed_payload_bytes=size,
         segment_count=a.segments, normal_stop=1,
         lifecycle_start_ns=0, lifecycle_ready_ns=10,
         lifecycle_first_byte_ns=20, lifecycle_first_committed_record_ns=30,
         lifecycle_last_committed_record_ns=40, lifecycle_stop_ns=50,
         startup_ns=10, first_byte_latency_ns=10, first_commit_latency_ns=20,
         throughput_payload_bytes_per_second=size * 1000000000 // 20,
         teardown_ns=10, correctness_fixture_crc_match=1,
         correctness_sequence_gap_count=0, correctness_duplicate_count=0,
         correctness_crc_error_count=0)
for name in ('process_cpu_ns', 'child_cpu_ns', 'peak_rss_bytes',
             'voluntary_context_switches', 'involuntary_context_switches',
             'process_tree_children_started', 'maximum_live_children',
             'queue_high_water_bytes', 'queue_high_water_chunks',
             'queue_backpressure_events', 'queue_dropped_records'):
    m[name + '_available'] = 0
    m[name + '_synthetic'] = 0
if MODE in ('measured', 'partial'):
    if MODE == 'measured' or a.trial % 2 == 0:
        m['peak_rss_bytes_available'] = 1
        m['peak_rss_bytes'] = 4096
    m['queue_high_water_chunks_synthetic'] = 1
r = dict(schema_version=1, benchmark='synthetic-live-capture-' + a.arm,
         status='ok', reason_code=None, message='completed', metrics=m)
if MODE.startswith('mutate:'):
    key, value = MODE[7:].split('=', 1)
    if key in r:
        r[key] = json.loads(value)
    else:
        m[key] = json.loads(value)
if MODE.startswith('missing:'):
    del m[MODE[8:]]
if MODE == 'missing_trial' and a.trial == 1:
    sys.exit(0)
if MODE == 'failed':
    r.update(status='failed', reason_code='benchmark_failed', message='failed', metrics={})
text = json.dumps(r)
if MODE == 'truncated': text = text[:-2]
if MODE == 'duplicate_key': text = text.replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1')
if MODE == 'duplicate_result': text += '\n' + text
if MODE == 'empty': text = ''
if MODE == 'deep_json': text = '[' * 2000 + '0' + ']' * 2000
if MODE == 'invalid_utf8':
    sys.stdout.buffer.write(b'\xff\n')
    sys.exit(0)
sys.stdout.write(text + ('\n' if MODE != 'no_newline' else ''))
'''
        path.write_text(source.replace("__MODE__", repr(mode)), encoding="utf-8")
        return [sys.executable, str(path)]

    @staticmethod
    def fixture_producer(root, name="fixture producer"):
        path = root / name
        path.write_bytes(b"fixture-producer")
        path.chmod(0o700)
        return path

    @staticmethod
    def bind_fixture_producer(command, producer):
        command_file = pathlib.Path(command[1])
        command_file.write_text(
            command_file.read_text(encoding="utf-8")
            + f"\n# embedded fixture producer: {producer}\n",
            encoding="utf-8",
        )

    def comparison(self, root, *, mode="valid", **kwargs):
        before = self.fixture_command(root, mode=mode)
        after = self.fixture_command(root, name="after executable")
        before_producer = self.fixture_producer(root, "before fixture producer")
        after_producer = self.fixture_producer(root, "after fixture producer")
        self.bind_fixture_producer(before, before_producer)
        self.bind_fixture_producer(after, after_producer)
        return self.module.run_comparison(
            before=before, after=after, output_dir=root / "results",
            arms=("process",), trial_count=4, records=1, segments=1,
            generation=7, before_producer=before_producer,
            after_producer=after_producer, **kwargs,
        )

    def test_execution_minimal_valid_fixture_completes_cli_abba(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root)
            producer = self.fixture_producer(root)
            self.bind_fixture_producer(command, producer)
            output = io.StringIO()
            code = self.module.run_cli([
                "--before", *command, "--after", *command,
                "--before-producer", str(producer), "--after-producer", str(producer),
                "--output-dir", str(root / "new results"), "--arm", "process",
                "--records", "1", "--segments", "1", "--generation", "7",
            ], output=output)
            self.assertEqual(code, 0, output.getvalue())
            summary = json.loads((root / "new results" / "summary.json").read_text())
            self.assertEqual(summary["status"], "ok")
            self.assertEqual([r["variant"] for r in summary["trials"]],
                             ["before", "after", "after", "before"])
            self.assertEqual([r["trial"] for r in summary["trials"]], list(range(4)))
            self.assertEqual(len(summary["groups"]), 2)
            self.assertFalse(summary["coverage"]["real_device"])
            self.assertFalse(summary["coverage"]["ui_search_save"])
            self.assertIsNone(summary["requested_duration_ms"])
            for trial in summary["trials"]:
                self.assertEqual(trial["status"], "ok")
                self.assertGreater(trial["wall_elapsed_ns"], 0)
                directory = root / "new results" / trial["artifact_dir"]
                self.assertTrue((directory / "stdout.jsonl").is_file())
                self.assertTrue((directory / "outcome.json").is_file())
                args = json.loads((root / "new results" / trial["working_dir"] /
                                   "invocation.json").read_text())
                self.assertEqual(args, ["--arm", "process", "--records", "1",
                                       "--segments", "1", "--generation", "7",
                                       "--trial", str(trial["trial"])])
            for group in summary["groups"]:
                self.assertEqual(group["expected_count"], 2)
                self.assertEqual(group["successful_count"], 2)
                self.assertEqual(group["metrics"]["startup_ns"]["values"], [10, 10])
                self.assertIsNone(group["metrics"]["peak_rss_bytes"]["median"])

    def test_execution_child_cwd_cannot_collide_with_runner_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            summary = self.comparison(root, mode="cwd_outcome")
            self.assertEqual(summary["status"], "ok")
            for trial in summary["trials"]:
                directory = root / "results" / trial["artifact_dir"]
                child_cwd = root / "results" / trial["working_dir"]
                self.assertNotEqual(child_cwd, directory)
                self.assertEqual(json.loads((directory / "outcome.json").read_text()), trial)
                if trial["variant"] == "before":
                    self.assertEqual((child_cwd / "outcome.json").read_bytes(),
                                     b"child-owned sentinel")

    def test_execution_artifact_collisions_settle_every_planned_trial(self):
        for target in ("outcome.json", "stdout.jsonl", "stderr.bin", "future"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                summary = self.comparison(root, mode="artifact_collision:" + target)
                results = root / "results"
                persisted = json.loads((results / "summary.json").read_text())
                self.assertEqual(persisted, summary)
                self.assertEqual(summary["status"], "failed")
                self.assertFalse(summary["comparison_valid"])
                self.assertTrue(all(group["metrics"] is None for group in summary["groups"]))
                plan = json.loads((results / "plan.json").read_text())
                self.assertEqual([row["trial"] for row in summary["trials"]],
                                 [row["trial"] for row in plan["trials"]])
                failed_index = 1 if target == "future" else 0
                self.assertEqual([row["status"] for row in summary["trials"]],
                                 ["failed" if i == failed_index else "ok" for i in range(4)])
                failed = summary["trials"][failed_index]
                self.assertIsNotNone(failed["reason_code"])
                self.assertIsNone(failed["result"])
                if target in ("future", "outcome.json"):
                    self.assertFalse(failed["outcome_persisted"])
                directory = results / failed["artifact_dir"]
                sentinel = directory / ("outcome.json" if target == "future" else target)
                self.assertEqual(sentinel.read_bytes(), b"child-owned sentinel")
                for trial in summary["trials"]:
                    if trial["outcome_persisted"]:
                        self.assertEqual(json.loads((results / trial["artifact_dir"] /
                                                    "outcome.json").read_text()), trial)
                    if trial["variant"] == "before":
                        self.assertEqual((results / trial["working_dir"] /
                                          "outcome.json").read_bytes(), b"child-owned sentinel")
                if target == "future":
                    self.assertEqual(list(directory.iterdir()), [sentinel])

    def test_execution_cancellation_contains_future_directory_collision(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            event = threading.Event()
            execute = self.module._run_process
            def cancel_after_execution(*args, **kwargs):
                result = execute(*args, **kwargs)
                event.set()
                return result
            with mock.patch.object(self.module, "_run_process", side_effect=cancel_after_execution):
                summary = self.comparison(root, mode="artifact_collision:future", cancel_event=event)
            self.assertEqual(summary["status"], "cancelled")
            self.assertEqual([row["status"] for row in summary["trials"]],
                             ["ok", "failed", "not_run", "not_run"])
            self.assertFalse(summary["comparison_valid"])
            self.assertTrue(all(row["reason_code"] for row in summary["trials"][1:]))
            self.assertEqual(json.loads((root / "results" / "summary.json").read_text()), summary)
            sentinel = root / "results" / summary["trials"][1]["artifact_dir"] / "outcome.json"
            self.assertEqual(sentinel.read_bytes(), b"child-owned sentinel")

    def test_execution_unpersistable_summary_reports_fatal_without_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root, mode="artifact_collision:summary.json")
            producer = self.fixture_producer(root)
            self.bind_fixture_producer(command, producer)
            output = io.StringIO()
            code = self.module.run_cli([
                "--before", *command, "--after", *command, "--arm", "process",
                "--before-producer", str(producer), "--after-producer", str(producer),
                "--output-dir", str(root / "results"),
            ], output=output)
            self.assertEqual(code, 1)
            result = json.loads(output.getvalue())
            self.assertEqual(result["status"], "failed")
            self.assertEqual(result["reason_code"], "artifact_error")
            self.assertIn("fatal", result["message"])
            self.assertIn("not persisted", result["message"])
            self.assertNotIn("see", result["message"])
            self.assertEqual((root / "results" / "summary.json").read_bytes(),
                             b"child-owned sentinel")
            plan = json.loads((root / "results" / "plan.json").read_text())
            for trial in plan["trials"]:
                outcome = json.loads((root / "results" / trial["artifact_dir"] /
                                      "outcome.json").read_text())
                self.assertEqual(outcome["status"], "ok")

    def test_execution_both_arms_repeated_abba_and_no_shell(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root)
            producer = self.fixture_producer(root)
            self.bind_fixture_producer(command, producer)
            import subprocess
            with mock.patch.object(self.module.subprocess, "Popen", wraps=subprocess.Popen) as popen:
                summary = self.module.run_comparison(
                    before=command, after=command, output_dir=root / "results",
                    arms=("process", "integrated"), trial_count=8,
                    records=3, segments=2, generation=7,
                    before_producer=producer, after_producer=producer,
                )
            self.assertEqual(summary["status"], "ok")
            self.assertEqual(len(summary["trials"]), 16)
            for arm in ("process", "integrated"):
                rows = [r for r in summary["trials"] if r["arm"] == arm]
                self.assertEqual([r["variant"] for r in rows],
                                 ["before", "after", "after", "before"] * 2)
            for call in popen.call_args_list:
                self.assertIs(call.kwargs["shell"], False)
                self.assertEqual(call.args[0][:2], command)
                self.assertIn(root, pathlib.Path(call.kwargs["cwd"]).parents)

    def test_execution_rejects_invalid_terminal_results_without_cherry_picking(self):
        cases = (
            "exit", "failed", "truncated", "empty", "no_newline", "duplicate_key",
            "duplicate_result", "invalid_utf8", "deep_json", "overflow", "stderr_overflow",
            'mutate:benchmark="synthetic-live-capture-integrated"',
            'mutate:schema_version=true', 'mutate:status=[]',
            'mutate:trial=99', 'mutate:generation=8', 'mutate:arm_process=0',
            'mutate:frame_version=2', 'mutate:fixture_crc32=0',
            'mutate:committed_records=2', 'mutate:committed_payload_bytes=1',
            'mutate:segment_count=2', 'mutate:normal_stop=0',
            'mutate:correctness_fixture_crc_match=0',
            'mutate:correctness_sequence_gap_count=1',
            'mutate:lifecycle_stop_ns=1', 'mutate:startup_ns=99',
            'mutate:throughput_payload_bytes_per_second=0',
            'mutate:queue_high_water_bytes_available=1',
            'mutate:queue_high_water_bytes=5',
            'mutate:queue_high_water_bytes_synthetic=2',
            'mutate:process_cpu_ns_available=NaN',
            'missing:lifecycle_ready_ns', 'missing:trial',
            'mutate:unexpected_metric=1',
        )
        for mode in cases:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                summary = self.comparison(root, mode=mode)
                self.assertEqual(summary["status"], "failed")
                self.assertEqual(len(summary["trials"]), 4)
                self.assertEqual([r["status"] for r in summary["trials"]],
                                 ["failed", "ok", "ok", "failed"])
                self.assertTrue(all(r["reason_code"] for r in summary["trials"]
                                    if r["status"] == "failed"))
                self.assertFalse(summary["comparison_valid"])
                self.assertTrue(all(g["metrics"] is None for g in summary["groups"]))
                self.assertNotIn("private fixture diagnostic", json.dumps(summary))
                for artifact in (root / "results").glob("trial-*/*"):
                    if artifact.name in ("stdout.jsonl", "stderr.bin"):
                        self.assertLessEqual(artifact.stat().st_size, 65536)

    def test_execution_missing_requested_trial_is_not_a_successful_repeat(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root, mode="missing_trial")
            producer = self.fixture_producer(root)
            self.bind_fixture_producer(command, producer)
            summary = self.module.run_comparison(
                before=command, after=command, output_dir=root / "results",
                arms=("process",), trial_count=4, records=1, segments=1, generation=7,
                before_producer=producer, after_producer=producer,
            )
            self.assertEqual([r["status"] for r in summary["trials"]],
                             ["ok", "failed", "ok", "ok"])
            self.assertFalse(summary["comparison_valid"])

    def test_execution_timeout_and_cancellation_reap_child_and_keep_outcomes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root, mode="timeout")
            producer = self.fixture_producer(root)
            self.bind_fixture_producer(command, producer)
            summary = self.module.run_comparison(
                before=command, after=command, output_dir=root / "results",
                arms=("process",), trial_count=4, records=1, segments=1,
                timeout_seconds=0.2, before_producer=producer,
                after_producer=producer,
            )
            # Every child deliberately hangs; no normal startup must beat 200ms.
            self.assertEqual([r["reason_code"] for r in summary["trials"]], ["timeout"] * 4)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            event = threading.Event()
            import subprocess
            children = []
            real_popen = subprocess.Popen
            def cancelling_start(*args, **kwargs):
                child = real_popen(*args, **kwargs)
                children.append(child)
                event.set()
                return child
            with mock.patch.object(self.module.subprocess, "Popen", side_effect=cancelling_start):
                summary = self.comparison(root, mode="timeout", cancel_event=event)
            self.assertEqual(summary["status"], "cancelled")
            self.assertEqual(len(children), 1)
            self.assertIsNotNone(children[0].poll())
            self.assertEqual([r["status"] for r in summary["trials"]],
                             ["cancelled", "not_run", "not_run", "not_run"])
            self.assertTrue((root / "results" / "summary.json").is_file())

    def test_execution_requires_new_output_and_valid_configuration_before_launch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root)
            (root / "existing").mkdir()
            sentinel = root / "existing" / "sentinel"
            sentinel.write_bytes(b"user artifact")
            producer = self.fixture_producer(root)
            self.bind_fixture_producer(command, producer)
            base = dict(before=command, after=command, output_dir=root / "results",
                        arms=("process",), trial_count=4, records=1, segments=1,
                        before_producer=producer, after_producer=producer)
            cases = [dict(output_dir=None), dict(output_dir=root / "existing"),
                     dict(trial_count=2), dict(arms=("native",)), dict(arms=()),
                     dict(records=0), dict(segments=2), dict(generation=-1),
                     dict(timeout_seconds=float("nan")), dict(before=[str(root / "absent")])]
            with mock.patch.object(self.module.subprocess, "Popen") as popen:
                for overrides in cases:
                    with self.subTest(overrides=overrides):
                        with self.assertRaises(self.module.ConfigurationError):
                            self.module.run_comparison(**{**base, **overrides})
                popen.assert_not_called()
            self.assertEqual(sentinel.read_bytes(), b"user artifact")
            self.assertFalse((root / "results").exists())

    def test_execution_process_arm_requires_and_records_fixture_producer_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root)
            before_producer = root / "before fixture producer"
            after_producer = root / "after fixture producer"
            before_producer.write_bytes(b"before-producer")
            after_producer.write_bytes(b"after-producer")
            before_producer.chmod(0o700)
            after_producer.chmod(0o700)
            self.bind_fixture_producer(command, before_producer)
            self.bind_fixture_producer(command, after_producer)

            with mock.patch.object(self.module.subprocess, "Popen") as popen:
                with self.assertRaises(self.module.ConfigurationError):
                    self.module.run_comparison(
                        before=command, after=command, output_dir=root / "missing-producer",
                        arms=("process",), trial_count=4, records=1, segments=1,
                    )
                popen.assert_not_called()

            summary = self.module.run_comparison(
                before=command, after=command, output_dir=root / "results",
                arms=("process",), trial_count=4, records=1, segments=1,
                before_producer=before_producer, after_producer=after_producer,
            )
            self.assertEqual(summary["status"], "ok")
            for variant, producer in (("before", before_producer),
                                      ("after", after_producer)):
                recorded = summary["provenance"][variant]["process_fixture_producer"]
                self.assertEqual(recorded["path"], str(producer))
                self.assertEqual(recorded["resolved_path"], str(producer.resolve()))
                self.assertEqual(recorded["sha256"],
                                 self.module._sha256_file(producer))
                for trial in summary["trials"]:
                    if trial["variant"] == variant:
                        self.assertEqual(trial["process_fixture_producer"], recorded)

    def test_execution_process_arm_rejects_unembedded_fixture_producer(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root)
            embedded = self.fixture_producer(root, "embedded producer")
            unrelated = self.fixture_producer(root, "unrelated producer")
            pathlib.Path(command[1]).write_text(
                pathlib.Path(command[1]).read_text(encoding="utf-8")
                + f"\n# embedded producer: {embedded}\n",
                encoding="utf-8",
            )
            with self.assertRaises(self.module.ConfigurationError):
                self.module.run_comparison(
                    before=command, after=command, output_dir=root / "results",
                    arms=("process",), trial_count=4, records=1, segments=1,
                    before_producer=unrelated, after_producer=unrelated,
                )

    def test_execution_process_arm_fails_closed_when_fixture_producer_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root)
            producer = root / "fixture producer"
            producer.write_bytes(b"stable")
            producer.chmod(0o700)
            self.bind_fixture_producer(command, producer)
            real_execute = self.module._run_process

            def changed(*args, **kwargs):
                result = real_execute(*args, **kwargs)
                producer.write_bytes(b"changed")
                return result

            with mock.patch.object(self.module, "_run_process", side_effect=changed):
                summary = self.module.run_comparison(
                    before=command, after=command, output_dir=root / "results",
                    arms=("process",), trial_count=4, records=1, segments=1,
                    before_producer=producer, after_producer=producer,
                )
            self.assertEqual(summary["trials"][0]["reason_code"],
                             "provenance_mismatch")
            self.assertFalse(summary["comparison_valid"])

    def test_execution_integrated_arm_does_not_require_fixture_producer(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root)
            summary = self.module.run_comparison(
                before=command, after=command, output_dir=root / "results",
                arms=("integrated",), trial_count=4, records=1, segments=1,
            )
            self.assertEqual(summary["status"], "ok")
            self.assertNotIn("process_fixture_producer", summary["provenance"]["before"])
            self.assertEqual(
                summary["performance_interpretation"],
                {
                    "scope": "whole supplied executables",
                    "causal_attribution_available": False,
                    "elapsed_time_is_gate": False,
                    "policy": "retain every ordered trial and report observed trade-offs",
                },
            )

    def test_execution_counter_availability_and_synthetic_provenance_are_preserved(self):
        for mode in ("measured", "partial"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                summary = self.comparison(pathlib.Path(temporary), mode=mode)
                self.assertTrue(summary["comparison_valid"])
                group = summary["groups"][0]
                rss = group["metrics"]["peak_rss_bytes"]
                self.assertEqual(rss["values"], [4096, 4096 if mode == "measured" else None])
                self.assertEqual(rss["median"], 4096 if mode == "measured" else None)
                self.assertEqual(group["metrics"]["queue_high_water_chunks"]["synthetic"], [1, 1])
                self.assertEqual(group["metrics"]["queue_high_water_chunks"]["values"], [None, None])

    def test_execution_bounds_trial_count_before_allocating_order(self):
        with mock.patch.object(self.module, "balanced_abba_order", side_effect=AssertionError("unbounded allocation")):
            with self.assertRaises(self.module.ConfigurationError):
                self.module.run_comparison(
                    before=[], after=[], output_dir=None, trial_count=10**12,
                )

    def test_execution_keyboard_interrupt_reaps_running_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            import subprocess
            real_popen = subprocess.Popen
            children = []
            def interrupting_start(*args, **kwargs):
                child = real_popen(*args, **kwargs)
                children.append(child)
                original_wait = child.wait
                first = True
                def interrupted_wait(*wait_args, **wait_kwargs):
                    nonlocal first
                    if first:
                        first = False
                        raise KeyboardInterrupt
                    return original_wait(*wait_args, **wait_kwargs)
                child.wait = interrupted_wait
                return child
            with mock.patch.object(self.module.subprocess, "Popen", side_effect=interrupting_start):
                summary = self.comparison(root, mode="timeout")
            self.assertEqual(summary["status"], "cancelled")
            self.assertEqual(len(children), 1)
            self.assertIsNotNone(children[0].poll())
            self.assertEqual([r["status"] for r in summary["trials"]],
                             ["cancelled", "not_run", "not_run", "not_run"])

    def test_execution_cli_failure_and_unsupported_duration_are_terminal(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root, mode="exit")
            producer = self.fixture_producer(root)
            self.bind_fixture_producer(command, producer)
            output = io.StringIO()
            code = self.module.run_cli([
                "--before", *command, "--after", *command, "--arm", "process",
                "--before-producer", str(producer), "--after-producer", str(producer),
                "--output-dir", str(root / "results"),
            ], output=output)
            self.assertEqual(code, 1)
            self.assertEqual(json.loads(output.getvalue())["reason_code"], "comparison_failed")
            self.assertNotIn("private fixture diagnostic", output.getvalue())
            with mock.patch.object(self.module.subprocess, "Popen") as popen:
                for argv in (["--duration-ms", "60000"], ["--unknown-" + "x" * 2000],
                             ["--before", *command, "--after", *command]):
                    output = io.StringIO()
                    self.assertEqual(self.module.run_cli(argv, output=output), 1)
                    self.module.validate_result(json.loads(output.getvalue()))
                for mode in ("--dry-run", "--list-only"):
                    self.assertEqual(self.module.run_cli([
                        mode, "--before", *command, "--after", *command,
                        "--output-dir", str(root / "must not exist"),
                    ], output=io.StringIO()), 0)
                popen.assert_not_called()
            self.assertFalse((root / "must not exist").exists())

    def test_execution_changed_executable_provenance_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            command = self.fixture_command(root)
            producer = self.fixture_producer(root)
            self.bind_fixture_producer(command, producer)
            real_execute = self.module._run_process
            def changed(*args, **kwargs):
                result = real_execute(*args, **kwargs)
                pathlib.Path(command[1]).write_text("changed after execution")
                return result
            with mock.patch.object(self.module, "_run_process", side_effect=changed):
                summary = self.module.run_comparison(
                    before=command, after=command, output_dir=root / "results",
                    arms=("process",), trial_count=4, records=1, segments=1,
                    before_producer=producer, after_producer=producer,
                )
            self.assertEqual(summary["trials"][0]["reason_code"], "provenance_mismatch")
            self.assertFalse(summary["comparison_valid"])

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
        benchmark_test_main = (
            ROOT / "tests" / "unit" / "live_capture_benchmark_main.cpp"
        ).read_text(encoding="utf-8")

        self.assertIn("const bool PersistentInfo::ForcePortable = true;", benchmark_test_main)
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
