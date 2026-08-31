#!/usr/bin/env python3
"""Fail-closed Mach-O, provenance, and package verification for the iOS native stack."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys


class VerificationError(RuntimeError):
    pass


def run(command: list[str]) -> str:
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise VerificationError(
            f"command failed ({result.returncode}): {' '.join(command)}\n{result.stderr.strip()}"
        )
    return result.stdout


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: pathlib.Path, label: str) -> dict:
    if not path.is_file() or path.is_symlink():
        raise VerificationError(f"missing or invalid {label}: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise VerificationError(f"invalid {label}: {error}") from error
    if not isinstance(value, dict):
        raise VerificationError(f"invalid {label}: root must be an object")
    return value


def dotted(value: str) -> tuple[int, ...]:
    if not re.fullmatch(r"\d+(?:\.\d+)*", value):
        raise VerificationError(f"invalid deployment_target: {value}")
    return tuple(int(part) for part in value.split("."))


def dependencies(path: pathlib.Path) -> list[str]:
    output = run(["otool", "-L", str(path)])
    return [line.strip().split(" ", 1)[0] for line in output.splitlines()[1:] if line.strip()]


def rpaths(path: pathlib.Path) -> list[str]:
    lines = run(["otool", "-l", str(path)]).splitlines()
    values: list[str] = []
    for index, line in enumerate(lines):
        if line.strip() != "cmd LC_RPATH":
            continue
        for candidate in lines[index + 1 : index + 5]:
            match = re.match(r"^\s*path\s+(\S+)\s+\(offset\s+\d+\)\s*$", candidate)
            if match is not None:
                values.append(match.group(1))
                break
        else:
            raise VerificationError(f"malformed LC_RPATH command: {path}")
    return values


def install_name(path: pathlib.Path) -> str:
    lines = [line.strip() for line in run(["otool", "-D", str(path)]).splitlines() if line.strip()]
    if len(lines) != 2:
        raise VerificationError(f"missing or ambiguous Mach-O install_name: {path}")
    return lines[1]


def architectures(path: pathlib.Path) -> list[str]:
    return run(["lipo", "-archs", str(path)]).split()


def deployment_target(path: pathlib.Path) -> str:
    output = run(["vtool", "-show-build", str(path)])
    if "LC_BUILD_VERSION" not in output:
        raise VerificationError(f"Mach-O lacks LC_BUILD_VERSION: {path}")
    match = re.search(r"^\s*minos\s+(\d+(?:\.\d+)+)\s*$", output, re.MULTILINE)
    if match is None:
        raise VerificationError(f"Mach-O lacks deployment_target/minos: {path}")
    return match.group(1)


def validate_build_receipt(
    lock: dict, lock_path: pathlib.Path, receipt: dict, architecture: str
) -> None:
    if receipt.get("receipt_kind") != "ios-native-build":
        raise VerificationError("invalid iOS native build receipt_kind")
    if receipt.get("lock_sha256") != sha256(lock_path):
        raise VerificationError("build receipt is not bound to the current lock sha256")
    if receipt.get("architecture") != architecture:
        raise VerificationError("build receipt architecture mismatch")
    thin = lock.get("artifact_contract", {}).get("thin_artifacts", {}).get(architecture)
    if not isinstance(thin, dict):
        raise VerificationError(f"architecture is not locked: {architecture}")
    if str(receipt.get("deployment_target")) != str(thin.get("deployment_target")):
        raise VerificationError("build receipt deployment target mismatch")
    if receipt.get("native_qualified") is not True or receipt.get("qualification") != "native":
        raise VerificationError("build receipt is inspection-only and not native_qualified")


def verify_stack(
    lock: dict,
    stack_root: pathlib.Path,
    architecture: str,
    receipt: dict,
    *,
    enforce_build_hashes: bool = True,
    require_code_signature: bool = False,
) -> list[dict]:
    contract = lock.get("artifact_contract", {})
    thin = contract.get("thin_artifacts", {}).get(architecture)
    if not isinstance(thin, dict):
        raise VerificationError(f"architecture is not locked: {architecture}")
    expected_target = str(thin["deployment_target"])
    allowed_rpaths = contract.get("allowed_dylib_rpaths")
    if not isinstance(allowed_rpaths, list) or not allowed_rpaths:
        raise VerificationError("lock is missing allowed_dylib_rpaths")
    allowed_system = contract.get("allowed_system_dependencies")
    if not isinstance(allowed_system, list) or not allowed_system:
        raise VerificationError("lock is missing allowed_system_dependencies")

    libdir = stack_root / "lib"
    if not libdir.is_dir() or libdir.is_symlink():
        raise VerificationError(f"missing or invalid iOS native dylib directory: {libdir}")
    required_dylibs = contract.get("required_dylibs", [])
    if not isinstance(required_dylibs, list) or not required_dylibs:
        raise VerificationError("lock is missing the required iOS native dylib closure")
    for name in required_dylibs:
        path = libdir / str(name)
        if not path.exists():
            raise VerificationError(f"missing required iOS native dylib: {name}")
        if path.resolve().parent != libdir.resolve():
            raise VerificationError(f"iOS native dylib symlink escapes the closure: {name}")
    dylibs = sorted(path for path in libdir.glob("*.dylib") if path.is_file() and not path.is_symlink())
    if not dylibs:
        raise VerificationError("missing iOS native dylib closure; fail closed")

    forbidden = tuple(contract.get("forbidden_dynamic_references", ())) + (
        "/opt/homebrew",
        "/usr/local/opt",
        "MobileDevice.framework",
        "/System/Library/PrivateFrameworks",
    )
    names = {path.name for path in dylibs}
    evidence = []
    for dylib in dylibs:
        actual_install_name = install_name(dylib)
        expected_install_name = f"@rpath/{dylib.name}"
        if actual_install_name != expected_install_name:
            raise VerificationError(
                f"dylib install_name mismatch: expected {expected_install_name}, got {actual_install_name}"
            )
        actual_rpaths = rpaths(dylib)
        if actual_rpaths != allowed_rpaths:
            raise VerificationError(
                f"LC_RPATH mismatch for {dylib.name}: {actual_rpaths} != {allowed_rpaths}"
            )
        actual_architectures = architectures(dylib)
        if actual_architectures != [architecture]:
            raise VerificationError(
                f"lipo architecture mismatch for {dylib.name}: {actual_architectures}"
            )
        actual_target = deployment_target(dylib)
        if dotted(actual_target) != dotted(expected_target):
            raise VerificationError(
                f"deployment_target mismatch for {dylib.name}: {actual_target} != {expected_target}"
            )

        imports = dependencies(dylib)
        for imported in imports:
            if any(value in imported for value in forbidden):
                raise VerificationError(f"forbidden dynamic closure reference in {dylib.name}: {imported}")
            if imported.startswith("@rpath/"):
                referenced = pathlib.PurePosixPath(imported).name
                if referenced not in names and not (libdir / referenced).exists():
                    raise VerificationError(
                        f"not found in bundled Mach-O closure: {dylib.name} -> {imported}"
                    )
            elif imported not in allowed_system:
                raise VerificationError(
                    f"unlocked system or unbundled Mach-O dependency in {dylib.name}: {imported}"
                )
        if require_code_signature:
            run(["codesign", "--verify", "--strict", "--verbose=2", str(dylib)])
        evidence.append(
            {
                "name": dylib.name,
                "sha256": sha256(dylib),
                "install_name": actual_install_name,
                "rpaths": actual_rpaths,
                "architectures": actual_architectures,
                "deployment_target": actual_target,
                "imports": imports,
            }
        )

    receipt_by_name = {
        item.get("name"): item for item in receipt.get("dylibs", []) if isinstance(item, dict)
    }
    if set(receipt_by_name) != names:
        raise VerificationError("build receipt does not bind every shipped dylib")
    if enforce_build_hashes:
        for item in evidence:
            if receipt_by_name[item["name"]].get("sha256") != item["sha256"]:
                raise VerificationError(f"build receipt sha256 mismatch: {item['name']}")

    bundled_tools = stack_root / "bin"
    if bundled_tools.exists() and any(bundled_tools.iterdir()):
        raise VerificationError("usbmuxd daemon or command-line tools must not be bundled")
    return evidence


def verify_application_rpath(lock: dict, executable: pathlib.Path) -> dict:
    if not executable.is_file() or executable.is_symlink():
        raise VerificationError(f"missing packaged application executable: {executable}")
    expected = lock.get("artifact_contract", {}).get("application_rpath")
    actual = rpaths(executable)
    if expected not in actual:
        raise VerificationError(f"application LC_RPATH is missing {expected}: {actual}")
    absolute = [value for value in actual if value.startswith("/")]
    if absolute:
        raise VerificationError(f"packaged application has absolute LC_RPATH values: {absolute}")
    forbidden = tuple(lock.get("artifact_contract", {}).get("forbidden_dynamic_references", ()))
    for value in actual + dependencies(executable):
        if any(item in value for item in forbidden):
            raise VerificationError(f"forbidden packaged application dependency/rpath: {value}")
    return {"path": str(executable), "sha256": sha256(executable), "rpaths": actual}


def bound_asset(base: pathlib.Path, record: object, label: str) -> dict:
    if not isinstance(record, dict):
        raise VerificationError(f"invalid {label} record")
    relative = record.get("path")
    expected = record.get("sha256")
    if not isinstance(relative, str) or pathlib.PurePosixPath(relative).is_absolute():
        raise VerificationError(f"invalid {label} path")
    path = base / relative
    resolved_base = base.resolve()
    resolved_path = path.resolve()
    if (
        not path.is_file()
        or path.is_symlink()
        or resolved_path.parent != resolved_base and resolved_base not in resolved_path.parents
    ):
        raise VerificationError(f"missing or escaping {label}: {path}")
    if sha256(path) != expected:
        raise VerificationError(f"{label} sha256 mismatch: {path}")
    return {"path": relative, "sha256": expected}


def verify_legal_assets(
    lock_path: pathlib.Path,
    architecture: str,
    source_receipt_path: pathlib.Path,
    legal_receipt_path: pathlib.Path,
    sbom_path: pathlib.Path,
    *,
    asset_scope: str | None = None,
    source_assets_root: pathlib.Path | None = None,
) -> dict:
    lock_hash = sha256(lock_path)
    source = read_json(source_receipt_path, "iOS native source-set receipt")
    legal = read_json(legal_receipt_path, "iOS native legal receipt")
    sbom = read_json(sbom_path, "iOS native SPDX SBOM")
    if (
        source.get("schema_version") != 1
        or source.get("receipt_kind") != "component-source-set"
        or source.get("component") != "ios-native"
        or source.get("lock_sha256") != lock_hash
    ):
        raise VerificationError("invalid or stale iOS native component source-set receipt")
    if legal.get("receipt_kind") != "legal" or legal.get("lock_sha256") != lock_hash:
        raise VerificationError("invalid or stale iOS native legal receipt")
    if legal.get("architecture") != architecture:
        raise VerificationError("iOS native legal receipt architecture mismatch")

    identity = source.get("source_identity")
    if not isinstance(identity, dict):
        raise VerificationError("iOS native source-set receipt lacks source identity")
    for field in ("manifest_or_closure_sha256", "final_tree_sha256"):
        if re.fullmatch(r"[0-9a-f]{64}", str(identity.get(field, ""))) is None:
            raise VerificationError(f"invalid iOS native source-set {field}")
    if identity.get("tree_hash_algorithm") != "sha256":
        raise VerificationError("unsupported iOS native source tree hash algorithm")
    if re.fullmatch(r"[0-9a-f]{64}", str(source.get("patch_chain_sha256", ""))) is None:
        raise VerificationError("invalid iOS native source-set patch chain sha256")
    if source.get("distribution") != {
        "package_required": False,
        "release_required": True,
    }:
        raise VerificationError("invalid iOS native source archive distribution")

    assets_root = source_assets_root or source_receipt_path.parent
    support = source.get("package_support_assets")
    if not isinstance(support, list) or not support:
        raise VerificationError("iOS native source-set receipt has no package support assets")
    seen_support = set()
    for index, item in enumerate(support):
        if not isinstance(item, dict) or not isinstance(item.get("kind"), str):
            raise VerificationError(f"invalid iOS native package support asset {index}")
        if item["kind"] in seen_support and not item["kind"].startswith("license:"):
            raise VerificationError(f"duplicate iOS native package support asset: {item['kind']}")
        seen_support.add(item["kind"])
        bound_asset(
            assets_root,
            {"path": item.get("file_name"), "sha256": item.get("sha256")},
            f"iOS native package support asset {item['kind']}",
        )
    required_support = {"source-offer", "replacement-guide", "notices"}
    if not required_support.issubset(seen_support) or not any(
        kind.startswith("license:") for kind in seen_support
    ):
        raise VerificationError("iOS native source-set package support coverage is incomplete")

    archive = source.get("archive")
    if not isinstance(archive, dict):
        raise VerificationError("iOS native source-set receipt lacks corresponding source archive")
    if asset_scope == "package":
        archive_name = archive.get("file_name")
        if not isinstance(archive_name, str) or pathlib.PurePosixPath(archive_name).name != archive_name:
            raise VerificationError("invalid corresponding source archive file name")
        archive_path = assets_root / archive_name
        if archive_path.exists() or archive_path.is_symlink() or archive_path.with_name(
            archive_path.name + ".sha256"
        ).exists():
            raise VerificationError(
                "iOS native package must not contain the corresponding source archive"
            )
    else:
        bound_asset(
            assets_root,
            {"path": archive.get("file_name"), "sha256": archive.get("sha256")},
            "corresponding source",
        )
    bound_asset(legal_receipt_path.parent, legal.get("notice"), "native notice")
    bound_asset(legal_receipt_path.parent, legal.get("replacement_guide"), "LGPL replacement guide")
    license_files = legal.get("license_files")
    if not isinstance(license_files, list) or not license_files:
        raise VerificationError("legal receipt has no license files")
    for index, item in enumerate(license_files):
        bound_asset(legal_receipt_path.parent, item, f"license file {index}")
    bound_sbom = bound_asset(legal_receipt_path.parent, legal.get("sbom"), "SPDX SBOM")
    if sbom_path.resolve() != (legal_receipt_path.parent / bound_sbom["path"]).resolve():
        raise VerificationError("explicit SBOM path does not match the legal receipt")
    if sbom.get("spdxVersion") != "SPDX-2.3":
        raise VerificationError("unsupported or missing SPDX version")
    source_set_hash = sha256(source_receipt_path)
    return {
        "source_receipt_sha256": source_set_hash,
        "source_set_receipt_sha256": source_set_hash,
        "legal_receipt_sha256": sha256(legal_receipt_path),
        "sbom_sha256": sha256(sbom_path),
    }


def verify_package_receipt(
    package: dict,
    lock_hash: str,
    architecture: str,
    build_receipt_hash: str,
    evidence: list[dict],
    legal_hashes: dict,
) -> None:
    if package.get("receipt_kind") != "ios-native-package":
        raise VerificationError("invalid ios-native-package receipt_kind")
    if package.get("lock_sha256") != lock_hash or package.get("architecture") != architecture:
        raise VerificationError("package receipt lock or architecture mismatch")
    if package.get("build_receipt_sha256") != build_receipt_hash:
        raise VerificationError("package receipt build receipt binding mismatch")
    for key, value in legal_hashes.items():
        if package.get(key) != value:
            raise VerificationError(f"package receipt {key} mismatch")
    expected = {item["name"]: item["sha256"] for item in evidence}
    actual = {
        item.get("name"): item.get("sha256")
        for item in package.get("dylibs", [])
        if isinstance(item, dict)
    }
    if actual != expected:
        raise VerificationError("package receipt does not bind the final dylib closure")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--stack-root", required=True, type=pathlib.Path)
    parser.add_argument("--architecture", choices=("x86_64", "arm64"), required=True)
    parser.add_argument("--receipt", type=pathlib.Path)
    parser.add_argument("--package-receipt", type=pathlib.Path)
    parser.add_argument("--verify-package-receipt", type=pathlib.Path)
    parser.add_argument("--source-receipt", type=pathlib.Path)
    parser.add_argument("--asset-scope", choices=("package", "release"))
    parser.add_argument("--source-assets-root", type=pathlib.Path)
    parser.add_argument("--legal-receipt", type=pathlib.Path)
    parser.add_argument("--sbom", type=pathlib.Path)
    parser.add_argument("--app-executable", type=pathlib.Path)
    parser.add_argument("--signed-package-stage", action="store_true")
    parser.add_argument(
        "--unsigned-package-stage",
        action="store_true",
        help="Verify a validation package receipt without requiring code signatures.",
    )
    args = parser.parse_args()

    try:
        if args.signed_package_stage and args.unsigned_package_stage:
            raise VerificationError(
                "signed and unsigned package-stage modes are mutually exclusive"
            )
        if args.unsigned_package_stage and args.verify_package_receipt is None:
            raise VerificationError(
                "unsigned package-stage mode requires --verify-package-receipt"
            )
        lock = read_json(args.lock, "iOS native lock")
        receipt_path = args.receipt or args.stack_root / "ios-native-build-receipt.json"
        receipt = read_json(receipt_path, "iOS native build receipt")
        validate_build_receipt(lock, args.lock, receipt, args.architecture)
        evidence = verify_stack(
            lock,
            args.stack_root,
            args.architecture,
            receipt,
            enforce_build_hashes=(
                not args.signed_package_stage
                and (
                    args.verify_package_receipt is None
                    or args.unsigned_package_stage
                )
            ),
            require_code_signature=args.signed_package_stage
            or (
                args.verify_package_receipt is not None and not args.unsigned_package_stage
            ),
        )
        application = (
            verify_application_rpath(lock, args.app_executable)
            if args.app_executable is not None
            else None
        )

        receipt_actions = args.package_receipt is not None or args.verify_package_receipt is not None
        legal_verification_required = receipt_actions or args.asset_scope is not None
        legal_paths = (args.source_receipt, args.legal_receipt, args.sbom)
        if legal_verification_required and any(path is None for path in legal_paths):
            raise VerificationError(
                "scoped/package verification requires source, legal, and SBOM paths"
            )
        if args.asset_scope == "release" and args.source_assets_root is None:
            raise VerificationError(
                "iOS native release scope requires an external --source-assets-root"
            )
        legal_hashes = {}
        if legal_verification_required:
            legal_hashes = verify_legal_assets(
                args.lock,
                args.architecture,
                args.source_receipt,
                args.legal_receipt,
                args.sbom,
                asset_scope=args.asset_scope,
                source_assets_root=args.source_assets_root,
            )
            expected_source_set_hash = legal_hashes["source_set_receipt_sha256"]
            bound_source_set_hash = receipt.get("source_set_receipt_sha256")
            if args.asset_scope is not None and bound_source_set_hash != expected_source_set_hash:
                raise VerificationError(
                    "iOS native build receipt source-set receipt sha256 mismatch"
                )
            if bound_source_set_hash not in (None, expected_source_set_hash):
                raise VerificationError(
                    "iOS native build receipt has a stale source-set receipt binding"
                )

        build_receipt_hash = sha256(receipt_path)
        if args.package_receipt is not None:
            if application is None:
                raise VerificationError("package receipt requires --app-executable binding")
            package = {
                "schema_version": 2,
                "receipt_kind": "ios-native-package",
                "lock_sha256": sha256(args.lock),
                "build_receipt_sha256": build_receipt_hash,
                "architecture": args.architecture,
                "stack_root": str(args.stack_root),
                "application": application,
                **legal_hashes,
                "source_set_receipt_sha256": legal_hashes.get(
                    "source_set_receipt_sha256"
                ),
                "dylibs": evidence,
                "status": "passed",
            }
            args.package_receipt.parent.mkdir(parents=True, exist_ok=True)
            args.package_receipt.write_text(
                json.dumps(package, indent=2, sort_keys=True) + "\n", encoding="utf-8"
            )
        if args.verify_package_receipt is not None:
            package = read_json(args.verify_package_receipt, "iOS native package receipt")
            verify_package_receipt(
                package,
                sha256(args.lock),
                args.architecture,
                build_receipt_hash,
                evidence,
                legal_hashes,
            )
        print(f"verified iOS native Mach-O closure ({args.architecture}, {len(evidence)} dylibs)")
        return 0
    except VerificationError as error:
        print(f"iOS native stack verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
