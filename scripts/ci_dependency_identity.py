#!/usr/bin/env python3
"""Version-independent binary-core and verifier-policy identities.

These keys deliberately do not replace the existing lock_sha256 fields in
legacy build receipts. A dependency producer must state both contracts
explicitly until receipt consumers migrate to a new schema.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import stat

from ci_dependency_catalog import validate_catalog
from ci_environment_registry import RegistryError, _json


ADB_BUILD_FILES = (
    "scripts/build_adb_helper.py",
    "packaging/adb/superbuild/CMakeLists.txt",
)
SHARED_POLICY_FILES = (
    "scripts/ci_dependency_gate.py",
    "scripts/ci_dependency_full_evidence.py",
    "scripts/ci_dependency_pipeline.py",
    "scripts/ci_dependency_core.py",
    "scripts/ci_dependency_producer.py",
    "scripts/ci_dependency_toolchain.py",
)
ADB_POLICY_FILES = (
    "scripts/prefetch_adb_helper_sources.py",
    "scripts/prefetch_adb_manifest_fallback.py",
    "scripts/prefetch_adb_source_context.py",
    "scripts/prefetch_adb_libusb_fallback.py",
    "scripts/prefetch_adb_source_closure.py",
    "scripts/verify_adb_helper_artifact.py",
    "scripts/verify_adb_helper_toolchain.py",
    "scripts/smoke_adb_helper.py",
) + SHARED_POLICY_FILES
IOS_BUILD_FILES = (
    "scripts/build_ios_native_stack.py",
    "packaging/ios-native/superbuild/CMakeLists.txt",
)
IOS_POLICY_FILES = (
    "scripts/verify_ios_native_stack.py",
    "scripts/build_ios_native_legal_assets.py",
) + SHARED_POLICY_FILES
ADB_LOCK_KEYS = {
    "schema_version", "helper", "release_policy", "sources", "dependencies",
    "patches", "toolchain_packages", "toolchains", "targets", "install_paths",
    "release_assets", "package_targets",
}
IOS_LOCK_KEYS = {
    "schema_version", "sources", "patches", "release_policy",
    "artifact_contract", "receipts",
}
ADB_RELEASE_POLICY_KEYS = {
    "source_build_required", "fail_closed_on_missing_helper", "allow_google_platform_tools_prebuilt",
    "allow_path_adb", "allow_android_sdk_adb", "allow_runtime_download",
    "allow_floating_revisions", "allow_system_dependency_substitution",
    "downloads_allowed_only_in_prefetch_job", "application_configure_disconnected",
    "source_date_epoch", "require_signing_for_release_qualification",
    "require_notarization_for_macos_qualification",
    "require_attestation_for_release_qualification",
}
IOS_RELEASE_POLICY_KEYS = {
    "allow_runtime_download", "allow_homebrew_runtime",
    "allow_system_dependency_substitution", "allow_usbmuxd_daemon_dependency",
    "allow_mobiledevice_framework", "allow_floating_revisions",
    "source_build_required", "application_configure_disconnected",
    "fail_closed_on_missing_native_stack", "required_architectures", "source_date_epoch",
}
ADB_CORE_TARGET_KEYS = {"os", "arch", "toolchain", "glibc_baseline", "deployment_target", "usb"}
ADB_POLICY_TARGET_KEYS = {"qualified", "symbol_version_maximums", "allowed_dynamic_imports", "qualification"}
IOS_CORE_CONTRACT_KEYS = {"dylib_install_name_prefix", "required_dylibs"}
IOS_POLICY_CONTRACT_KEYS = {
    "allowed_dylib_rpaths", "allowed_system_dependencies", "application_rpath",
    "bundle_directory", "required_exported_symbols", "required_architectures",
    "thin_artifacts", "forbidden_dynamic_references",
}


class DependencyIdentityError(ValueError):
    """A core identity cannot be derived from the reviewed inputs."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise DependencyIdentityError(message)


def _hash_object(value: object) -> str:
    try:
        data = json.dumps(value, sort_keys=True, separators=(",", ":"),
                          allow_nan=False, ensure_ascii=True).encode("ascii")
    except (TypeError, ValueError) as error:
        raise DependencyIdentityError("dependency identity is not canonical JSON") from error
    return hashlib.sha256(data).hexdigest()


def _file_hashes(root: pathlib.Path, files: tuple[str, ...]) -> dict[str, str]:
    root = pathlib.Path(root).resolve(strict=True)
    result = {}
    for relative in files:
        path = root / relative
        if path.is_symlink() or not path.is_file() or not stat.S_ISREG(path.stat().st_mode):
            raise DependencyIdentityError(f"missing or unsafe dependency recipe: {relative}")
        try:
            path.resolve(strict=True).relative_to(root)
        except ValueError as error:
            raise DependencyIdentityError(f"dependency recipe escapes root: {relative}") from error
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
        result[relative] = digest.hexdigest()
    return result


def _ios_producer_environment(root: pathlib.Path, target: str) -> dict:
    root = pathlib.Path(root).resolve(strict=True)
    path = root / "ci/dependencies/catalog.json"
    try:
        _require(not path.is_symlink() and path.is_file(), "missing iOS toolchain catalog")
        path.resolve(strict=True).relative_to(root)
        targets = validate_catalog(_json(path.read_bytes()))
    except (OSError, ValueError, RegistryError) as error:
        raise DependencyIdentityError("invalid iOS producer toolchain catalog") from error
    row = targets["ios-" + target]
    return {"runner": row["runner"], "toolchain": row["toolchain"]}


def _sources_without_legal(records: object, label: str) -> list[dict]:
    _require(isinstance(records, list) and bool(records), f"{label} must be nonempty")
    stripped = []
    for item in records:
        _require(isinstance(item, dict), f"{label} record must be an object")
        stripped.append({key: value for key, value in item.items()
                         if key not in ("legal", "license")})
    return stripped


def core_identity(component: str, target: str, lock: dict, repo_root: pathlib.Path) -> str:
    """Return a SHA-256 key over build-affecting inputs for one native target.

    Entire source/patch and selected toolchain records are hashed rather than
    enumerating implementation-specific flags. Top-level keys are constrained
    so a new build-affecting field cannot silently fall outside the contract.
    """
    _require(isinstance(lock, dict) and type(lock.get("schema_version")) is int
             and lock["schema_version"] == 2, "unsupported dependency lock")
    if component == "adb-helper":
        _require(set(lock) == ADB_LOCK_KEYS, "unknown ADB lock fields")
        _require(isinstance(lock["release_policy"], dict)
                 and set(lock["release_policy"]) == ADB_RELEASE_POLICY_KEYS,
                 "unknown ADB release policy fields")
        targets = lock["targets"]
        _require(isinstance(targets, dict) and target in targets, "unsupported ADB target")
        plan = targets[target]
        _require(
            isinstance(plan, dict)
            and {"os", "arch", "toolchain", "usb"}.issubset(plan)
            and set(plan).issubset(ADB_CORE_TARGET_KEYS | ADB_POLICY_TARGET_KEYS),
            "invalid or untracked ADB target plan",
        )
        toolchain = plan.get("toolchain")
        _require(isinstance(toolchain, str) and toolchain in lock["toolchains"],
                 "ADB target has no locked toolchain")
        _require(type(lock["release_policy"].get("source_date_epoch")) is int,
                 "ADB core has no source date epoch")
        content = {
            "helper": lock["helper"],
            "sources": _sources_without_legal(lock["sources"], "ADB sources"),
            "dependencies": [
                {key: value for key, value in item.items() if key != "license"}
                for item in lock["dependencies"]
            ],
            "patches": lock["patches"],
            "toolchain_packages": (
                lock["toolchain_packages"] if plan["os"] == "windows" else []
            ),
            "toolchain": lock["toolchains"][toolchain],
            "target": {key: plan[key] for key in ADB_CORE_TARGET_KEYS if key in plan},
            "source_date_epoch": lock["release_policy"]["source_date_epoch"],
        }
        recipe = _file_hashes(repo_root, ADB_BUILD_FILES)
    elif component == "ios-native":
        _require(set(lock) == IOS_LOCK_KEYS, "unknown iOS native lock fields")
        _require(isinstance(lock["release_policy"], dict)
                 and set(lock["release_policy"]) == IOS_RELEASE_POLICY_KEYS,
                 "unknown iOS native release policy fields")
        contract = lock["artifact_contract"]
        thin = contract.get("thin_artifacts")
        _require(
            isinstance(contract, dict)
            and set(contract) == IOS_CORE_CONTRACT_KEYS | IOS_POLICY_CONTRACT_KEYS
            and isinstance(thin, dict)
            and target in ("x86_64", "arm64")
            and target in thin
            and isinstance(thin[target], dict)
            and set(thin[target]) == {
                "architecture", "deployment_target", "receipt_file", "native_qualified"
            }
            and thin[target]["architecture"] == target,
            "unsupported or untracked iOS native architecture contract",
        )
        _require(type(lock["release_policy"].get("source_date_epoch")) is int,
                 "iOS native core has no source date epoch")
        content = {
            "sources": _sources_without_legal(lock["sources"], "iOS native sources"),
            "patches": lock["patches"],
            "artifact": {key: contract[key] for key in IOS_CORE_CONTRACT_KEYS},
            "architecture": target,
            "deployment_target": thin[target]["deployment_target"],
            "producer_environment": _ios_producer_environment(repo_root, target),
            "source_date_epoch": lock["release_policy"]["source_date_epoch"],
        }
        recipe = _file_hashes(repo_root, IOS_BUILD_FILES)
    else:
        raise DependencyIdentityError("unsupported dependency component")
    return _hash_object({"schema_version": 1, "kind": "binary-core", "component": component,
                         "target": target, "inputs": content, "recipe_files": recipe})


def policy_identity(component: str, repo_root: pathlib.Path, *, lock: dict) -> str:
    """Bind verifier code and lock qualification rules without rebuilding the core."""
    _require(isinstance(lock, dict), "missing dependency qualification lock")
    if component == "adb-helper":
        _require(set(lock) == ADB_LOCK_KEYS
                 and isinstance(lock["release_policy"], dict)
                 and set(lock["release_policy"]) == ADB_RELEASE_POLICY_KEYS,
                 "invalid ADB qualification lock")
        _require(isinstance(lock["targets"], dict) and bool(lock["targets"])
                 and all(isinstance(plan, dict)
                         and {"os", "arch", "toolchain", "usb"}.issubset(plan)
                         and set(plan).issubset(ADB_CORE_TARGET_KEYS | ADB_POLICY_TARGET_KEYS)
                         for plan in lock["targets"].values()),
                 "untracked ADB target qualification fields")
        files = ADB_POLICY_FILES
        policy = {
            "release_policy": {key: value for key, value in lock["release_policy"].items()
                               if key != "source_date_epoch"},
            "targets": {target: {key: value for key, value in plan.items()
                                 if key in ADB_POLICY_TARGET_KEYS}
                        for target, plan in lock["targets"].items()},
            "package_targets": lock["package_targets"],
            "install_paths": lock["install_paths"],
            "sources_legal": [item.get("legal") for item in lock["sources"]],
            "dependencies_legal": [item.get("license") for item in lock["dependencies"]],
        }
    elif component == "ios-native":
        _require(set(lock) == IOS_LOCK_KEYS
                 and isinstance(lock["release_policy"], dict)
                 and set(lock["release_policy"]) == IOS_RELEASE_POLICY_KEYS
                 and isinstance(lock["artifact_contract"], dict)
                 and set(lock["artifact_contract"]) == IOS_CORE_CONTRACT_KEYS | IOS_POLICY_CONTRACT_KEYS,
                 "invalid iOS native qualification lock")
        files = IOS_POLICY_FILES
        policy = {
            "release_policy": {key: value for key, value in lock["release_policy"].items()
                               if key != "source_date_epoch"},
            "artifact_contract": {key: lock["artifact_contract"][key]
                                  for key in IOS_POLICY_CONTRACT_KEYS},
            "receipts": lock["receipts"],
            "sources_legal": [item.get("legal") for item in lock["sources"]],
        }
    else:
        raise DependencyIdentityError("unsupported dependency component")
    return _hash_object({"schema_version": 1, "kind": "qualification-policy",
                         "component": component, "policy": policy,
                         "verification_files": _file_hashes(repo_root, files)})
