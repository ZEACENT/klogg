#!/usr/bin/env python3
"""Deterministic contract helpers for live-capture benchmark orchestration.

The first implementation deliberately contains no device or process transport.  It
validates benchmark inputs and results, and exposes dry-run/list-only CLI paths
that cannot construct a transport.
"""

import argparse
import json
import math
import pathlib
import re
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

AVAILABLE_BENCHMARKS = ("ios-usb-live-capture",)


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
    if result.get("schema_version") != RESULT_SCHEMA_VERSION:
        raise ResultSchemaError(
            f"unsupported result schema version {result.get('schema_version')!r}"
        )

    benchmark = result.get("benchmark")
    if not isinstance(benchmark, str) or _SAFE_IDENTIFIER.fullmatch(benchmark) is None:
        raise ResultSchemaError("benchmark must be a bounded identifier")

    status = result.get("status")
    if status not in _RESULT_STATUSES:
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
    return parser


def _emit_result(output, result):
    output.write(serialize_result(result))
    return result_exit_code(result)


def _configuration_failure(message):
    return build_result(
        benchmark=AVAILABLE_BENCHMARKS[0],
        status="failed",
        reason_code="configuration_error",
        message=message,
        metrics={},
    )


def run_cli(argv=None, *, transport_factory=None, output=None):
    """Run non-transport CLI modes; execution remains unavailable in Cycle 1."""

    if output is None:
        import sys

        output = sys.stdout

    # Retain the explicit seam without constructing anything in this cycle.
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

    return _emit_result(
        output,
        build_result(
            benchmark=AVAILABLE_BENCHMARKS[0],
            status="not_run",
            reason_code="unavailable",
            message="benchmark transport execution is not implemented",
            metrics={},
        ),
    )


def main():
    return run_cli()


if __name__ == "__main__":
    raise SystemExit(main())
