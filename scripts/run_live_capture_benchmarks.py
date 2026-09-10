#!/usr/bin/env python3
"""Run record-count bounded synthetic before/after benchmarks in ABBA order.

Both process and integrated arms exercise fixture bytes through the controller,
StreamingLogData and CaptureStore. Integrated is NOT a device/socket/parser/UI
acceptance arm. No duration, slow-sink, heartbeat or sustained-RSS seam exists in
this synthetic CLI. Native-device orchestration remains unavailable (the separate
C++ real-device CLI supports only 500-10000 ms samples, not lossless acceptance).

Use --before /path/to/before-binary --after /path/to/after-binary --output-dir NEW.
Process arms also require --before-producer and --after-producer naming the exact
fixture executables embedded in those benchmark binaries. Command prefixes may
include positional arguments; they are argv, never shell text. Results/error
streams are bounded; destinations are never reused. ABBA balances before/after
within each arm, not between arms. Executable and producer hashes identify the
supplied files, not their source trees or remaining runtime dependencies.
Timeout/cancellation kills the process group on POSIX; on Windows only the direct
child is guaranteed reaped. Dry-run/list-only never launch a subprocess.
"""

import argparse
import hashlib
import json
import math
import os
import pathlib
import re
import secrets
import signal
import statistics
import struct
import subprocess
import threading
import time
import zlib


RESULT_SCHEMA_VERSION = 1
DEFAULT_MAX_RECORD_BYTES = 64 * 1024
HARD_MAX_RECORD_BYTES = 1024 * 1024
MAX_RECORD_COUNT = 1_000_000
MAX_RESULT_METRICS = 64
MAX_RESULT_MESSAGE_BYTES = 1024

_RECORD_FIELDS = {
    "generation",
    "trial",
    "reconnect",
    "sequence",
    "payload",
    "crc32",
}
_RESULT_FIELDS = {
    "schema_version",
    "benchmark",
    "status",
    "reason_code",
    "message",
    "metrics",
}
_RESULT_STATUSES = {"ok", "failed", "not_run"}
_FORBIDDEN_METRIC_NAMES = {
    "content",
    "line",
    "lines",
    "log",
    "logs",
    "payload",
    "raw",
    "raw_content",
    "record",
    "records",
    "sample",
    "samples",
    "stderr",
    "stdout",
}
_SAFE_IDENTIFIER = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,127}$")
_SAFE_METRIC_NAME = re.compile(r"^[a-z][a-z0-9_]{0,63}$")
_CRC32 = re.compile(r"^[0-9a-f]{8}$")

AVAILABLE_BENCHMARKS = ("ios-usb-live-capture", "synthetic-live-capture-comparison")


class BenchmarkContractError(ValueError):
    """Base class for deterministic benchmark contract failures."""


class RecordValidationError(BenchmarkContractError):
    pass


class DeviceSelectionError(BenchmarkContractError):
    pass


class ConfigurationError(BenchmarkContractError):
    pass


class ResultSchemaError(BenchmarkContractError):
    pass


class CleanupError(BenchmarkContractError):
    pass


def _required_integer(document, field):
    value = document.get(field)
    if isinstance(value, bool) or not isinstance(value, int):
        raise RecordValidationError(f"record {field} must be an integer")
    if value < 0:
        raise RecordValidationError(f"record {field} must not be negative")
    return value


def _unique_json_object(pairs):
    document = {}
    for key, value in pairs:
        if key in document:
            raise RecordValidationError(f"duplicate JSON key {key!r}")
        document[key] = value
    return document


def _record_iterator(records):
    try:
        iterator = iter(records)
    except TypeError as error:
        raise RecordValidationError("record stream must be iterable") from error
    while True:
        try:
            yield next(iterator)
        except StopIteration:
            return
        except Exception as error:
            raise RecordValidationError(f"record stream iterator failed: {error}") from error


def validate_records(
    records,
    *,
    expected_generation,
    expected_trial,
    expected_reconnect,
    max_record_bytes=DEFAULT_MAX_RECORD_BYTES,
):
    """Validate bounded newline-delimited JSON records and return aggregates."""

    if (
        isinstance(max_record_bytes, bool)
        or not isinstance(max_record_bytes, int)
        or max_record_bytes <= 0
        or max_record_bytes > HARD_MAX_RECORD_BYTES
    ):
        raise RecordValidationError(
            f"record size limit must be between 1 and {HARD_MAX_RECORD_BYTES} bytes"
        )

    expected_bindings = {
        "generation": expected_generation,
        "trial": expected_trial,
        "reconnect": expected_reconnect,
    }
    for field, value in expected_bindings.items():
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise RecordValidationError(f"expected {field} must be a non-negative integer")

    first_sequence = None
    last_sequence = None
    record_count = 0
    payload_bytes = 0

    for raw_record in _record_iterator(records):
        record_count += 1
        if record_count > MAX_RECORD_COUNT:
            raise RecordValidationError(
                f"record count exceeds bounded limit {MAX_RECORD_COUNT}"
            )
        if not isinstance(raw_record, (bytes, bytearray, memoryview)):
            raise RecordValidationError("malformed record: expected bytes")
        encoded = bytes(raw_record)
        if len(encoded) > max_record_bytes:
            raise RecordValidationError(
                f"oversized record: {len(encoded)} bytes exceeds size limit "
                f"{max_record_bytes}"
            )

        try:
            text = encoded.decode("utf-8")
            document = json.loads(text, object_pairs_hook=_unique_json_object)
        except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
            raise RecordValidationError(f"malformed record: {error}") from error
        if not isinstance(document, dict):
            raise RecordValidationError("malformed record: expected a JSON object")
        if set(document) != _RECORD_FIELDS:
            missing = sorted(_RECORD_FIELDS - set(document))
            extra = sorted(set(document) - _RECORD_FIELDS)
            raise RecordValidationError(
                f"malformed record fields; missing={missing}, extra={extra}"
            )

        for field, expected in expected_bindings.items():
            actual = _required_integer(document, field)
            if actual != expected:
                raise RecordValidationError(
                    f"{field} mismatch: expected {expected}, received {actual}"
                )

        sequence = _required_integer(document, "sequence")
        if first_sequence is None:
            if sequence != 0:
                raise RecordValidationError(
                    f"sequence gap: expected first sequence 0, received {sequence}"
                )
            first_sequence = sequence
        else:
            expected_sequence = last_sequence + 1
            if sequence == last_sequence or sequence < expected_sequence:
                raise RecordValidationError(
                    f"duplicate or out-of-order sequence {sequence}"
                )
            if sequence > expected_sequence:
                raise RecordValidationError(
                    f"sequence gap: expected {expected_sequence}, received {sequence}"
                )
        last_sequence = sequence

        payload = document.get("payload")
        if not isinstance(payload, str):
            raise RecordValidationError("malformed record payload: expected text")
        encoded_payload = payload.encode("utf-8")
        crc32 = document.get("crc32")
        if not isinstance(crc32, str) or _CRC32.fullmatch(crc32) is None:
            raise RecordValidationError(
                "record crc32 must use canonical eight-digit lowercase hexadecimal"
            )
        expected_crc32 = f"{zlib.crc32(encoded_payload) & 0xFFFFFFFF:08x}"
        if crc32 != expected_crc32:
            raise RecordValidationError(
                f"crc mismatch for sequence {sequence}: expected {expected_crc32}"
            )
        payload_bytes += len(encoded_payload)

    if record_count == 0:
        raise RecordValidationError("record stream must not be empty")

    return {
        "first_sequence": first_sequence,
        "last_sequence": last_sequence,
        "payload_bytes": payload_bytes,
        "record_count": record_count,
    }


def balanced_abba_order(variants, *, trial_count):
    """Return a deterministic balanced A-B-B-A order for two variants."""

    try:
        pair = tuple(variants)
    except TypeError as error:
        raise ConfigurationError("ABBA ordering requires exactly two variants") from error
    if len(pair) != 2 or pair[0] == pair[1]:
        raise ConfigurationError("ABBA ordering requires two distinct variants")
    if (
        isinstance(trial_count, bool)
        or not isinstance(trial_count, int)
        or trial_count <= 0
        or trial_count % 4 != 0
    ):
        raise ConfigurationError("ABBA trial count must be a positive multiple of four")
    return [pair[index] for _ in range(trial_count // 4) for index in (0, 1, 1, 0)]


def select_exact_usb_device(devices, requested_udid):
    """Select one exact USB endpoint; never infer a device from list order."""

    if not isinstance(requested_udid, str) or not requested_udid.strip():
        raise DeviceSelectionError("an explicit non-empty udid is required")

    matching_udid = []
    matching_usb = []
    for device in devices:
        if not isinstance(device, dict):
            raise DeviceSelectionError("malformed device listing entry")
        if device.get("udid") != requested_udid:
            continue
        matching_udid.append(device)
        connection_type = device.get("connection_type")
        if isinstance(connection_type, str) and connection_type.casefold() == "usb":
            matching_usb.append(device)

    if not matching_udid:
        raise DeviceSelectionError(f"requested udid {requested_udid!r} was not found")
    if not matching_usb:
        raise DeviceSelectionError(
            f"requested udid {requested_udid!r} has no USB endpoint; network-only is rejected"
        )
    if len(matching_usb) != 1:
        raise DeviceSelectionError(
            f"ambiguous USB selection for udid {requested_udid!r}: "
            f"found {len(matching_usb)} endpoints"
        )
    return matching_usb[0]


def validate_real_device_options(*, enable_real_device, udid, native_stack_root):
    """Fail closed unless real-device access is explicitly and fully configured."""

    options_present = udid is not None or native_stack_root is not None
    if not enable_real_device:
        if options_present:
            raise ConfigurationError(
                "real-device options require explicit --enable-real-device opt-in"
            )
        return None

    if not isinstance(udid, str) or not udid.strip():
        raise ConfigurationError("real-device mode requires an explicit udid")
    if native_stack_root is None:
        raise ConfigurationError("real-device mode requires a native stack root")
    try:
        root = pathlib.Path(native_stack_root)
    except TypeError as error:
        raise ConfigurationError("native stack root must be a filesystem path") from error
    if not root.is_absolute():
        raise ConfigurationError("native stack root must be absolute")
    if not root.is_dir():
        raise ConfigurationError("native stack root must be an existing directory")

    return {
        "enabled": True,
        "native_stack_root": root.resolve(),
        "udid": udid,
    }


def _validate_metric_value(name, value):
    if name in _FORBIDDEN_METRIC_NAMES:
        raise ResultSchemaError(f"raw-content metric {name!r} is forbidden")
    if _SAFE_METRIC_NAME.fullmatch(name) is None:
        raise ResultSchemaError(f"invalid metric name {name!r}")
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ResultSchemaError(
            f"metric {name!r} must be an aggregate numeric value, not raw content"
        )
    if isinstance(value, float) and not math.isfinite(value):
        raise ResultSchemaError(f"metric {name!r} must be finite")


def validate_result(result):
    """Validate the closed, aggregate-only result schema."""

    if not isinstance(result, dict):
        raise ResultSchemaError("result must be a JSON object")
    if set(result) != _RESULT_FIELDS:
        missing = sorted(_RESULT_FIELDS - set(result))
        extra = sorted(set(result) - _RESULT_FIELDS)
        raise ResultSchemaError(
            f"result schema fields mismatch; missing={missing}, extra={extra}"
        )
    if type(result.get("schema_version")) is not int or result["schema_version"] != RESULT_SCHEMA_VERSION:
        raise ResultSchemaError(
            f"unsupported result schema version {result.get('schema_version')!r}"
        )

    benchmark = result.get("benchmark")
    if not isinstance(benchmark, str) or _SAFE_IDENTIFIER.fullmatch(benchmark) is None:
        raise ResultSchemaError("benchmark must be a bounded identifier")

    status = result.get("status")
    if not isinstance(status, str) or status not in _RESULT_STATUSES:
        raise ResultSchemaError(f"invalid terminal status {status!r}")

    reason_code = result.get("reason_code")
    if status == "ok":
        if reason_code is not None:
            raise ResultSchemaError("ok results must not have a reason code")
    elif not isinstance(reason_code, str) or _SAFE_IDENTIFIER.fullmatch(reason_code) is None:
        raise ResultSchemaError(f"{status} results require a bounded reason code")

    message = result.get("message")
    if not isinstance(message, str) or not message:
        raise ResultSchemaError("result message must be non-empty text")
    try:
        encoded_message = message.encode("utf-8")
    except UnicodeEncodeError as error:
        raise ResultSchemaError("result message must be valid UTF-8") from error
    if len(encoded_message) > MAX_RESULT_MESSAGE_BYTES:
        raise ResultSchemaError("result message exceeds its bounded size")
    if "\n" in message or "\r" in message or "\x00" in message:
        raise ResultSchemaError("result message must be a single diagnostic line")

    metrics = result.get("metrics")
    if not isinstance(metrics, dict):
        raise ResultSchemaError("result metrics must be a JSON object")
    if len(metrics) > MAX_RESULT_METRICS:
        raise ResultSchemaError("result contains too many metrics")
    for name, value in metrics.items():
        if not isinstance(name, str):
            raise ResultSchemaError("metric names must be text")
        _validate_metric_value(name, value)

    return result


def build_result(*, benchmark, status, reason_code, message, metrics):
    result = {
        "schema_version": RESULT_SCHEMA_VERSION,
        "benchmark": benchmark,
        "status": status,
        "reason_code": reason_code,
        "message": message,
        "metrics": dict(metrics),
    }
    return validate_result(result)


def serialize_result(result):
    validate_result(result)
    return json.dumps(result, sort_keys=True, separators=(",", ":")) + "\n"


def result_exit_code(result):
    validate_result(result)
    if result["status"] == "ok":
        return 0
    if result["status"] == "not_run" and result["reason_code"] == "disabled":
        return 0
    if result["status"] == "not_run" and result["reason_code"] == "unavailable":
        return 2
    return 1


def require_empty_cleanup_root(root):
    """Require an existing real directory with no visible or hidden entries."""

    try:
        path = pathlib.Path(root)
    except TypeError as error:
        raise CleanupError("cleanup root must be a filesystem path") from error
    if not path.is_absolute():
        raise CleanupError("cleanup root must be absolute")
    if path.is_symlink():
        raise CleanupError("cleanup root must not be a symlink")
    if not path.exists():
        raise CleanupError(f"cleanup root does not exist: {path}")
    if not path.is_dir():
        raise CleanupError(f"cleanup root is not a directory: {path}")
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        raise CleanupError(f"cannot resolve cleanup root {path}: {error}") from error
    if resolved != path:
        raise CleanupError("cleanup root path must not traverse a symlink")
    try:
        first_entry = next(path.iterdir(), None)
    except OSError as error:
        raise CleanupError(f"cannot inspect cleanup root {path}: {error}") from error
    if first_entry is not None:
        raise CleanupError("cleanup root must be empty")


# These fields mirror serializeAggregateJson, not the separate real-device schema.
_COUNTER_METRICS = (
    "process_cpu_ns", "child_cpu_ns", "peak_rss_bytes",
    "voluntary_context_switches", "involuntary_context_switches",
    "process_tree_children_started", "maximum_live_children",
    "queue_high_water_bytes", "queue_high_water_chunks",
    "queue_backpressure_events", "queue_dropped_records",
)
_TIMELINE_METRICS = (
    "lifecycle_start_ns", "lifecycle_ready_ns", "lifecycle_first_byte_ns",
    "lifecycle_first_committed_record_ns", "lifecycle_last_committed_record_ns",
    "lifecycle_stop_ns",
)
_TIMING_METRICS = (
    "startup_ns", "first_byte_latency_ns", "first_commit_latency_ns",
    "teardown_ns", "throughput_payload_bytes_per_second",
)
MAX_PROCESS_OUTPUT_BYTES = 64 * 1024  # Per stream, including retained error output.
MAX_COMPARISON_TRIALS = 1000  # Per requested arm; every trial is accounted for.


class ExecutionError(BenchmarkContractError):
    def __init__(self, reason_code, message):
        super().__init__(message)
        self.reason_code = reason_code


def _fixture_metadata(records, segments, generation, trial):
    """Hash the exact C++ CLI KLCB fixture incrementally, without keeping payloads."""
    checksum = payload_bytes = 0
    for segment in range(segments):
        count = records // segments + (segment < records % segments)
        for sequence in range(count):
            payload = f"synthetic-segment-{segment}-record-{sequence}".encode("ascii")
            header = struct.pack(
                ">4sHHIQIIQII", b"KLCB", 1, 44, 44 + len(payload),
                generation, trial, segment, sequence, len(payload), zlib.crc32(payload),
            )
            checksum = zlib.crc32(payload, zlib.crc32(header, checksum))
            payload_bytes += len(payload)
    return {"fixture_crc32": checksum, "committed_payload_bytes": payload_bytes}


def validate_synthetic_result(data, *, arm, records, segments, generation, trial):
    """Require exactly one complete, correctly bound synthetic arm result."""
    if not data.endswith(b"\n"):
        raise ResultSchemaError("missing or truncated result (newline required)")
    try:
        result = json.loads(data.decode("utf-8"), object_pairs_hook=_unique_json_object)
        validate_result(result)
    except (ValueError, UnicodeError, RecursionError) as error:
        # Do not copy raw output or untrusted field names into aggregate diagnostics.
        raise ResultSchemaError("malformed benchmark JSON or result schema") from error
    if result["status"] != "ok":
        raise ResultSchemaError("benchmark reported a non-success terminal result")
    if result["benchmark"] != f"synthetic-live-capture-{arm}":
        raise ResultSchemaError("missing requested arm or benchmark identity mismatch")
    metrics = result["metrics"]
    expected = {
        "arm_process": int(arm == "process"), "frame_version": 1,
        "generation": generation, "trial": trial,
        "committed_records": records, "segment_count": segments, "normal_stop": 1,
        "correctness_fixture_crc_match": 1, "correctness_sequence_gap_count": 0,
        "correctness_duplicate_count": 0, "correctness_crc_error_count": 0,
        **_fixture_metadata(records, segments, generation, trial),
    }
    fields = set(expected) | set(_TIMELINE_METRICS) | set(_TIMING_METRICS)
    for name in _COUNTER_METRICS:
        fields.update((name + "_available", name + "_synthetic"))
        if metrics.get(name + "_available") == 1:
            fields.add(name)
    if set(metrics) != fields:
        raise ResultSchemaError("synthetic metric fields incomplete or inconsistent")
    for value in metrics.values():
        if type(value) is not int or not 0 <= value <= (1 << 64) - 1:
            raise ResultSchemaError("synthetic metrics must be uint64 integers")
    for name, value in expected.items():
        if metrics[name] != value:
            raise ResultSchemaError(f"synthetic {name} metadata mismatch")
    for name in _COUNTER_METRICS:
        if any(metrics[name + suffix] not in (0, 1)
               for suffix in ("_available", "_synthetic")):
            raise ResultSchemaError("invalid counter availability or provenance flag")
    timeline = [metrics[name] for name in _TIMELINE_METRICS]
    if timeline != sorted(timeline):
        raise ResultSchemaError("non-monotonic benchmark lifecycle")
    start, ready, first_byte, first_commit, last_commit, stop = timeline
    derived = {
        "startup_ns": ready - start, "first_byte_latency_ns": first_byte - ready,
        "first_commit_latency_ns": first_commit - ready, "teardown_ns": stop - last_commit,
    }
    for name, value in derived.items():
        if metrics[name] != value:
            raise ResultSchemaError(f"inconsistent {name}")
    interval = last_commit - first_byte
    rate = expected["committed_payload_bytes"] * 1_000_000_000 // interval if interval else 0
    # C++ computes with long double, which is double on some supported platforms.
    # Allow its final integer rounding, not arbitrary self-reported throughput.
    if abs(metrics["throughput_payload_bytes_per_second"] - rate) > (1 if interval else 0):
        raise ResultSchemaError("inconsistent throughput_payload_bytes_per_second")
    return result


def _bounded_diagnostic(message):
    text = " ".join(str(message).replace("\x00", " ").split())
    return text.encode("utf-8", errors="replace")[:MAX_RESULT_MESSAGE_BYTES].decode(
        "utf-8", errors="ignore"
    ) or "benchmark execution failed"


def _write_json(path, document):
    # No replace/rename-overwrite, including when a child leaves an unexpected file.
    with path.open("x", encoding="utf-8") as output:
        json.dump(document, output, indent=2, sort_keys=True, allow_nan=False)
        output.write("\n")


def _sha256_file(path):
    digest = hashlib.sha256()
    with pathlib.Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _file_provenance(path, *, label, require_executable=False):
    if path is None:
        raise ConfigurationError(f"{label} path is required")
    path = pathlib.Path(path)
    if not path.is_absolute() or not path.is_file():
        raise ConfigurationError(f"{label} must be an absolute regular file")
    if require_executable and not os.access(path, os.X_OK):
        raise ConfigurationError(f"{label} must be executable")
    return {"path": str(path), "resolved_path": str(path.resolve()),
            "sha256": _sha256_file(path), "size_bytes": path.stat().st_size}


def _command_provenance(command):
    if not isinstance(command, (list, tuple)) or not command:
        raise ConfigurationError("before/after must be non-empty argv sequences")
    if any(not isinstance(arg, str) or not arg or "\x00" in arg for arg in command):
        raise ConfigurationError("command arguments must be non-empty text without NUL")
    executable = pathlib.Path(command[0])
    if not executable.is_absolute() or not executable.is_file() or not os.access(executable, os.X_OK):
        raise ConfigurationError("benchmark executable must be an absolute executable file")
    files = []
    for arg in command:
        path = pathlib.Path(arg)
        if path.is_file():
            if not path.is_absolute():
                raise ConfigurationError("command file arguments must be absolute paths")
            files.append(_file_provenance(path, label="command file"))
    return {"argv_prefix": list(command), "files": files}


def _process_fixture_producer_provenance(command_provenance, producer):
    result = _file_provenance(
        producer, label="process fixture producer", require_executable=True,
    )
    needles = {os.fsencode(result["path"]), os.fsencode(result["resolved_path"])}
    embedded_in = []
    for command_file in command_provenance["files"]:
        if any(needle in pathlib.Path(command_file["path"]).read_bytes()
               for needle in needles):
            embedded_in.append(command_file["resolved_path"])
    if not embedded_in:
        raise ConfigurationError(
            "process fixture producer path is not embedded in a command file"
        )
    result["embedded_path_verified"] = True
    result["embedded_in"] = embedded_in
    return result


def _provenance_matches(provenance):
    try:
        current = _command_provenance(provenance["argv_prefix"])
        if current != {key: provenance[key] for key in ("argv_prefix", "files")}:
            return False
        producer = provenance.get("process_fixture_producer")
        if producer is not None:
            current_producer = _process_fixture_producer_provenance(
                current, pathlib.Path(producer["path"])
            )
            if current_producer != producer:
                return False
        return True
    except (ConfigurationError, KeyError, OSError, TypeError, ValueError):
        return False


def _kill_process(process):
    if os.name == "posix":
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    elif process.poll() is None:
        process.kill()
    process.wait()


def _run_process(argv, *, cwd, timeout_seconds, cancel_event):
    """Bound both streams while waiting; never wait on a child with full pipes."""
    started = time.monotonic_ns()
    process = None
    buffers = [bytearray(), bytearray()]
    overflow = threading.Event()
    read_failure = threading.Event()
    readers = []
    reason = None
    message = "completed"

    def drain(pipe, buffer):
        try:
            while True:
                chunk = os.read(pipe.fileno(), 8192)
                if not chunk:
                    return
                remaining = MAX_PROCESS_OUTPUT_BYTES - len(buffer)
                buffer.extend(chunk[:remaining])
                if len(chunk) > remaining:
                    overflow.set()
                    return
        except OSError:
            read_failure.set()

    try:
        process = subprocess.Popen(
            list(argv), shell=False, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=cwd,
            start_new_session=(os.name == "posix"),
        )
        for pipe, buffer in zip((process.stdout, process.stderr), buffers):
            reader = threading.Thread(target=drain, args=(pipe, buffer), daemon=True)
            reader.start()
            readers.append(reader)
        deadline = time.monotonic() + timeout_seconds
        while True:
            if cancel_event is not None and cancel_event.is_set():
                raise ExecutionError("cancelled", "benchmark comparison cancelled")
            if overflow.is_set():
                raise ExecutionError("output_limit", "benchmark output exceeded per-stream limit")
            if read_failure.is_set():
                raise ExecutionError("output_read_failed", "could not read benchmark output")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ExecutionError("timeout", "benchmark exceeded per-trial timeout")
            try:
                process.wait(timeout=min(remaining, 0.05))
                break
            except subprocess.TimeoutExpired:
                continue
    except KeyboardInterrupt:
        reason, message = "cancelled", "benchmark comparison interrupted"
    except ExecutionError as error:
        reason, message = error.reason_code, str(error)
    except OSError:
        reason, message = "launch_failed", "could not launch benchmark executable"
    finally:
        if process is not None:
            # Also terminate inherited pipe holders in the POSIX process group.
            _kill_process(process)
            for reader in readers:
                reader.join(timeout=1)
            for pipe in (process.stdout, process.stderr):
                pipe.close()
    if reason is None:
        if overflow.is_set():
            reason, message = "output_limit", "benchmark output exceeded per-stream limit"
        elif read_failure.is_set() or any(reader.is_alive() for reader in readers):
            reason, message = "output_read_failed", "benchmark output stream did not close"
        elif process.returncode != 0:
            reason, message = "process_failed", f"benchmark exited with code {process.returncode}"
    return {
        "reason_code": reason, "message": message,
        "returncode": process.returncode if process is not None else None,
        "wall_elapsed_ns": time.monotonic_ns() - started,
        "stdout": bytes(buffers[0]), "stderr": bytes(buffers[1]),
    }


def _persist_trial_outcome(directory, outcome):
    # summary.json is the existing authoritative ledger if this exclusive write
    # fails. Never replace an artifact or use an unreserved trial directory.
    outcome["outcome_persisted"] = True
    try:
        _write_json(directory / "outcome.json", outcome)
    except OSError:
        outcome.update(status="cancelled" if outcome["status"] == "cancelled" else "failed",
                       reason_code="artifact_error", message="could not persist trial outcome",
                       result=None, outcome_persisted=False)
    return outcome


def _execute_trial(trial, *, root, provenance, records, segments, generation,
                   timeout_seconds, cancel_event, skip_launch=False):
    directory = root / trial["artifact_dir"]
    reserved = False
    outcome = {**trial, "status": "failed", "reason_code": None,
               "message": "completed", "returncode": None, "wall_elapsed_ns": 0,
               "result": None, "outcome_persisted": False}
    try:
        directory.mkdir(mode=0o700)
        reserved = True
        if skip_launch:
            outcome.update(status="not_run", reason_code="cancelled",
                           message="not launched after cancellation")
        else:
            working_directory = root / trial["working_dir"]
            working_directory.mkdir(mode=0o700)
            if not _provenance_matches(provenance):
                raise ExecutionError("provenance_mismatch", "command files changed before trial")
            execution = _run_process(trial["argv"], cwd=working_directory,
                                     timeout_seconds=timeout_seconds, cancel_event=cancel_event)
            outcome.update({key: execution[key] for key in (
                "reason_code", "message", "returncode", "wall_elapsed_ns",
            )})
            for stream, filename in (("stdout", "stdout.jsonl"), ("stderr", "stderr.bin")):
                with (directory / filename).open("xb") as output:
                    output.write(execution[stream])
            if execution["reason_code"]:
                raise ExecutionError(execution["reason_code"], execution["message"])
            if not _provenance_matches(provenance):
                raise ExecutionError("provenance_mismatch", "command files changed during trial")
            outcome["result"] = validate_synthetic_result(
                execution["stdout"], arm=trial["arm"], records=records, segments=segments,
                generation=generation, trial=trial["trial"],
            )
            outcome["status"] = "ok"
    except ExecutionError as error:
        outcome.update(reason_code=error.reason_code, message=str(error))
        if error.reason_code == "cancelled":
            outcome["status"] = "cancelled"
    except ResultSchemaError as error:
        outcome.update(reason_code="invalid_result", message=str(error))
    except KeyboardInterrupt:
        outcome.update(status="cancelled", reason_code="cancelled", message="comparison interrupted")
    except (OSError, ConfigurationError):
        outcome.update(reason_code="artifact_or_provenance_error",
                       message="could not access trial artifacts or command files")
    return _persist_trial_outcome(directory, outcome) if reserved else outcome


def _statistics(values):
    return {"values": values, "median": statistics.median(values) if values else None,
            "min": min(values) if values else None, "max": max(values) if values else None}


def _summarize_groups(trials, *, arms, comparison_valid):
    groups = []
    for arm in arms:
        for variant in ("before", "after"):
            rows = [r for r in trials if r["arm"] == arm and r["variant"] == variant]
            group = {"arm": arm, "variant": variant, "expected_count": len(rows),
                     "successful_count": sum(r["status"] == "ok" for r in rows),
                     "trial_ids": [r["trial"] for r in rows], "metrics": None}
            if comparison_valid:
                metrics = {
                    "wall_elapsed_ns": _statistics([r["wall_elapsed_ns"] for r in rows]),
                    "lifecycle_elapsed_ns": _statistics([
                        r["result"]["metrics"]["lifecycle_stop_ns"]
                        - r["result"]["metrics"]["lifecycle_start_ns"] for r in rows
                    ]),
                }
                for name in (*_TIMING_METRICS, *_COUNTER_METRICS):
                    results = [r["result"]["metrics"] for r in rows]
                    values = [r.get(name) for r in results]
                    entry = _statistics(values if all(v is not None for v in values) else [])
                    entry["values"] = values  # Preserve absent samples, never substitute zero.
                    if name in _COUNTER_METRICS:
                        entry["available"] = [r[name + "_available"] for r in results]
                        entry["synthetic"] = [r[name + "_synthetic"] for r in results]
                        if len(set(entry["synthetic"])) != 1:
                            entry.update(median=None, min=None, max=None)
                    metrics[name] = entry
                group["metrics"] = metrics
            groups.append(group)
    return groups


def run_comparison(*, before, after, output_dir, arms=("process", "integrated"),
                   trial_count=4, records=16, segments=2, generation=None,
                   timeout_seconds=120, cancel_event=None, before_producer=None,
                   after_producer=None):
    """Run ABBA per arm, settle every outcome, suppress statistics on any failure.

    trial_count is the total number of before+after trials PER ARM, not repeats per
    variant. A fresh generation defaults to a random uint64; trial IDs span arms.
    A caller may supply a threading.Event for cooperative cancellation. Child cwd
    is work/ under each trial, separate from runner metadata (not an OS sandbox).
    plan.json stays immutable; summary.json records all terminal dispositions,
    including outcome_persisted=False when a trial artifact could not be written.
    Failure to persist the plan or final summary raises OSError, never success.
    """
    if type(trial_count) is not int or not 1 <= trial_count <= MAX_COMPARISON_TRIALS:
        raise ConfigurationError("trial count must be an integer within comparison bound")
    order = balanced_abba_order(("before", "after"), trial_count=trial_count)
    if not isinstance(arms, (list, tuple)) or not arms or any(
        arm not in ("process", "integrated") for arm in arms
    ) or len(set(arms)) != len(arms):
        raise ConfigurationError("request distinct process and/or integrated arms")
    if type(records) is not int or not 1 <= records <= MAX_RECORD_COUNT:
        raise ConfigurationError("records must be between 1 and 1000000")
    if type(segments) is not int or not 1 <= segments <= records:
        raise ConfigurationError("segments must be positive and cannot exceed records")
    if generation is None:
        generation = secrets.randbits(64)
    if type(generation) is not int or not 0 <= generation < 1 << 64:
        raise ConfigurationError("generation must be a uint64 integer")
    if isinstance(timeout_seconds, bool) or not isinstance(timeout_seconds, (int, float)) or not (
        math.isfinite(timeout_seconds) and 0 < timeout_seconds <= 3600
    ):
        raise ConfigurationError("timeout must be finite and between 0 and 3600 seconds")
    if output_dir is None:
        raise ConfigurationError("an explicit new output directory is required")
    try:
        root = pathlib.Path(output_dir).absolute()
        if root.exists() or root.is_symlink():
            raise ConfigurationError("output directory already exists; prior artifacts are never overwritten")
        provenance = {name: _command_provenance(command)
                      for name, command in (("before", before), ("after", after))}
        if "process" in arms:
            for name, producer in (("before", before_producer),
                                   ("after", after_producer)):
                provenance[name]["process_fixture_producer"] = (
                    _process_fixture_producer_provenance(provenance[name], producer)
                )
        root.mkdir(mode=0o700)  # Atomic reservation; no exist_ok and no recursive parent writes.
    except (OSError, TypeError, ValueError) as error:
        raise ConfigurationError(_bounded_diagnostic(error)) from error

    trials = []
    for arm in arms:
        for variant in order:
            trial = len(trials)
            argv = [*provenance[variant]["argv_prefix"], "--arm", arm,
                    "--records", str(records), "--segments", str(segments),
                    "--generation", str(generation), "--trial", str(trial)]
            trial_plan = {"trial": trial, "variant": variant, "arm": arm, "argv": argv,
                          "artifact_dir": f"trial-{trial:04d}-{variant}-{arm}",
                          "working_dir": f"trial-{trial:04d}-{variant}-{arm}/work",
                          "status": "not_run"}
            if arm == "process":
                trial_plan["process_fixture_producer"] = provenance[variant][
                    "process_fixture_producer"
                ]
            trials.append(trial_plan)
    summary = {
        "schema_version": 1, "benchmark": "synthetic-live-capture-comparison",
        "evidence_scope": "supplied-executable synthetic comparison, not fixed-tree acceptance",
        "coverage_origin": "current C++ CLI contract, not independent pipeline verification",
        "measurement_notes": {
            "trial_wall_elapsed_ns": "process launch through child/pipe cleanup; excludes fingerprinting and validation",
            "comparison_wall_elapsed_ns": "trial loop including validation; excludes initial planning and fingerprinting",
            "throughput_payload_bytes_per_second": "fixture payload bytes divided by first-byte to last-commit interval",
            "peak_rss_bytes": "C++ reported process peak, not sustained or process-tree RSS; missing is not zero",
        },
        "status": "not_run", "comparison_valid": False, "provenance": provenance,
        "performance_interpretation": {
            "scope": "whole supplied executables",
            "causal_attribution_available": False,
            "elapsed_time_is_gate": False,
            "policy": "retain every ordered trial and report observed trade-offs",
        },
        "records": records, "segments": segments, "generation": generation,
        "comparison_axis": "before-after-within-each-arm", "arm_order": list(arms),
        "forced_termination_note": "capture temp files may remain in the retained trial directory",
        "trials_per_arm": trial_count, "requested_duration_ms": None,
        "timeout_seconds": timeout_seconds,
        "coverage": {"synthetic_fixture": True, "controller_streaming_capture": True,
                     "real_device": False, "native_socket_parser": False,
                     "ui_search_save": False, "slow_sink": False, "heartbeat": False,
                     "sustained_rss": False, "device_losslessness": False,
                     "source_tree_verified": False, "dependency_hashes_verified": False,
                     "posix_process_group_cleanup": os.name == "posix"},
        "trials": trials,
    }
    _write_json(root / "plan.json", summary)
    started = time.monotonic_ns()
    cancelled = False
    for index, trial in enumerate(trials):
        cancelled = cancelled or (cancel_event is not None and cancel_event.is_set())
        trials[index] = _execute_trial(
            trial, root=root, provenance=provenance[trial["variant"]],
            records=records, segments=segments, generation=generation,
            timeout_seconds=timeout_seconds, cancel_event=cancel_event,
            skip_launch=cancelled,
        )
        cancelled = cancelled or trials[index]["status"] == "cancelled"
    valid = all(r["status"] == "ok" for r in trials)
    summary.update(status="cancelled" if cancelled else ("ok" if valid else "failed"),
                   comparison_valid=valid, wall_elapsed_ns=time.monotonic_ns() - started,
                   groups=_summarize_groups(trials, arms=arms, comparison_valid=valid))
    _write_json(root / "summary.json", summary)
    return summary


class _ContractArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        raise ConfigurationError(f"argument error: {message}")


def _argument_parser():
    parser = _ContractArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--dry-run", action="store_true")
    mode.add_argument("--list-only", action="store_true")
    parser.add_argument("--enable-real-device", action="store_true")
    parser.add_argument("--udid")
    parser.add_argument("--native-stack-root", type=pathlib.Path)
    parser.add_argument("--before", nargs="+", metavar="ARGV", help="before executable and positional prefix arguments")
    parser.add_argument("--after", nargs="+", metavar="ARGV", help="after executable and positional prefix arguments")
    parser.add_argument("--before-producer", type=pathlib.Path,
                        help="absolute fixture producer embedded in the before process benchmark")
    parser.add_argument("--after-producer", type=pathlib.Path,
                        help="absolute fixture producer embedded in the after process benchmark")
    parser.add_argument("--output-dir", type=pathlib.Path, help="required NEW directory; never overwrite")
    parser.add_argument("--arm", choices=("process", "integrated", "both"), default="both")
    parser.add_argument("--trials", type=int, default=4, help="total ABBA trials per arm, positive multiple of four")
    parser.add_argument("--records", type=int, default=16)
    parser.add_argument("--segments", type=int, default=2)
    parser.add_argument("--generation", type=int, help="uint64 fixture identity; default fresh random value")
    parser.add_argument("--timeout-seconds", type=float, default=120, help="per-process deadline, NOT capture duration")
    return parser


def _emit_result(output, result):
    output.write(serialize_result(result))
    return result_exit_code(result)


def _configuration_failure(message):
    return build_result(
        benchmark="live-capture-orchestration",
        status="failed",
        reason_code="configuration_error",
        message=_bounded_diagnostic(message),
        metrics={},
    )


def run_cli(argv=None, *, transport_factory=None, output=None):
    """Run synthetic comparisons; retain the never-construct-device transport seam."""

    if output is None:
        import sys

        output = sys.stdout

    # Real-device transport construction is intentionally not implemented here.
    _ = transport_factory

    try:
        args = _argument_parser().parse_args(argv)
    except ConfigurationError as error:
        return _emit_result(output, _configuration_failure(str(error)))

    if args.list_only:
        output.write(json.dumps({"benchmarks": list(AVAILABLE_BENCHMARKS)}, sort_keys=True))
        output.write("\n")
        return 0

    try:
        options = validate_real_device_options(
            enable_real_device=args.enable_real_device,
            udid=args.udid,
            native_stack_root=args.native_stack_root,
        )
    except ConfigurationError as error:
        return _emit_result(output, _configuration_failure(str(error)))

    if args.dry_run:
        return _emit_result(
            output,
            build_result(
                benchmark=AVAILABLE_BENCHMARKS[0],
                status="not_run",
                reason_code="disabled",
                message="dry run; no transport constructed",
                metrics={"real_device_enabled": int(options is not None)},
            ),
        )

    if args.before is not None or args.after is not None or args.output_dir is not None:
        try:
            if options is not None:
                raise ConfigurationError("synthetic comparison cannot use real-device options")
            summary = run_comparison(
                before=args.before, after=args.after, output_dir=args.output_dir,
                arms=("process", "integrated") if args.arm == "both" else (args.arm,),
                trial_count=args.trials, records=args.records, segments=args.segments,
                generation=args.generation, timeout_seconds=args.timeout_seconds,
                before_producer=args.before_producer, after_producer=args.after_producer,
            )
        except ConfigurationError as error:
            return _emit_result(output, _configuration_failure(str(error)))
        except OSError:
            return _emit_result(output, build_result(
                benchmark="synthetic-live-capture-comparison", status="failed",
                reason_code="artifact_error",
                message="fatal artifact error; final summary was not persisted",
                metrics={},
            ))
        except KeyboardInterrupt:
            return _emit_result(output, build_result(
                benchmark="synthetic-live-capture-comparison", status="failed",
                reason_code="cancelled", message="benchmark comparison interrupted",
                metrics={},
            ))
        ok = summary["comparison_valid"]
        return _emit_result(output, build_result(
            benchmark="synthetic-live-capture-comparison", status="ok" if ok else "failed",
            reason_code=None if ok else ("cancelled" if summary["status"] == "cancelled" else "comparison_failed"),
            message="synthetic comparison completed; see summary.json" if ok
                    else "comparison incomplete or failed; see every trial outcome in summary.json",
            metrics={"planned_trials": len(summary["trials"]),
                     "successful_trials": sum(r["status"] == "ok" for r in summary["trials"]),
                     "wall_elapsed_ns": summary["wall_elapsed_ns"]},
        ))

    return _emit_result(
        output,
        build_result(
            benchmark=AVAILABLE_BENCHMARKS[0],
            status="not_run",
            reason_code="unavailable",
            message="real-device orchestration is unavailable; synthetic comparisons require --before, --after and --output-dir",
            metrics={},
        ),
    )


def main():
    return run_cli()


if __name__ == "__main__":
    raise SystemExit(main())
