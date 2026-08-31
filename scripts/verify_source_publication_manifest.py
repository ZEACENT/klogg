#!/usr/bin/env python3
"""Verify a coherent package and external component-source publication set."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys

from source_publication_identity import normalize_base_url, source_asset_url


class PublicationError(RuntimeError):
    pass


EVIDENCE_LEVELS = {"validation", "signed"}
MANDATORY_PACKAGE_RECEIPTS = {
    "binary-build",
    "binary-smoke",
    "package-verification",
}


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: pathlib.Path, label: str) -> dict:
    if not path.is_file() or path.is_symlink():
        raise PublicationError(f"missing or invalid {label}: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise PublicationError(f"invalid {label}: {error}") from error
    if not isinstance(value, dict):
        raise PublicationError(f"invalid {label}: root must be an object")
    return value


def regular_asset(root: pathlib.Path, name: object, label: str) -> pathlib.Path:
    if not isinstance(name, str):
        raise PublicationError(f"invalid {label} file name")
    relative = pathlib.PurePosixPath(name)
    if relative.is_absolute() or len(relative.parts) != 1 or relative.name != name:
        raise PublicationError(f"unsafe {label} file name: {name}")
    path = root / name
    if not path.is_file() or path.is_symlink() or path.resolve().parent != root.resolve():
        raise PublicationError(f"missing or invalid {label}: {path}")
    return path


def require_hash(path: pathlib.Path, expected: object, label: str) -> str:
    actual = sha256(path)
    if not isinstance(expected, str) or expected != actual:
        raise PublicationError(f"{label} hash mismatch: {path.name}")
    return actual


def verify_sha256_sidecar(
    root: pathlib.Path, asset: pathlib.Path, label: str, *, required: bool = True
) -> str | None:
    sidecar = root / f"{asset.name}.sha256"
    if not sidecar.exists() and not required:
        return None
    sidecar = regular_asset(root, sidecar.name, f"{label} checksum sidecar")
    expected = f"{sha256(asset)}  {asset.name}\n"
    if sidecar.read_text(encoding="utf-8") != expected:
        raise PublicationError(f"{label} checksum sidecar mismatch: {sidecar.name}")
    return sidecar.name


def verify_component(
    component: str,
    record: object,
    assets_root: pathlib.Path,
    tag: str,
    base_url: str,
) -> tuple[str, set[str]]:
    if not isinstance(record, dict):
        raise PublicationError(f"invalid source component record: {component}")
    receipt_record = record.get("source_set_receipt")
    archive_record = record.get("source_archive")
    if not isinstance(receipt_record, dict) or not isinstance(archive_record, dict):
        raise PublicationError(f"source component lacks receipt/archive binding: {component}")

    receipt_path = regular_asset(
        assets_root, receipt_record.get("file_name"), f"{component} source-set receipt"
    )
    receipt_hash = require_hash(
        receipt_path, receipt_record.get("sha256"), f"{component} source receipt"
    )
    referenced_assets = {receipt_path.name}
    receipt_sidecar = verify_sha256_sidecar(
        assets_root, receipt_path, f"{component} source-set receipt"
    )
    if receipt_sidecar is not None:
        referenced_assets.add(receipt_sidecar)
    receipt = read_json(receipt_path, f"{component} source-set receipt")
    if (
        receipt.get("schema_version") != 1
        or receipt.get("receipt_kind") != "component-source-set"
        or receipt.get("component") != component
    ):
        raise PublicationError(f"invalid source-set receipt identity: {component}")
    for field in ("lock_sha256", "patch_chain_sha256"):
        if re.fullmatch(r"[0-9a-f]{64}", str(receipt.get(field, ""))) is None:
            raise PublicationError(f"invalid source-set {field}: {component}")
    identity = receipt.get("source_identity")
    if not isinstance(identity, dict) or identity.get("tree_hash_algorithm") != "sha256":
        raise PublicationError(f"invalid source tree identity: {component}")
    for field in ("manifest_or_closure_sha256", "final_tree_sha256"):
        if re.fullmatch(r"[0-9a-f]{64}", str(identity.get(field, ""))) is None:
            raise PublicationError(f"invalid source tree {field}: {component}")
    if receipt.get("distribution") != {
        "package_required": False,
        "release_required": True,
    }:
        raise PublicationError(f"invalid source archive distribution: {component}")

    archive_path = regular_asset(
        assets_root, archive_record.get("file_name"), f"{component} source archive"
    )
    archive_hash = require_hash(
        archive_path, archive_record.get("sha256"), f"{component} source archive"
    )
    receipt_archive = receipt.get("archive")
    if not isinstance(receipt_archive, dict) or receipt_archive.get("sha256") != archive_hash:
        raise PublicationError(f"source receipt/archive hash mismatch: {component}")
    if archive_record.get("source_set_file_name") != receipt_archive.get("file_name"):
        raise PublicationError(f"source receipt/archive file-name mapping mismatch: {component}")
    expected_suffix = f"-{archive_hash[:12]}.tar.gz"
    if not archive_path.name.endswith(expected_suffix):
        raise PublicationError(f"source archive is not content-addressed: {component}")
    expected_url = source_asset_url(base_url, tag, archive_path.name)
    if archive_record.get("url") != expected_url:
        raise PublicationError(f"source archive repository/tag URL mismatch: {component}")
    referenced_assets.add(archive_path.name)
    archive_sidecar = verify_sha256_sidecar(
        assets_root, archive_path, f"{component} source archive"
    )
    if archive_sidecar is not None:
        referenced_assets.add(archive_sidecar)

    support = receipt.get("package_support_assets")
    if not isinstance(support, list) or not support:
        raise PublicationError(f"source receipt package support coverage is missing: {component}")
    for index, item in enumerate(support):
        if not isinstance(item, dict):
            raise PublicationError(f"invalid source support record {component}:{index}")
        relative = pathlib.PurePosixPath(str(item.get("file_name", "")))
        if relative.is_absolute() or ".." in relative.parts or not relative.parts:
            raise PublicationError(f"unsafe source support path {component}:{index}")
        if len(relative.parts) != 1:
            continue
        support_path = regular_asset(
            assets_root, relative.name, f"{component} source support asset {index}"
        )
        require_hash(
            support_path,
            item.get("sha256"),
            f"{component} source support asset {relative.name}",
        )
        referenced_assets.add(support_path.name)
        support_sidecar = verify_sha256_sidecar(
            assets_root,
            support_path,
            f"{component} source support asset {relative.name}",
        )
        if support_sidecar is not None:
            referenced_assets.add(support_sidecar)
    return receipt_hash, referenced_assets


def verify_package_receipt_evidence(
    receipt: dict, package_name: str, evidence_level: str
) -> None:
    required = receipt.get("required_receipts")
    verified = receipt.get("verified_receipts")
    if not isinstance(required, list) or not all(isinstance(item, str) for item in required):
        raise PublicationError(f"qualification required-receipt list is invalid: {package_name}")
    if not isinstance(verified, list) or not all(isinstance(item, str) for item in verified):
        raise PublicationError(f"qualification verified-receipt list is invalid: {package_name}")
    required_set = set(required)
    verified_set = set(verified)
    if not MANDATORY_PACKAGE_RECEIPTS.issubset(required_set):
        raise PublicationError(f"qualification receipt policy is incomplete: {package_name}")
    if not MANDATORY_PACKAGE_RECEIPTS.issubset(verified_set):
        raise PublicationError(f"qualification validation evidence is incomplete: {package_name}")
    if not verified_set.issubset(required_set):
        raise PublicationError(f"qualification verified evidence is outside policy: {package_name}")
    release_qualified = receipt.get("release_qualified")
    if not isinstance(release_qualified, bool):
        raise PublicationError(f"qualification release claim is invalid: {package_name}")
    if evidence_level == "signed" and (
        release_qualified is not True or verified_set != required_set
    ):
        raise PublicationError(f"qualification receipt is not signed/release-qualified: {package_name}")


def verify_signed_receipt(
    package: dict,
    qualification_receipt: dict,
    package_path: pathlib.Path,
    package_hash: str,
    assets_root: pathlib.Path,
    kind: str,
) -> None:
    record = package.get(f"{kind}_receipt")
    if not isinstance(record, dict):
        raise PublicationError(f"signed evidence lacks {kind} receipt: {package_path.name}")
    path = regular_asset(assets_root, record.get("file_name"), f"{kind} receipt")
    require_hash(path, record.get("sha256"), f"{kind} receipt")
    receipt = read_json(path, f"{kind} receipt")
    if receipt.get("receipt_kind") != kind or receipt.get("status") != "passed":
        raise PublicationError(f"invalid {kind} receipt: {package_path.name}")
    identity_fields = (
        ("identity",)
        if kind == "signing"
        else ("team_id", "submission_id")
    )
    if any(
        not isinstance(receipt.get(field), str) or not receipt[field].strip()
        for field in identity_fields
    ):
        raise PublicationError(f"{kind} receipt identity is missing: {package_path.name}")
    if receipt.get("target") != qualification_receipt.get("target"):
        raise PublicationError(f"{kind} receipt target mismatch: {package_path.name}")
    if receipt.get("package_sha256", receipt.get("dmg_sha256")) != package_hash:
        raise PublicationError(f"{kind} receipt package hash mismatch: {package_path.name}")
    if receipt.get("source_helper_sha256") != qualification_receipt.get(
        "source_helper_sha256"
    ):
        raise PublicationError(f"{kind} receipt source-helper binding mismatch: {package_path.name}")
    if receipt.get("signed_helper_sha256") != qualification_receipt.get("helper_sha256"):
        raise PublicationError(f"{kind} receipt helper binding mismatch: {package_path.name}")


def verify_manifest(
    manifest_path: pathlib.Path,
    assets_root: pathlib.Path,
    expected_channel: str,
    expected_evidence_level: str,
    expected_tag: str,
    expected_version: str,
    expected_commit: str,
    expected_base_url: str,
) -> None:
    document = read_json(manifest_path, "source publication manifest")
    if document.get("schema_version") != 1 or document.get("manifest_kind") != "klogg-source-publication":
        raise PublicationError("unsupported source publication manifest schema")
    if document.get("channel") != expected_channel:
        raise PublicationError("publication channel mismatch")
    if (
        expected_evidence_level not in EVIDENCE_LEVELS
        or document.get("evidence_level") != expected_evidence_level
    ):
        raise PublicationError("publication evidence level mismatch")
    release = document.get("release")
    if not isinstance(release, dict):
        raise PublicationError("publication release identity is missing")
    expected_mutable = expected_channel == "continuous"
    expected = {
        "tag": expected_tag,
        "version": expected_version,
        "commit": expected_commit,
        "mutable": expected_mutable,
    }
    for field, value in expected.items():
        if release.get(field) != value:
            raise PublicationError(f"publication {field} mismatch")

    components = document.get("components")
    if not isinstance(components, dict) or set(components) != {"adb-helper", "ios-native"}:
        raise PublicationError("publication source component coverage mismatch")
    component_results = {
        component: verify_component(
            component,
            record,
            assets_root,
            expected_tag,
            normalize_base_url(expected_base_url),
        )
        for component, record in components.items()
    }
    source_set_hashes = {
        component: result[0] for component, result in component_results.items()
    }

    packages = document.get("packages")
    if not isinstance(packages, list) or not packages:
        raise PublicationError("publication package set is empty")
    seen_packages = set()
    for index, package in enumerate(packages):
        if not isinstance(package, dict):
            raise PublicationError(f"invalid package record {index}")
        package_path = regular_asset(
            assets_root, package.get("file_name"), f"package {index}"
        )
        if package_path.name in seen_packages:
            raise PublicationError(f"duplicate publication package: {package_path.name}")
        seen_packages.add(package_path.name)
        package_hash = require_hash(
            package_path, package.get("sha256"), f"package {package_path.name}"
        )

        qualification = package.get("qualification_receipt")
        if not isinstance(qualification, dict):
            raise PublicationError(f"package lacks qualification receipt: {package_path.name}")
        qualification_path = regular_asset(
            assets_root,
            qualification.get("file_name"),
            f"qualification receipt for {package_path.name}",
        )
        require_hash(
            qualification_path,
            qualification.get("sha256"),
            f"package qualification receipt {package_path.name}",
        )
        qualification_receipt = read_json(
            qualification_path, f"qualification receipt for {package_path.name}"
        )
        if qualification_receipt.get("receipt_kind") != "package-verification":
            raise PublicationError(
                f"invalid qualification receipt kind: {package_path.name}"
            )
        verify_package_receipt_evidence(
            qualification_receipt, package_path.name, expected_evidence_level
        )
        qualified_packages = qualification_receipt.get("packages")
        if not isinstance(qualified_packages, list):
            raise PublicationError(
                f"qualification receipt package list is invalid: {package_path.name}"
            )
        matching_packages = [
            item
            for item in qualified_packages
            if isinstance(item, dict)
            and item.get("name") == package_path.name
            and item.get("sha256") == sha256(package_path)
        ]
        if len(matching_packages) != 1:
            raise PublicationError(
                f"qualification receipt package filename/hash mismatch: {package_path.name}"
            )

        bound_sources = package.get("source_sets")
        if not isinstance(bound_sources, dict) or not bound_sources:
            raise PublicationError(f"package source-set binding is missing: {package_path.name}")
        if package_path.suffix.lower() == ".dmg":
            required_sources = {"adb-helper", "ios-native"}
        elif package_path.suffix.lower() in (".deb", ".appimage", ".exe", ".zip"):
            required_sources = {"adb-helper"}
        else:
            raise PublicationError(
                f"package type has no source coverage policy: {package_path.name}"
            )
        if set(bound_sources) != required_sources:
            raise PublicationError(f"package source component mismatch: {package_path.name}")
        if qualification_receipt.get("source_set_receipt_sha256") != bound_sources[
            "adb-helper"
        ]:
            raise PublicationError(
                f"ADB qualification source-set binding mismatch: {package_path.name}"
            )

        ios_qualification = package.get("ios_qualification_receipt")
        if "ios-native" in required_sources:
            if not isinstance(ios_qualification, dict):
                raise PublicationError(
                    f"iOS qualification receipt is missing: {package_path.name}"
                )
            ios_qualification_path = regular_asset(
                assets_root,
                ios_qualification.get("file_name"),
                f"iOS qualification receipt for {package_path.name}",
            )
            require_hash(
                ios_qualification_path,
                ios_qualification.get("sha256"),
                f"iOS qualification receipt {package_path.name}",
            )
            ios_receipt = read_json(
                ios_qualification_path,
                f"iOS qualification receipt for {package_path.name}",
            )
            ios_package_record = package.get("ios_package_receipt")
            if not isinstance(ios_package_record, dict):
                raise PublicationError(
                    f"iOS package receipt is missing: {package_path.name}"
                )
            ios_package_path = regular_asset(
                assets_root,
                ios_package_record.get("file_name"),
                f"iOS package receipt for {package_path.name}",
            )
            ios_package_hash = require_hash(
                ios_package_path,
                ios_package_record.get("sha256"),
                f"iOS package receipt {package_path.name}",
            )
            ios_package_receipt = read_json(
                ios_package_path, f"iOS package receipt for {package_path.name}"
            )
            if (
                ios_package_receipt.get("receipt_kind") != "ios-native-package"
                or ios_package_receipt.get("status") != "passed"
                or ios_package_receipt.get("source_set_receipt_sha256")
                != bound_sources["ios-native"]
                or ios_receipt.get("ios_native_package_receipt_sha256")
                != ios_package_hash
            ):
                raise PublicationError(
                    f"iOS package receipt evidence mismatch: {package_path.name}"
                )
            if (
                ios_receipt.get("receipt_kind") != "ios-native-package-verification"
                or ios_receipt.get("qualification") != expected_evidence_level
                or not isinstance(ios_receipt.get("release_qualified"), bool)
                or (
                    expected_evidence_level == "signed"
                    and ios_receipt.get("release_qualified") is not True
                )
                or ios_receipt.get("source_set_receipt_sha256")
                != bound_sources["ios-native"]
                or re.fullmatch(
                    r"[0-9a-f]{64}",
                    str(ios_receipt.get("ios_native_package_receipt_sha256", "")),
                )
                is None
            ):
                raise PublicationError(
                    f"iOS qualification source-set binding mismatch: {package_path.name}"
                )
            ios_packages = ios_receipt.get("packages")
            if not isinstance(ios_packages, list):
                raise PublicationError(
                    f"iOS qualification package list is invalid: {package_path.name}"
                )
            ios_matches = [
                item
                for item in ios_packages
                if isinstance(item, dict)
                and item.get("name") == package_path.name
                and item.get("sha256") == sha256(package_path)
            ]
            if len(ios_matches) != 1:
                raise PublicationError(
                    f"iOS qualification package filename/hash mismatch: {package_path.name}"
                )
        elif (
            ios_qualification is not None
            or package.get("ios_package_receipt") is not None
        ):
            raise PublicationError(
                f"unexpected iOS qualification receipt: {package_path.name}"
            )

        if expected_evidence_level == "signed" and package_path.suffix.lower() == ".dmg":
            verify_signed_receipt(
                package,
                qualification_receipt,
                package_path,
                package_hash,
                assets_root,
                "signing",
            )
            verify_signed_receipt(
                package,
                qualification_receipt,
                package_path,
                package_hash,
                assets_root,
                "notarization",
            )
        elif package.get("signing_receipt") is not None or package.get(
            "notarization_receipt"
        ) is not None:
            raise PublicationError(
                f"unexpected signed evidence for validation publication: {package_path.name}"
            )

        for component, expected_hash in bound_sources.items():
            if expected_hash != source_set_hashes[component]:
                raise PublicationError(
                    f"package source-set hash mismatch: {package_path.name} -> {component}"
                )

    referenced_publication_assets = {manifest_path.name}
    for _, referenced in component_results.values():
        referenced_publication_assets.update(referenced)
    for package in packages:
        referenced_publication_assets.add(package["qualification_receipt"]["file_name"])
        ios_qualification = package.get("ios_qualification_receipt")
        if isinstance(ios_qualification, dict):
            referenced_publication_assets.add(ios_qualification["file_name"])
        ios_package_receipt = package.get("ios_package_receipt")
        if isinstance(ios_package_receipt, dict):
            referenced_publication_assets.add(ios_package_receipt["file_name"])
        for kind in ("signing", "notarization"):
            evidence = package.get(f"{kind}_receipt")
            if isinstance(evidence, dict):
                referenced_publication_assets.add(evidence["file_name"])
    publication_assets = {
        path.name
        for path in assets_root.iterdir()
        if path.is_file()
        and (
            path.name.endswith("-source-set-receipt.json")
            or path.name.endswith("-source-set-receipt.json.sha256")
            or re.fullmatch(r"klogg-v.+-.+-source-[0-9a-f]{12}\.tar\.gz(?:\.sha256)?", path.name)
            or path.name.endswith(".qualification.json")
            or path.name.endswith(".ios-qualification.json")
            or path.name.endswith(".ios-package.json")
            or path.name.endswith(".signing.json")
            or path.name.endswith(".notarization.json")
            or path.name.endswith("source-publication-manifest.json")
        )
    }
    unreferenced = publication_assets - referenced_publication_assets
    if unreferenced:
        raise PublicationError(
            "unreferenced source/publication assets: " + ", ".join(sorted(unreferenced))
        )

    published_packages = {
        path.name
        for path in assets_root.iterdir()
        if path.is_file()
        and "-x86-" not in path.name.lower()
        and (
            path.suffix.lower() in (".dmg", ".deb", ".appimage", ".exe")
            or path.name.lower().endswith("-portable.zip")
        )
    }
    unreferenced_packages = published_packages - seen_packages
    if unreferenced_packages:
        raise PublicationError(
            "unreferenced or unqualified package assets: "
            + ", ".join(sorted(unreferenced_packages))
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--assets-root", required=True, type=pathlib.Path)
    parser.add_argument("--expected-channel", choices=("stable", "continuous"), required=True)
    parser.add_argument(
        "--expected-evidence-level", choices=tuple(sorted(EVIDENCE_LEVELS)), required=True
    )
    parser.add_argument("--expected-tag", required=True)
    parser.add_argument("--expected-version", required=True)
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument("--expected-base-url", required=True)
    args = parser.parse_args()
    try:
        verify_manifest(
            args.manifest,
            args.assets_root,
            args.expected_channel,
            args.expected_evidence_level,
            args.expected_tag,
            args.expected_version,
            args.expected_commit,
            args.expected_base_url,
        )
        print(
            f"verified coherent {args.expected_channel}/{args.expected_evidence_level} "
            "package/source publication: "
            f"{args.manifest}"
        )
        return 0
    except PublicationError as error:
        print(f"source publication verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
