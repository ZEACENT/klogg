#!/usr/bin/env python3
"""Fail-closed verification for source-built ADB helper artifacts and packages."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import re
import stat

from verify_adb_helper_envelope import EnvelopeError, verify_checksum_file


BINARY_BUILD_RECEIPT = "binary-build"
BINARY_SMOKE_RECEIPT = "binary-smoke"
PACKAGE_VERIFICATION_RECEIPT = "package-verification"
REQUIRED_SMOKE_PROBES = [
    "version",
    "complete-client",
    "loopback-private-server",
    "smart-socket-host-version",
    "no-lingering-process",
]
PRODUCTION_SERVER_ARGUMENTS = ["server", "nodaemon"]
PRODUCTION_SERVER_SCRUBBED_ENVIRONMENT = [
    "ADB_VENDOR_KEYS",
    "ADB_SERVER_PORT",
    "ANDROID_ADB_SERVER_PORT",
    "ANDROID_ADB_SERVER_ADDRESS",
]
REQUIRED_SERVER_STABILITY_WINDOW_SECONDS = 0.25
# Stable normalized identifiers used by receipt-policy tooling.
binary_build = BINARY_BUILD_RECEIPT
binary_smoke = BINARY_SMOKE_RECEIPT
package_verification = PACKAGE_VERIFICATION_RECEIPT


class VerificationError(RuntimeError):
    pass


def has_exact_schema(document: object, expected: int) -> bool:
    return (
        isinstance(document, dict)
        and type(document.get("schema_version")) is int
        and document["schema_version"] == expected
    )


def read_json(path: pathlib.Path, label: str) -> dict:
    if not path.is_file():
        raise VerificationError(f"missing {label}: {path}")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise VerificationError(f"invalid {label} JSON: {error}") from error
    if not isinstance(document, dict):
        raise VerificationError(f"invalid {label}: root must be an object")
    return document


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_relative(value: str, label: str) -> pathlib.PurePosixPath:
    relative = pathlib.PurePosixPath(value.replace("\\", "/"))
    if relative.is_absolute() or ".." in relative.parts or not relative.parts:
        raise VerificationError(f"unsafe {label} path: {value}")
    return relative


def require_regular_file(path: pathlib.Path, label: str) -> None:
    if not path.exists():
        raise VerificationError(f"missing {label}: {path}")
    if path.is_symlink():
        raise VerificationError(f"{label} must not be a symlink; sha256 identity is not trusted: {path}")
    if not stat.S_ISREG(path.stat().st_mode):
        raise VerificationError(f"{label} is not a regular file: {path}")


def require_regular_executable(path: pathlib.Path) -> None:
    require_regular_file(path, "ADB helper")
    if os.name != "nt" and path.stat().st_mode & 0o111 == 0:
        raise VerificationError(f"ADB helper is not executable: {path}")


def find_receipt_asset(receipt: dict, kind: str) -> dict:
    matches = [
        asset
        for asset in receipt.get("release_assets", [])
        if isinstance(asset, dict) and asset.get("kind") == kind
    ]
    if len(matches) != 1:
        raise VerificationError(
            f"release asset receipt coverage must contain exactly one {kind} entry; got {len(matches)}"
        )
    return matches[0]


def dotted_version(value: object, label: str) -> tuple[int, ...]:
    if not isinstance(value, str) or not value or any(
        not part.isdigit() for part in value.split(".")
    ):
        raise VerificationError(f"invalid {label}: {value}")
    return tuple(int(part) for part in value.split("."))


def verify_hash_sidecar(path: pathlib.Path, expected_hash: str, expected_name: str) -> None:
    require_regular_file(path, "release asset sha256 sidecar")
    fields = path.read_text(encoding="utf-8").strip().split()
    if fields != [expected_hash, expected_name]:
        raise VerificationError(
            f"release asset sha256 sidecar mismatch: expected {expected_hash}  {expected_name}, got {path.read_text(encoding='utf-8').strip()}"
        )


def validate_production_server_probe(smoke_receipt: dict) -> None:
    if not has_exact_schema(smoke_receipt, 1):
        raise VerificationError("ADB helper binary-smoke receipt has an unsupported schema")
    if any(field in smoke_receipt for field in ("error", "cleanup_error")):
        raise VerificationError("ADB helper binary-smoke receipt records a smoke or cleanup failure")
    if smoke_receipt.get("passed_probes") != REQUIRED_SMOKE_PROBES:
        raise VerificationError("ADB helper binary-smoke receipt lacks the complete ordered probe set")

    endpoint = smoke_receipt.get("server_endpoint")
    if not isinstance(endpoint, str):
        raise VerificationError("ADB helper binary-smoke receipt lacks a server endpoint probe")
    endpoint_match = re.fullmatch(r"tcp:127\.0\.0\.1:([1-9][0-9]{0,4})", endpoint)
    if endpoint_match is None:
        raise VerificationError("ADB helper binary-smoke server endpoint probe is malformed")
    port = int(endpoint_match.group(1))
    if port > 65535:
        raise VerificationError("ADB helper binary-smoke server endpoint probe has an invalid port")

    expected_probe = {
        "name": "production-server-invocation",
        "arguments": PRODUCTION_SERVER_ARGUMENTS,
        "environment": {"ADB_SERVER_SOCKET": f"tcp:{port}"},
        "scrubbed_environment": PRODUCTION_SERVER_SCRUBBED_ENVIRONMENT,
    }
    if smoke_receipt.get("required_probe") != expected_probe:
        raise VerificationError(
            "ADB helper binary-smoke receipt lacks the exact production server invocation probe"
        )

    host_version = smoke_receipt.get("host_version")
    if (
        not isinstance(host_version, str)
        or re.fullmatch(r"[0-9a-fA-F]+", host_version) is None
        or smoke_receipt.get("stability_host_version") != host_version
    ):
        raise VerificationError(
            "ADB helper binary-smoke receipt lacks a stable repeated host:version probe"
        )
    stability_window = smoke_receipt.get("stability_window_seconds")
    if (
        type(stability_window) not in (int, float)
        or not math.isfinite(stability_window)
        or stability_window < REQUIRED_SERVER_STABILITY_WINDOW_SECONDS
    ):
        raise VerificationError(
            "ADB helper binary-smoke receipt lacks the required server stability probe window"
        )


def asset_required_for_scope(asset: dict, scope: str | None) -> bool:
    if asset.get("required") is not True:
        return False
    if scope is None:
        return True
    distribution = asset.get("distribution")
    if not isinstance(distribution, dict):
        raise VerificationError(
            f"ADB release asset lacks package/release distribution: {asset.get('kind')}"
        )
    return distribution.get(f"{scope}_required") is True


def validate_source_set_receipt(
    lock: dict,
    lock_path: pathlib.Path,
    build_receipt: dict,
    source_assets_root: pathlib.Path,
    scope: str | None,
) -> str | None:
    source_assets = [
        asset
        for asset in lock.get("release_assets", [])
        if isinstance(asset, dict) and asset.get("kind") == "source-set-receipt"
    ]
    if not source_assets:
        if scope is not None:
            raise VerificationError("ADB lock lacks the required source-set receipt asset")
        return None
    if len(source_assets) != 1:
        raise VerificationError("ADB lock must declare exactly one source-set receipt asset")
    locked_asset = source_assets[0]
    receipt_path = source_assets_root / safe_relative(
        str(locked_asset.get("file_name", "")), "ADB source-set receipt"
    )
    source_set = read_json(receipt_path, "ADB source-set receipt")
    if (
        not has_exact_schema(source_set, 1)
        or source_set.get("receipt_kind") != "component-source-set"
        or source_set.get("component") != "adb-helper"
        or source_set.get("lock_sha256") != sha256(lock_path)
    ):
        raise VerificationError("invalid or stale ADB component source-set receipt")
    actual_receipt_hash = sha256(receipt_path)
    if build_receipt.get("source_set_receipt_sha256") != actual_receipt_hash:
        raise VerificationError("ADB binary-build receipt source-set sha256 mismatch")

    identity = source_set.get("source_identity")
    if not isinstance(identity, dict):
        raise VerificationError("ADB source-set receipt lacks source identity")
    for field in ("manifest_or_closure_sha256", "final_tree_sha256"):
        if not isinstance(identity.get(field), str) or re.fullmatch(
            r"[0-9a-f]{64}", identity[field]
        ) is None:
            raise VerificationError(f"ADB source-set receipt has invalid {field}")
    if identity.get("tree_hash_algorithm") != "sha256":
        raise VerificationError("ADB source-set receipt uses an unsupported tree hash algorithm")
    if re.fullmatch(r"[0-9a-f]{64}", str(source_set.get("patch_chain_sha256", ""))) is None:
        raise VerificationError("ADB source-set receipt has invalid patch chain sha256")
    if source_set.get("distribution") != {
        "package_required": False,
        "release_required": True,
    }:
        raise VerificationError("ADB source-set receipt has invalid archive distribution")

    expected_support = {
        asset["kind"]: asset
        for asset in lock.get("release_assets", [])
        if isinstance(asset, dict)
        and asset.get("kind") != "source-set-receipt"
        and isinstance(asset.get("distribution"), dict)
        and asset["distribution"].get("package_required") is True
    }
    support_records = source_set.get("package_support_assets")
    if not isinstance(support_records, list):
        raise VerificationError("ADB source-set receipt package support assets must be an array")
    support_by_kind = {
        item.get("kind"): item for item in support_records if isinstance(item, dict)
    }
    if set(support_by_kind) != set(expected_support) or len(support_by_kind) != len(
        support_records
    ):
        raise VerificationError("ADB source-set package support asset coverage mismatch")
    for kind, locked in expected_support.items():
        record = support_by_kind[kind]
        if record.get("file_name") != locked.get("file_name"):
            raise VerificationError(f"ADB source-set package support path mismatch: {kind}")
        path = source_assets_root / safe_relative(record["file_name"], f"{kind} support asset")
        require_regular_file(path, f"ADB package support asset {kind}")
        if record.get("sha256") != sha256(path):
            raise VerificationError(f"ADB package support asset sha256 mismatch: {kind}")

    archive = source_set.get("archive")
    archive_assets = [
        asset
        for asset in lock.get("release_assets", [])
        if isinstance(asset, dict) and asset.get("kind") == "source-archive"
    ]
    if len(archive_assets) != 1 or not isinstance(archive, dict):
        raise VerificationError("ADB source-set receipt lacks one locked source archive")
    locked_archive = archive_assets[0]
    if archive.get("file_name") != locked_archive.get("file_name") or re.fullmatch(
        r"[0-9a-f]{64}", str(archive.get("sha256", ""))
    ) is None:
        raise VerificationError("ADB source-set archive identity mismatch")
    archive_path = source_assets_root / safe_relative(
        archive["file_name"], "ADB corresponding source archive"
    )
    archive_sidecar = source_assets_root / safe_relative(
        str(locked_archive.get("sha256_file", archive["file_name"] + ".sha256")),
        "ADB corresponding source archive checksum",
    )
    if scope == "package":
        if archive_path.exists() or archive_path.is_symlink() or archive_sidecar.exists():
            raise VerificationError(
                "ADB package must not contain the corresponding source archive or checksum"
            )
    else:
        require_regular_file(archive_path, "ADB corresponding source archive")
        if sha256(archive_path) != archive["sha256"]:
            raise VerificationError("ADB corresponding source archive sha256 mismatch")
    return actual_receipt_hash


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--receipt", required=True, type=pathlib.Path)
    parser.add_argument("--package-root", required=True, type=pathlib.Path)
    parser.add_argument("--release-root", type=pathlib.Path)
    parser.add_argument("--asset-scope", choices=("package", "release"))
    parser.add_argument("--source-assets-root", type=pathlib.Path)
    parser.add_argument("--layout")
    parser.add_argument("--helper-path")
    parser.add_argument("--expected-target")
    parser.add_argument("--maximum-glibc-version")
    parser.add_argument("--binary-smoke-receipt", type=pathlib.Path)
    parser.add_argument("--package-target")
    parser.add_argument("--package-verification-receipt", type=pathlib.Path)
    parser.add_argument("--checksum-envelope", type=pathlib.Path)
    parser.add_argument("--signing-receipt", type=pathlib.Path)
    parser.add_argument("--notarization-receipt", type=pathlib.Path)
    parser.add_argument("--package-file", type=pathlib.Path, action="append", default=[])
    parser.add_argument("--require-lock-binding", action="store_true")
    args = parser.parse_args()

    try:
        lock = read_json(args.lock, "ADB helper lock")
        receipt = read_json(args.receipt, "ADB helper receipt")
        if not has_exact_schema(lock, 2) or not has_exact_schema(receipt, 1):
            raise VerificationError("unsupported ADB helper lock or receipt schema")
        source_assets_root = args.source_assets_root or args.release_root
        if source_assets_root is None:
            raise VerificationError(
                "ADB verification requires --source-assets-root or legacy --release-root"
            )
        if args.asset_scope is None and args.release_root is None:
            raise VerificationError("legacy ADB asset verification requires --release-root")
        if args.checksum_envelope is not None:
            try:
                verify_checksum_file(args.checksum_envelope)
            except EnvelopeError as error:
                raise VerificationError(f"checksum envelope verification failed: {error}") from error
        if receipt.get("receipt_kind", BINARY_BUILD_RECEIPT) != BINARY_BUILD_RECEIPT:
            raise VerificationError("ADB helper receipt is not binary-build evidence")

        smoke_receipt = None
        smoke_required = (
            args.asset_scope is not None
            or args.package_target is not None
            or args.package_verification_receipt is not None
        )
        if smoke_required and args.binary_smoke_receipt is None:
            raise VerificationError(
                "ADB package/release verification requires binary-smoke evidence"
            )
        if args.binary_smoke_receipt is not None:
            smoke_receipt = read_json(args.binary_smoke_receipt, "ADB helper binary-smoke receipt")
            if smoke_receipt.get("receipt_kind", BINARY_SMOKE_RECEIPT) != BINARY_SMOKE_RECEIPT:
                raise VerificationError("ADB helper smoke receipt is not binary-smoke evidence")
            validate_production_server_probe(smoke_receipt)

        if args.require_lock_binding:
            actual_lock_hash = sha256(args.lock)
            if receipt.get("lock_sha256") != actual_lock_hash:
                raise VerificationError(
                    f"receipt lock sha256 mismatch: expected {actual_lock_hash}, got {receipt.get('lock_sha256')}"
                )

        target = receipt.get("target")
        if args.expected_target and target != args.expected_target:
            raise VerificationError(
                f"ADB helper target mismatch: expected {args.expected_target}, receipt contains {target}"
            )
        target_plan = lock.get("targets", {}).get(target)
        if not isinstance(target_plan, dict):
            raise VerificationError(f"receipt names an unlocked ADB helper target: {target}")
        if target_plan.get("qualified") is not True:
            raise VerificationError(f"ADB helper target is not release-qualified: {target}")

        layout = args.layout or receipt.get("layout")
        if args.helper_path:
            helper_relative = args.helper_path
        elif args.layout:
            helper_relative = lock.get("install_paths", {}).get(layout)
        else:
            helper_relative = receipt.get("helper", {}).get("path")
        if not isinstance(helper_relative, str):
            raise VerificationError(f"missing locked ADB helper path for layout: {layout}")
        helper_path = args.package_root / safe_relative(helper_relative, "ADB helper")
        require_regular_executable(helper_path)

        helper_receipt = receipt.get("helper")
        if not isinstance(helper_receipt, dict):
            raise VerificationError("receipt lacks ADB helper identity")
        if helper_receipt.get("kind") != lock.get("helper", {}).get("kind"):
            raise VerificationError("receipt does not identify a complete ADB executable")
        source_helper_hash = helper_receipt.get("sha256")
        actual_helper_hash = sha256(helper_path)

        package_records = []
        for package_file in args.package_file:
            require_regular_file(package_file, "qualified package")
            package_records.append(
                {"name": package_file.name, "sha256": sha256(package_file)}
            )
        package_hash = package_records[0]["sha256"] if len(package_records) == 1 else None

        signing_receipt = None
        if args.signing_receipt is not None:
            signing_receipt = read_json(args.signing_receipt, "ADB helper signing receipt")
            if (
                signing_receipt.get("receipt_kind") != "signing"
                or signing_receipt.get("status") != "passed"
                or signing_receipt.get("target") != target
            ):
                raise VerificationError("ADB helper signing receipt is not passed target-bound evidence")
            if signing_receipt.get("source_helper_sha256") != source_helper_hash:
                raise VerificationError("ADB helper signing receipt is not bound to the source-built helper")
            if signing_receipt.get("signed_helper_sha256") != actual_helper_hash:
                raise VerificationError("ADB helper signing receipt is not bound to the signed helper")
            signed_package_hash = signing_receipt.get(
                "package_sha256", signing_receipt.get("dmg_sha256")
            )
            if package_hash is None or signed_package_hash != package_hash:
                raise VerificationError("ADB helper signing receipt is not bound to the qualified package")
        elif actual_helper_hash != source_helper_hash:
            raise VerificationError(
                f"ADB helper sha256 mismatch: expected {source_helper_hash}, got {actual_helper_hash}"
            )

        notarization_receipt = None
        if args.notarization_receipt is not None:
            notarization_receipt = read_json(
                args.notarization_receipt, "ADB helper notarization receipt"
            )
            if (
                notarization_receipt.get("receipt_kind") != "notarization"
                or notarization_receipt.get("status") != "passed"
                or notarization_receipt.get("target") != target
                or not notarization_receipt.get("submission_id")
            ):
                raise VerificationError(
                    "ADB helper notarization receipt is not accepted target-bound evidence"
                )
            if notarization_receipt.get("signed_helper_sha256") != actual_helper_hash:
                raise VerificationError("ADB helper notarization receipt is not bound to the signed helper")
            notarized_package_hash = notarization_receipt.get(
                "package_sha256", notarization_receipt.get("dmg_sha256")
            )
            if package_hash is None or notarized_package_hash != package_hash:
                raise VerificationError("ADB helper notarization receipt is not bound to the qualified package")

        if smoke_receipt is not None and smoke_receipt.get("helper_sha256") != actual_helper_hash:
            raise VerificationError(
                "ADB helper binary-smoke receipt is not bound to the verified helper sha256"
            )

        binary = receipt.get("binary_verification")
        if not has_exact_schema(binary, 2):
            raise VerificationError("receipt lacks supported binary verification")
        architectures = binary.get("architectures", [])
        expected_architectures = [target_plan.get("arch")]
        if architectures != expected_architectures:
            raise VerificationError(
                f"ADB helper architecture mismatch: expected {expected_architectures}, got {architectures}"
            )
        expected_deployment = target_plan.get("deployment_target")
        if expected_deployment and binary.get("deployment_target") != expected_deployment:
            raise VerificationError(
                f"ADB helper deployment target mismatch: expected {expected_deployment}, got {binary.get('deployment_target')}"
            )
        locked_glibc = target_plan.get("glibc_baseline")
        if locked_glibc is not None:
            actual_glibc = binary.get("glibc_maximum_required")
            if dotted_version(actual_glibc, "ADB helper GLIBC requirement") > dotted_version(
                locked_glibc, "locked GLIBC baseline"
            ):
                raise VerificationError(
                    f"ADB helper requires GLIBC_{actual_glibc}, newer than locked baseline GLIBC_{locked_glibc}"
                )
            if args.maximum_glibc_version and dotted_version(
                actual_glibc, "ADB helper GLIBC requirement"
            ) > dotted_version(args.maximum_glibc_version, "package GLIBC maximum"):
                raise VerificationError(
                    f"ADB helper requires GLIBC_{actual_glibc}, newer than package maximum GLIBC_{args.maximum_glibc_version}"
                )
        elif args.maximum_glibc_version:
            raise VerificationError("package GLIBC maximum was specified for a non-GLIBC target")
        for probe in ("version_probe", "complete_client_probe"):
            if binary.get(probe) != "passed":
                raise VerificationError(f"ADB helper {probe.replace('_', ' ')} did not pass")

        usb = target_plan.get("usb", {})
        imports = [str(item).lower() for item in binary.get("dynamic_imports", [])]
        imports_by_binary = binary.get("imports_by_binary", {})
        windows_system_imports = binary.get(
            "windows_system_imports_by_binary", {}
        )
        runtime_loads = [str(item).lower() for item in binary.get("runtime_loads", [])]
        expected_runtime = usb.get("runtime_files", [])
        runtime_receipt = binary.get("runtime_closure", [])
        expected_delayed_loads = usb.get("required_delayed_runtime_loads", [])
        delayed_load_evidence = binary.get("runtime_load_evidence", [])
        observed_runtime_edges = binary.get("observed_runtime_edges", [])
        if not isinstance(expected_runtime, list) or not isinstance(runtime_receipt, list):
            raise VerificationError("ADB helper runtime closure metadata must be arrays")
        if not isinstance(imports_by_binary, dict) or not imports_by_binary:
            raise VerificationError("ADB helper receipt lacks per-binary import evidence")
        expected_import_binaries = {helper_path.name}
        if str(target).startswith("windows-"):
            expected_import_binaries.update(expected_runtime)
        if set(imports_by_binary) != expected_import_binaries:
            raise VerificationError("ADB helper per-binary import evidence is incomplete")
        if any(
            not isinstance(binary_imports, list)
            or not all(isinstance(item, str) for item in binary_imports)
            for binary_imports in imports_by_binary.values()
        ):
            raise VerificationError("ADB helper per-binary imports must be string arrays")
        if {
            str(item).lower() for item in imports_by_binary[helper_path.name]
        } != set(imports):
            raise VerificationError("ADB helper direct import evidence is contradictory")
        if usb.get("backend") == "dynamic-libusb":
            expected_private_imports = usb.get("required_private_imports_by_binary")
            if not isinstance(expected_private_imports, dict) or set(
                expected_private_imports
            ) != expected_import_binaries:
                raise VerificationError("ADB helper lock lacks a complete private import graph")
            runtime_names = {str(item).lower() for item in expected_runtime}
            for binary_name, expected_binary_imports in expected_private_imports.items():
                if not isinstance(expected_binary_imports, list) or not all(
                    isinstance(item, str) for item in expected_binary_imports
                ):
                    raise VerificationError("ADB helper private import graph must contain arrays")
                actual_private_imports = {
                    str(item).lower()
                    for item in imports_by_binary[binary_name]
                    if str(item).lower() in runtime_names
                }
                if actual_private_imports != {
                    str(item).lower() for item in expected_binary_imports
                }:
                    raise VerificationError(
                        f"ADB helper private imports mismatch for {binary_name}"
                    )
            if str(target).startswith("windows-"):
                allowed_system_imports = usb.get(
                    "allowed_system_imports_by_binary"
                )
                if not isinstance(allowed_system_imports, dict) or set(
                    allowed_system_imports
                ) != expected_import_binaries:
                    raise VerificationError(
                        "ADB helper lock lacks a complete Windows system import policy"
                    )
                if any(
                    not isinstance(imports, list)
                    or not all(isinstance(item, str) for item in imports)
                    for imports in allowed_system_imports.values()
                ):
                    raise VerificationError(
                        "Windows system import policy must contain arrays"
                    )
                if not isinstance(windows_system_imports, dict) or set(
                    windows_system_imports
                ) != expected_import_binaries:
                    raise VerificationError(
                        "Windows ADB receipt lacks complete system import evidence"
                    )
                for binary_name, system_imports in windows_system_imports.items():
                    if not isinstance(system_imports, list) or not all(
                        isinstance(item, str) for item in system_imports
                    ):
                        raise VerificationError(
                            "Windows ADB system import evidence must contain arrays"
                        )
                    recorded_system_imports = {
                        str(item).lower() for item in system_imports
                    }
                    allowed_names = {
                        str(item).lower()
                        for item in allowed_system_imports[binary_name]
                    }
                    if not recorded_system_imports.issubset(allowed_names):
                        raise VerificationError(
                            f"Windows ADB system import evidence is not locked for {binary_name}"
                        )
                    actual_system_imports = {
                        str(item).lower()
                        for item in imports_by_binary[binary_name]
                        if str(item).lower() not in runtime_names
                    }
                    if actual_system_imports != recorded_system_imports:
                        raise VerificationError(
                            f"Windows ADB system import evidence mismatch for {binary_name}"
                        )
        if not isinstance(observed_runtime_edges, list) or any(
            not isinstance(edge, dict) for edge in observed_runtime_edges
        ):
            raise VerificationError("ADB helper observed runtime edges must be objects")
        runtime_by_name = {}
        for entry in runtime_receipt:
            if not isinstance(entry, dict):
                raise VerificationError("ADB helper runtime closure contains a non-object entry")
            name = entry.get("name")
            relative = safe_relative(str(name or ""), "ADB helper runtime")
            if len(relative.parts) != 1 or relative.name != name:
                raise VerificationError(f"ADB helper runtime must be a sibling file: {name}")
            if name in runtime_by_name:
                raise VerificationError(f"duplicate ADB helper runtime closure entry: {name}")
            runtime_by_name[name] = entry
        if set(runtime_by_name) != set(expected_runtime):
            raise VerificationError(
                f"ADB helper runtime closure mismatch: expected {expected_runtime}, got {sorted(runtime_by_name)}"
            )
        if str(target).startswith("windows-") and set(runtime_loads) != {
            str(item).lower() for item in expected_runtime
        }:
            raise VerificationError("Windows ADB runtime load probe did not cover the complete private closure")
        if not isinstance(expected_delayed_loads, list) or not isinstance(
            delayed_load_evidence, list
        ) or not isinstance(observed_runtime_edges, list):
            raise VerificationError("ADB helper delayed runtime load metadata must be arrays")

        delayed_by_key = {}
        for expected_load in expected_delayed_loads:
            if not isinstance(expected_load, dict):
                raise VerificationError("ADB helper delayed runtime lock contains a non-object")
            key = (expected_load.get("loaded_by"), expected_load.get("runtime_file"))
            if key in delayed_by_key:
                raise VerificationError("duplicate locked ADB delayed runtime edge")
            delayed_by_key[key] = expected_load
        direct_runtime = set(usb.get("required_imports", []))
        delayed_runtime = {str(key[1]) for key in delayed_by_key}
        if direct_runtime & delayed_runtime or direct_runtime | delayed_runtime != set(expected_runtime):
            raise VerificationError(
                "ADB helper direct and delayed runtime edges must partition the private closure"
            )

        evidence_by_key = {}
        for actual_load in delayed_load_evidence:
            if not isinstance(actual_load, dict):
                raise VerificationError("ADB helper delayed runtime load evidence contains a non-object")
            key = (actual_load.get("loaded_by"), actual_load.get("runtime_file"))
            if key in evidence_by_key:
                raise VerificationError("duplicate ADB delayed runtime load evidence")
            evidence_by_key[key] = actual_load
        if set(evidence_by_key) != set(delayed_by_key):
            raise VerificationError("ADB helper delayed runtime load evidence is incomplete")
        for key, expected_load in delayed_by_key.items():
            actual_load = evidence_by_key[key]
            if re.fullmatch(
                r"[0-9a-f]{64}", str(expected_load.get("source_sha256", ""))
            ) is None:
                raise VerificationError("ADB delayed runtime lock has invalid source sha256")
            for field in (
                "runtime_file",
                "loaded_by",
                "source",
                "expression",
                "source_sha256",
                "loader_symbol",
            ):
                if actual_load.get(field) != expected_load.get(field):
                    raise VerificationError(
                        f"ADB helper delayed runtime load evidence mismatches locked {field}"
                    )
            if actual_load.get("runtime_name_encoding") != "utf-16le":
                raise VerificationError("ADB helper delayed runtime name lacks binary evidence")
            loader_imports = {
                str(item).lower() for item in imports_by_binary.get(str(key[0]), [])
            }
            if str(expected_load["runtime_file"]).lower() in loader_imports:
                raise VerificationError("ADB delayed runtime sidecar became a direct PE import")
        expected_observed_edges = [
            {"loaded_by": str(key[0]), "runtime_file": str(key[1])}
            for key in sorted(delayed_by_key)
        ]
        if sorted(observed_runtime_edges, key=lambda item: (item.get("loaded_by"), item.get("runtime_file"))) != expected_observed_edges:
            raise VerificationError("ADB helper delayed runtime edge probe did not pass")
        for name in expected_runtime:
            runtime = helper_path.parent / name
            require_regular_file(runtime, f"ADB helper runtime {name}")
            actual_runtime_hash = sha256(runtime)
            if actual_runtime_hash != runtime_by_name[name].get("sha256"):
                raise VerificationError(
                    f"ADB helper runtime sha256 mismatch ({name}): expected {runtime_by_name[name].get('sha256')}, got {actual_runtime_hash}"
                )

        closure_imports = [
            str(imported)
            for binary_imports in imports_by_binary.values()
            for imported in binary_imports
        ]
        import_names = {
            pathlib.PurePosixPath(imported.replace("\\", "/")).name.lower()
            for imported in closure_imports
        }
        forbidden_imports = []
        for forbidden in usb.get("forbidden_imports", []):
            normalized = str(forbidden).lower()
            if normalized.endswith(".dll"):
                matched = normalized in import_names
            else:
                matched = any(normalized in imported for imported in import_names)
            if matched:
                forbidden_imports.append(str(forbidden))
        if forbidden_imports:
            raise VerificationError(
                "ADB helper imports forbidden runtime material: "
                + ", ".join(forbidden_imports)
            )

        if usb.get("backend") == "dynamic-libusb":
            direct_import_names = {
                pathlib.PurePosixPath(imported.replace("\\", "/")).name.lower()
                for imported in imports
            }
            runtime_names = {str(item).lower() for item in expected_runtime}
            expected_direct_names = {
                str(item).lower() for item in usb.get("required_imports", [])
            }
            if direct_import_names & runtime_names != expected_direct_names:
                raise VerificationError(
                    "ADB helper private direct imports do not match the locked runtime partition"
                )
            delayed_names = {
                str(record.get("runtime_file", "")).lower()
                for record in expected_delayed_loads
            }
            if import_names & delayed_names:
                raise VerificationError("ADB delayed runtime sidecar became a direct PE import")
            missing_imports = [
                item
                for item in usb.get("required_imports", [])
                if pathlib.PurePath(str(item)).name.lower() not in direct_import_names
            ]
            if missing_imports:
                raise VerificationError("ADB helper lacks required dynamic libusb import: " + ", ".join(missing_imports))
            if binary.get("libusb_replacement_probe") != "passed":
                raise VerificationError("ADB helper private libusb replacement probe did not pass")
        elif usb.get("backend") == "native-iokit":
            frameworks = {str(item).lower() for item in binary.get("native_frameworks", [])}
            missing_frameworks = [
                item for item in usb.get("frameworks", []) if item.lower() not in frameworks
            ]
            if missing_frameworks:
                raise VerificationError("ADB helper lacks native IOKit framework coverage: " + ", ".join(missing_frameworks))
        else:
            raise VerificationError(f"unsupported locked USB backend: {usb.get('backend')}")

        for required in lock.get("release_assets", []):
            if not isinstance(required, dict) or not asset_required_for_scope(
                required, args.asset_scope
            ):
                continue
            kind = required.get("kind")
            asset_receipt = find_receipt_asset(receipt, kind)
            relative = safe_relative(str(asset_receipt.get("path", "")), f"{kind} asset")
            if relative.as_posix() != required.get("file_name"):
                raise VerificationError(
                    f"release asset path is not the locked file name ({kind}): {relative}"
                )
            asset = source_assets_root / relative
            if not asset.is_file() or asset.is_symlink():
                raise VerificationError(f"missing or invalid release asset ({kind}): {asset}")
            actual = sha256(asset)
            if actual != asset_receipt.get("sha256"):
                raise VerificationError(f"release asset sha256 mismatch ({kind}): expected {asset_receipt.get('sha256')}, got {actual}")
            sidecar_relative = safe_relative(
                str(required.get("sha256_file", "")), f"{kind} sha256 sidecar"
            )
            if sidecar_relative.as_posix() != required.get("sha256_file"):
                raise VerificationError(
                    f"release asset sha256 sidecar is not the locked file name ({kind}): {sidecar_relative}"
                )
            verify_hash_sidecar(
                source_assets_root / sidecar_relative, actual, relative.name
            )

        source_set_receipt_hash = validate_source_set_receipt(
            lock,
            args.lock,
            receipt,
            source_assets_root,
            args.asset_scope,
        )

        verified_receipts = [BINARY_BUILD_RECEIPT]
        if smoke_receipt is not None:
            verified_receipts.append(BINARY_SMOKE_RECEIPT)
        if signing_receipt is not None:
            verified_receipts.append("signing")
        if notarization_receipt is not None:
            verified_receipts.append("notarization")

        package_plan = None
        required_receipts: set[str] = set()
        release_qualified = False
        if args.package_target:
            package_plan = lock.get("package_targets", {}).get(args.package_target)
            if not isinstance(package_plan, dict):
                raise VerificationError(f"unknown locked ADB package target: {args.package_target}")
            if package_plan.get("helper_target") != target:
                raise VerificationError(
                    f"ADB package target {args.package_target} is not bound to helper target {target}"
                )
            qualification = package_plan.get("qualification")
            if not isinstance(qualification, dict):
                raise VerificationError(f"ADB package target lacks qualification policy: {args.package_target}")
            required_receipts = set(qualification.get("required_receipts", []))
            mandatory = {
                BINARY_BUILD_RECEIPT,
                BINARY_SMOKE_RECEIPT,
                PACKAGE_VERIFICATION_RECEIPT,
            }
            if not mandatory.issubset(required_receipts):
                raise VerificationError(
                    f"ADB package target omits mandatory qualification receipts: {args.package_target}"
                )
            if args.package_verification_receipt is not None and not package_records:
                raise VerificationError(
                    "qualified ADB package verification requires the final package file"
                )
            available = set(verified_receipts) | {PACKAGE_VERIFICATION_RECEIPT}
            release_qualified = required_receipts.issubset(available)

        if args.package_verification_receipt is not None:
            receipt_evidence = verified_receipts + [PACKAGE_VERIFICATION_RECEIPT]
            verification_receipt = {
                "schema_version": 1,
                "receipt_kind": PACKAGE_VERIFICATION_RECEIPT,
                "target": target,
                "package_target": args.package_target,
                "layout": layout,
                "source_set_receipt_sha256": source_set_receipt_hash,
                "source_helper_sha256": source_helper_hash,
                "helper_sha256": actual_helper_hash,
                "package_sha256": package_hash,
                "packages": package_records,
                "verified_receipts": receipt_evidence,
                "required_receipts": sorted(required_receipts),
                "release_qualified": release_qualified,
            }
            args.package_verification_receipt.parent.mkdir(parents=True, exist_ok=True)
            args.package_verification_receipt.write_text(
                json.dumps(verification_receipt, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )

        print(f"verified source-built ADB helper target={target} layout={layout} path={helper_relative}")
        return 0
    except VerificationError as error:
        print(f"ADB helper verification failed: {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
