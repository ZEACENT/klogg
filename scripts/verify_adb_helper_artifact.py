#!/usr/bin/env python3
"""Fail-closed verification for source-built ADB helper artifacts and packages."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import stat

from verify_adb_helper_envelope import EnvelopeError, verify_checksum_file


BINARY_BUILD_RECEIPT = "binary-build"
BINARY_SMOKE_RECEIPT = "binary-smoke"
PACKAGE_VERIFICATION_RECEIPT = "package-verification"
# Stable normalized identifiers used by receipt-policy tooling.
binary_build = BINARY_BUILD_RECEIPT
binary_smoke = BINARY_SMOKE_RECEIPT
package_verification = PACKAGE_VERIFICATION_RECEIPT


class VerificationError(RuntimeError):
    pass


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--receipt", required=True, type=pathlib.Path)
    parser.add_argument("--package-root", required=True, type=pathlib.Path)
    parser.add_argument("--release-root", required=True, type=pathlib.Path)
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
        if lock.get("schema_version") != 1 or receipt.get("schema_version") != 1:
            raise VerificationError("unsupported ADB helper lock or receipt schema")
        if args.checksum_envelope is not None:
            try:
                verify_checksum_file(args.checksum_envelope)
            except EnvelopeError as error:
                raise VerificationError(f"checksum envelope verification failed: {error}") from error
        if receipt.get("receipt_kind", BINARY_BUILD_RECEIPT) != BINARY_BUILD_RECEIPT:
            raise VerificationError("ADB helper receipt is not binary-build evidence")

        smoke_receipt = None
        if args.binary_smoke_receipt is not None:
            smoke_receipt = read_json(args.binary_smoke_receipt, "ADB helper binary-smoke receipt")
            if smoke_receipt.get("receipt_kind", BINARY_SMOKE_RECEIPT) != BINARY_SMOKE_RECEIPT:
                raise VerificationError("ADB helper smoke receipt is not binary-smoke evidence")
            passed_probes = set(smoke_receipt.get("passed_probes", []))
            required_smoke_probes = {
                "version",
                "complete-client",
                "loopback-private-server",
                "smart-socket-host-version",
                "no-lingering-process",
            }
            if passed_probes != required_smoke_probes:
                raise VerificationError("ADB helper binary-smoke receipt lacks the complete probe set")

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
        if not isinstance(binary, dict):
            raise VerificationError("receipt lacks binary verification")
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
        runtime_loads = [str(item).lower() for item in binary.get("runtime_loads", [])]
        expected_runtime = usb.get("runtime_files", [])
        runtime_receipt = binary.get("runtime_closure", [])
        if not isinstance(expected_runtime, list) or not isinstance(runtime_receipt, list):
            raise VerificationError("ADB helper runtime closure metadata must be arrays")
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
        for name in expected_runtime:
            runtime = helper_path.parent / name
            require_regular_file(runtime, f"ADB helper runtime {name}")
            actual_runtime_hash = sha256(runtime)
            if actual_runtime_hash != runtime_by_name[name].get("sha256"):
                raise VerificationError(
                    f"ADB helper runtime sha256 mismatch ({name}): expected {runtime_by_name[name].get('sha256')}, got {actual_runtime_hash}"
                )

        import_names = {
            pathlib.PurePosixPath(imported.replace("\\", "/")).name.lower()
            for imported in imports
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
            missing_imports = [
                item
                for item in usb.get("required_imports", [])
                if pathlib.PurePath(str(item)).name.lower() not in import_names
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
            if not required.get("required"):
                continue
            kind = required.get("kind")
            asset_receipt = find_receipt_asset(receipt, kind)
            relative = safe_relative(str(asset_receipt.get("path", "")), f"{kind} asset")
            if relative.as_posix() != required.get("file_name"):
                raise VerificationError(
                    f"release asset path is not the locked file name ({kind}): {relative}"
                )
            asset = args.release_root / relative
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
                args.release_root / sidecar_relative, actual, relative.name
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
