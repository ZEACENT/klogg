#!/usr/bin/env python3
"""Qualify an existing native artifact and prepare an OFFLINE core candidate.

The candidate is not a publication, lock, signature, or reusable qualified
receipt. Existing build receipts remain bound to the full legacy source lock;
reusing a core across changed source-publication policy needs separate receipt
schema migration and a trusted CI publisher gate.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile

from ci_dependency_catalog import TARGETS, validate_catalog
from ci_dependency_core import ADB_RUNTIME, IOS_LIB, package_core
from ci_dependency_identity import core_identity, policy_identity

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ProducerError(ValueError):
    """This artifact cannot become an offline, nonpublishing core candidate."""


def _require(condition: bool, reason: str) -> None:
    if not condition:
        raise ProducerError(reason)


def _json(path: pathlib.Path) -> dict:
    _require(path.is_file() and not path.is_symlink(), f"missing or unsafe input: {path}")
    try:
        def unique(pairs):
            result = {}
            for key, value in pairs:
                _require(key not in result, f"duplicate JSON key: {key}")
                result[key] = value
            return result

        result = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ProducerError(f"invalid input JSON: {path}") from error
    _require(isinstance(result, dict), f"input must be a JSON object: {path}")
    return result


def _sha(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _outside(path: pathlib.Path, *roots: pathlib.Path) -> None:
    for root in roots:
        try:
            path.resolve().relative_to(root.resolve())
        except ValueError:
            continue
        raise ProducerError("candidate outputs must not enter qualified inputs")


def _verify(argv: list[str], run_verifier) -> None:
    try:
        result = run_verifier(argv, capture_output=True, text=True, timeout=300)
    except (OSError, subprocess.TimeoutExpired, subprocess.CalledProcessError) as error:
        raise ProducerError(f"native qualification verifier failed: {pathlib.Path(argv[1]).name}") from error
    _require(result.returncode == 0,
             f"native qualification verifier failed: {pathlib.Path(argv[1]).name}: "
             f"{(result.stderr or result.stdout or '').strip()[-500:]}")


def _stage_file(source: pathlib.Path, destination: pathlib.Path) -> None:
    _require(not source.is_symlink() and source.is_file()
             and stat.S_ISREG(source.stat().st_mode), f"missing or unsafe binary: {source}")
    before = _sha(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    _require(before == _sha(destination) == _sha(source),
             f"binary changed during core projection: {source}")


def _stage_adb(artifact_root: pathlib.Path, stage: pathlib.Path, lock: dict, target: str) -> None:
    plan = lock["targets"][target]
    runtime = plan["usb"].get("runtime_files", [])
    _require(target in ADB_RUNTIME and isinstance(runtime, list)
             and runtime == list(ADB_RUNTIME[target]), "untracked locked ADB runtime closure")
    helper = "adb.exe" if target.startswith("windows-") else "adb"
    expected = {helper, *runtime}
    helper_dir = artifact_root / "helpers"
    _require(helper_dir.is_dir() and not helper_dir.is_symlink(), "invalid ADB helpers directory")
    _require({path.name for path in helper_dir.iterdir()} == expected,
             "ADB artifact binary closure does not match the lock")
    for name in sorted(expected):
        _stage_file(helper_dir / name, stage / "helpers" / name)


def _stage_ios(artifact_root: pathlib.Path, stage: pathlib.Path) -> None:
    lib = artifact_root / "lib"
    _require(lib.is_dir() and not lib.is_symlink(), "invalid iOS dylib directory")
    files = list(lib.iterdir())
    _require(bool(files), "empty iOS native binary closure")
    for path in files:
        relative = "lib/" + path.name
        _require(IOS_LIB.fullmatch(relative) is not None,
                 f"untracked iOS native binary: {relative}")
        destination = stage / relative
        if path.is_symlink():
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.symlink_to(os.readlink(path))
        else:
            _stage_file(path, destination)
    # package_core verifies every symlink target, path, type, and mode.


def _ios_toolchain(artifact_root: pathlib.Path, pinned: dict) -> None:
    receipt = _json(artifact_root / "ios-native-build-receipt.json")
    observed = receipt.get("toolchain")
    _require(isinstance(observed, dict), "missing observed iOS native toolchain")
    for name, expected in pinned.items():
        _require(name in observed and observed[name] == expected,
                 f"unobserved or unpinned iOS native {name} (Ninja is mandatory)")
    # A self-reported receipt is only offline candidate evidence. The later
    # trusted workflow must independently enforce runner/toolchain provenance.


def _verify_adb(repo_root, artifact_root, source_assets_root, target, lock_path,
                temporary, run_verifier) -> None:
    _verify([sys.executable, str(repo_root / "scripts/verify_adb_helper_envelope.py"),
             "--lock", str(lock_path), "--artifact-root", str(artifact_root),
             "--expected-target", target], run_verifier)
    computed_receipt = temporary / "package-verification.json"
    helper = "adb.exe" if target.startswith("windows-") else "adb"
    _verify([sys.executable, str(repo_root / "scripts/verify_adb_helper_artifact.py"),
             "--lock", str(lock_path), "--receipt", str(artifact_root / "receipt.json"),
             "--binary-smoke-receipt", str(artifact_root / "package-smoke.json"),
             "--package-root", str(artifact_root), "--asset-scope", "package",
             "--source-assets-root", str(source_assets_root),
             "--helper-path", "helpers/" + helper, "--expected-target", target,
             "--package-verification-receipt", str(computed_receipt),
             "--require-lock-binding"], run_verifier)
    existing = artifact_root / "package-verification.json"
    _require(computed_receipt.is_file() and not computed_receipt.is_symlink(),
             "missing requalified ADB package receipt")
    stored, rechecked = _json(existing), _json(computed_receipt)
    immutable = ("target", "layout", "source_set_receipt_sha256",
                 "source_helper_sha256", "helper_sha256", "package_sha256", "packages")
    _require(all(key in stored and key in rechecked and stored[key] == rechecked[key]
                 for key in immutable)
             and all(receipt.get("schema_version") == 1
                     and receipt.get("receipt_kind") == "package-verification"
                     and receipt.get("target") == target
                     for receipt in (stored, rechecked)),
             "ADB requalification changed its binary or source binding")
    # The old signed envelope remains unchanged. Policy-only verification may
    # legitimately add new evidence to the independently produced receipt.
    _verify([sys.executable, str(repo_root / "scripts/verify_adb_helper_envelope.py"),
             "--lock", str(lock_path), "--artifact-root", str(artifact_root),
             "--expected-target", target], run_verifier)
    return {"legacy_package_receipt_sha256": _sha(existing),
            "rechecked_package_receipt_sha256": _sha(computed_receipt)}


def _verify_ios(repo_root, artifact_root, source_assets_root, target, lock_path,
                run_verifier) -> None:
    _verify([sys.executable, str(repo_root / "scripts/verify_ios_native_stack.py"),
             "--lock", str(lock_path), "--stack-root", str(artifact_root),
             "--architecture", target,
             "--receipt", str(artifact_root / "ios-native-build-receipt.json"),
             "--asset-scope", "release", "--source-assets-root", str(source_assets_root),
             "--source-receipt", str(source_assets_root / "ios-native-source-set-receipt.json"),
             "--legal-receipt", str(source_assets_root / "ios-native-legal-receipt.json"),
             "--sbom", str(source_assets_root / "ios-native-sbom.spdx.json")], run_verifier)


def produce_candidate(*, repo_root: pathlib.Path, target_id: str,
                      artifact_root: pathlib.Path, source_assets_root: pathlib.Path,
                      archive: pathlib.Path, candidate_receipt: pathlib.Path,
                      run_verifier=subprocess.run) -> dict:
    """Generate a nonpublishing candidate from a fully qualified existing build."""
    repo_root = pathlib.Path(repo_root).resolve()
    artifact_root, source_assets_root = pathlib.Path(artifact_root), pathlib.Path(source_assets_root)
    archive, candidate_receipt = pathlib.Path(archive), pathlib.Path(candidate_receipt)
    _require(artifact_root.is_dir() and not artifact_root.is_symlink()
             and source_assets_root.is_dir() and not source_assets_root.is_symlink(),
             "missing or unsafe artifact/legal input roots")
    catalog_path = repo_root / "ci/dependencies/catalog.json"
    targets = validate_catalog(_json(catalog_path))
    _require(target_id in targets, "unsupported dependency catalog target")
    row = targets[target_id]
    component, target = row["component"], row["target"]
    _require(archive.name == row["archive_name"], "candidate archive name does not match catalog")
    _require(not archive.exists() and not archive.is_symlink()
             and not candidate_receipt.exists() and not candidate_receipt.is_symlink(),
             "candidate outputs already exist")
    _require(archive.parent.is_dir() and candidate_receipt.parent.is_dir()
             and not archive.parent.is_symlink() and not candidate_receipt.parent.is_symlink(),
             "candidate output parents must exist and be real directories")
    _outside(archive, artifact_root, source_assets_root)
    _outside(candidate_receipt, artifact_root, source_assets_root)
    lock_path = (repo_root / "packaging/adb/adb-helper.lock.json" if component == "adb-helper"
                 else repo_root / "3rdparty/libimobiledevice/libimobiledevice.lock.json")
    lock = _json(lock_path)
    binary_key = core_identity(component, target, lock, repo_root)
    policy_key = policy_identity(component, repo_root, lock=lock)
    if component == "ios-native":
        _ios_toolchain(artifact_root, row["toolchain"])
    with tempfile.TemporaryDirectory(dir=archive.parent) as directory:
        temporary = pathlib.Path(directory)
        if component == "adb-helper":
            qualification = _verify_adb(repo_root, artifact_root, source_assets_root,
                                        target, lock_path, temporary, run_verifier)
            _stage_adb(artifact_root, temporary / "binary-core", lock, target)
        else:
            _verify_ios(repo_root, artifact_root, source_assets_root, target,
                        lock_path, run_verifier)
            qualification = None
            _stage_ios(artifact_root, temporary / "binary-core")
        identity = package_core(temporary / "binary-core", archive, component=component,
                                target=target, core_identity=binary_key)
    document = {
        "schema_version": 1,
        "kind": "native-dependency-core-candidate",
        "publication_status": "candidate-only",
        "catalog_target": target_id,
        "catalog_sha256": _sha(catalog_path),
        "component": component,
        "target": target,
        "runner": row["runner"],
        "core_identity": binary_key,
        "policy_identity": policy_key,
        "archive": {"name": row["archive_name"], **identity},
    }
    if qualification is not None:
        document["qualification"] = qualification
    payload = (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8")
    temporary_receipt = None
    try:
        with tempfile.NamedTemporaryFile(dir=candidate_receipt.parent, delete=False) as output:
            temporary_receipt = pathlib.Path(output.name)
            output.write(payload)
        os.link(temporary_receipt, candidate_receipt)
    except OSError:
        archive.unlink()
        raise
    finally:
        if temporary_receipt is not None:
            temporary_receipt.unlink(missing_ok=True)
    return document


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-id", required=True, choices=sorted(TARGETS))
    parser.add_argument("--artifact-root", required=True, type=pathlib.Path)
    parser.add_argument("--source-assets-root", required=True, type=pathlib.Path)
    parser.add_argument("--archive", required=True, type=pathlib.Path)
    parser.add_argument("--candidate-receipt", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        candidate = produce_candidate(
            repo_root=ROOT, target_id=args.target_id, artifact_root=args.artifact_root,
            source_assets_root=args.source_assets_root, archive=args.archive,
            candidate_receipt=args.candidate_receipt,
        )
    except (ProducerError, OSError, ValueError) as error:
        parser.exit(1, f"offline native-core candidate refused: {error}\n")
    print(json.dumps(candidate, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
