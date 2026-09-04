#!/usr/bin/env python3
"""Verify a coherent package and external component-source publication set."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
import tarfile

from source_publication_identity import normalize_base_url, source_asset_url


class PublicationError(RuntimeError):
    pass


EVIDENCE_LEVELS = {"validation", "signed"}
MANDATORY_PACKAGE_RECEIPTS = {
    "binary-build",
    "binary-smoke",
    "package-verification",
}
SIGNED_MACOS_PACKAGE_RECEIPTS = {"signing", "notarization"}


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
    require_schema_version(
        receipt.get("schema_version"), 1, f"{component} source-set receipt"
    )
    if (
        receipt.get("receipt_kind") != "component-source-set"
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
    require_source_distribution(receipt.get("distribution"), component)

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
    receipt: dict,
    package_name: str,
    evidence_level: str,
    additional_required_receipts: set[str] | None = None,
) -> None:
    required = receipt.get("required_receipts")
    verified = receipt.get("verified_receipts")
    if not isinstance(required, list) or not all(isinstance(item, str) for item in required):
        raise PublicationError(f"qualification required-receipt list is invalid: {package_name}")
    if not isinstance(verified, list) or not all(isinstance(item, str) for item in verified):
        raise PublicationError(f"qualification verified-receipt list is invalid: {package_name}")
    required_set = set(required)
    verified_set = set(verified)
    required_policy = MANDATORY_PACKAGE_RECEIPTS | (
        additional_required_receipts or set()
    )
    missing_policy = sorted(required_policy - required_set)
    if missing_policy:
        raise PublicationError(
            f"qualification receipt policy is incomplete for {package_name}; "
            f"missing={missing_policy}"
        )
    missing_evidence = sorted(required_policy - verified_set)
    if missing_evidence:
        raise PublicationError(
            f"qualification validation evidence is incomplete for {package_name}; "
            f"missing={missing_evidence}"
        )
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


def verify_manifest_v1(
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
    require_schema_version(
        document.get("schema_version"), 1, "source publication manifest"
    )
    if document.get("manifest_kind") != "klogg-source-publication":
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
        if field == "mutable":
            require_boolean(release.get(field), value, "publication mutable")
        elif release.get(field) != value:
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
            qualification_receipt,
            package_path.name,
            expected_evidence_level,
            (
                SIGNED_MACOS_PACKAGE_RECEIPTS
                if expected_evidence_level == "signed"
                and package_path.suffix.lower() == ".dmg"
                else None
            ),
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



MANIFEST_V2_FIELDS = {
    "schema_version",
    "manifest_kind",
    "channel",
    "evidence_level",
    "release",
    "components",
    "packages",
    "support_assets",
    "evidence_archive",
    "checksums",
}
RELEASE_V2_FIELDS = {
    "tag",
    "version",
    "commit",
    "mutable",
    "public_name",
    "page_url",
    "direct_asset_urls_are_archival",
}
EVIDENCE_KINDS = {
    "qualification",
    "ios-qualification",
    "ios-package",
    "signing",
    "notarization",
}
SUPPORT_DISPLAY_NAMES = {
    "adb-helper-licenses.tar.gz": "ADB helper licenses",
    "adb-helper-notices.tar.gz": "ADB helper notices",
    "adb-helper-sbom.spdx.json": "ADB helper SBOM",
    "ADB-HELPER-SOURCE-OFFER.txt": "ADB helper source offer",
    "adb-helper-source-manifest.json": "ADB helper source manifest",
    "ios-native-source-offer.txt": "iOS native source offer",
    "ios-native-lgpl-replacement.txt": "iOS native LGPL replacement guide",
    "NOTICE-ios-native.txt": "iOS native notices",
}
COMPONENT_DISPLAY_NAMES = {
    "adb-helper": (
        "ADB helper corresponding source",
        "ADB helper source-set receipt",
    ),
    "ios-native": (
        "iOS native corresponding source",
        "iOS native source-set receipt",
    ),
}


def require_fields(value: object, expected: set[str], label: str) -> dict:
    if not isinstance(value, dict):
        raise PublicationError(f"invalid {label}: expected an object")
    actual = set(value)
    if actual != expected:
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)
        raise PublicationError(
            f"invalid {label} schema fields; missing={missing}, extra={extra}"
        )
    return value


def require_sha256(value: object, label: str) -> str:
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise PublicationError(f"invalid {label} SHA-256")
    return value


def require_schema_version(value: object, expected: int, label: str) -> None:
    if type(value) is not int or value != expected:
        raise PublicationError(f"invalid {label} schema version integer")


def require_boolean(value: object, expected: bool, label: str) -> None:
    if type(value) is not bool or value is not expected:
        raise PublicationError(f"invalid {label} boolean")


def require_source_distribution(value: object, component: str) -> None:
    distribution = require_fields(
        value,
        {"package_required", "release_required"},
        f"source archive distribution for {component}",
    )
    require_boolean(
        distribution["package_required"],
        False,
        f"source archive package_required for {component}",
    )
    require_boolean(
        distribution["release_required"],
        True,
        f"source archive release_required for {component}",
    )


def supported_package_display(version: str) -> dict[str, tuple[str, str]]:
    return {
        f"klogg-{version}-x64-Qt6-vs-avx2-setup.exe": (
            "Windows",
            "Windows x64 installer (Qt 6, Vectorscan AVX2)",
        ),
        f"klogg-{version}-x64-Qt6-vs-avx2-portable.zip": (
            "Windows",
            "Windows x64 portable (Qt 6, Vectorscan AVX2)",
        ),
        f"klogg-{version}-jammy-vs-gen.deb": (
            "Linux",
            "Ubuntu 22.04 (Jammy, Vectorscan generic)",
        ),
        f"klogg-{version}-noble-vs-gen.deb": (
            "Linux",
            "Ubuntu 24.04 (Noble, Vectorscan generic)",
        ),
        f"klogg-{version}-resolute-vs-gen.deb": (
            "Linux",
            "Ubuntu 26.04 (Resolute, Vectorscan generic)",
        ),
        f"klogg-{version}-appimage-vs-gen.AppImage": (
            "Linux",
            "AppImage (Vectorscan generic)",
        ),
        f"klogg-{version}-mac-x64-vs-gen.dmg": (
            "macOS",
            "Intel (x64, Vectorscan generic)",
        ),
        f"klogg-{version}-mac-arm64-vs-arm64.dmg": (
            "macOS",
            "Apple Silicon (ARM64, Vectorscan)",
        ),
    }


def safe_evidence_name(name: object, label: str) -> str:
    if not isinstance(name, str):
        raise PublicationError(f"unsafe evidence member name for {label}")
    relative = pathlib.PurePosixPath(name)
    if (
        relative.is_absolute()
        or str(relative) != name
        or not relative.parts
        or any(part in ("", ".", "..") for part in relative.parts)
    ):
        raise PublicationError(f"unsafe evidence member name: {name}")
    return name


def parse_evidence_archive(
    path: pathlib.Path, record: dict
) -> tuple[dict[str, bytes], dict[tuple[str, str], dict]]:
    require_fields(
        record,
        {"file_name", "sha256", "index_member", "index_sha256"},
        "evidence archive binding",
    )
    if record["file_name"] != "klogg-release-evidence.tar":
        raise PublicationError("invalid evidence archive file name")
    require_hash(path, record["sha256"], "release evidence archive")
    if record["index_member"] != "index.json":
        raise PublicationError("invalid evidence index member binding")

    try:
        with tarfile.open(path, "r:") as archive:
            members = archive.getmembers()
            names = [member.name for member in members]
            if len(names) != len(set(names)):
                raise PublicationError("duplicate evidence archive member")
            if "index.json" not in names:
                raise PublicationError("missing evidence archive index")
            for member in members:
                safe_evidence_name(member.name, "archive")
                if not member.isfile():
                    raise PublicationError(
                        f"evidence archive member must be a regular file: {member.name}"
                    )
                if (
                    member.mode != 0o644
                    or member.uid != 0
                    or member.gid != 0
                    or member.mtime != 0
                    or member.uname != ""
                    or member.gname != ""
                ):
                    raise PublicationError(
                        f"evidence archive metadata is not deterministic: {member.name}"
                    )
            member_bytes: dict[str, bytes] = {}
            for member in members:
                stream = archive.extractfile(member)
                if stream is None:
                    raise PublicationError(
                        f"evidence archive member cannot be read: {member.name}"
                    )
                member_bytes[member.name] = stream.read()
    except (tarfile.TarError, OSError) as error:
        raise PublicationError(f"invalid evidence archive: {error}") from error

    index_bytes = member_bytes["index.json"]
    if hashlib.sha256(index_bytes).hexdigest() != record["index_sha256"]:
        raise PublicationError("evidence index hash mismatch")
    try:
        index = json.loads(index_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise PublicationError(f"invalid evidence index: {error}") from error
    require_fields(index, {"schema_version", "index_kind", "receipts", "references"}, "evidence index")
    require_schema_version(index["schema_version"], 1, "evidence index")
    if index["index_kind"] != "klogg-release-evidence":
        raise PublicationError("invalid evidence index identity")
    receipts = index["receipts"]
    references = index["references"]
    if not isinstance(receipts, list) or not isinstance(references, list):
        raise PublicationError("invalid evidence index receipt/reference lists")

    indexed_names: list[str] = []
    content_hashes: set[str] = set()
    receipt_bytes: dict[str, bytes] = {}
    for offset, raw_receipt in enumerate(receipts):
        receipt = require_fields(
            raw_receipt, {"member", "sha256", "size"}, f"evidence receipt {offset}"
        )
        member_name = safe_evidence_name(receipt["member"], f"receipt {offset}")
        digest = require_sha256(receipt["sha256"], f"evidence receipt {member_name}")
        if digest in content_hashes:
            raise PublicationError("duplicate evidence receipt content identity")
        content_hashes.add(digest)
        if member_name in indexed_names:
            raise PublicationError("duplicate evidence receipt member")
        indexed_names.append(member_name)
        content = member_bytes.get(member_name)
        if content is None:
            raise PublicationError(f"missing indexed evidence receipt: {member_name}")
        if hashlib.sha256(content).hexdigest() != digest:
            raise PublicationError(f"evidence receipt hash mismatch: {member_name}")
        if not isinstance(receipt["size"], int) or receipt["size"] != len(content):
            raise PublicationError(f"evidence receipt size mismatch: {member_name}")
        if member_name != f"receipts/{digest}.json":
            raise PublicationError(
                f"evidence receipt member is not content-addressed: {member_name}"
            )
        receipt_bytes[member_name] = content

    if indexed_names != sorted(indexed_names):
        raise PublicationError("evidence receipt index order is not deterministic")
    expected_members = ["index.json", *indexed_names]
    if list(member_bytes) != expected_members:
        extra = sorted(set(member_bytes) - set(expected_members))
        missing = sorted(set(expected_members) - set(member_bytes))
        raise PublicationError(
            f"evidence archive/index coverage mismatch; missing={missing}, extra={extra}"
        )

    associations: dict[tuple[str, str], dict] = {}
    reference_sort_keys: list[tuple[str, str, str]] = []
    referenced_members: set[str] = set()
    for offset, raw_reference in enumerate(references):
        reference = require_fields(
            raw_reference,
            {"package", "kind", "source_file_name", "member", "sha256"},
            f"evidence reference {offset}",
        )
        package = regular_asset_name(reference["package"], "evidence package")
        kind = reference["kind"]
        if not isinstance(kind, str) or kind not in EVIDENCE_KINDS:
            raise PublicationError(f"invalid evidence kind: {kind}")
        source_file_name = regular_asset_name(
            reference["source_file_name"], "evidence source file"
        )
        member_name = safe_evidence_name(reference["member"], "reference")
        digest = require_sha256(reference["sha256"], "evidence reference")
        if member_name not in receipt_bytes:
            raise PublicationError(f"evidence reference names missing receipt: {member_name}")
        if hashlib.sha256(receipt_bytes[member_name]).hexdigest() != digest:
            raise PublicationError(f"evidence reference hash mismatch: {member_name}")
        association = (package, kind)
        if association in associations:
            raise PublicationError(
                f"duplicate evidence logical association: {package}:{kind}"
            )
        associations[association] = reference
        referenced_members.add(member_name)
        reference_sort_keys.append((package, kind, source_file_name))
    if reference_sort_keys != sorted(reference_sort_keys):
        raise PublicationError("evidence reference order is not deterministic")
    unreferenced = set(receipt_bytes) - referenced_members
    if unreferenced:
        raise PublicationError(
            "unreferenced evidence receipts: " + ", ".join(sorted(unreferenced))
        )
    return receipt_bytes, associations


def regular_asset_name(value: object, label: str) -> str:
    if not isinstance(value, str):
        raise PublicationError(f"invalid {label} file name")
    relative = pathlib.PurePosixPath(value)
    if relative.is_absolute() or len(relative.parts) != 1 or relative.name != value:
        raise PublicationError(f"unsafe {label} file name: {value}")
    return value


def evidence_json(
    package_name: str,
    kind: str,
    evidence: dict,
    receipt_bytes: dict[str, bytes],
    associations: dict[tuple[str, str], dict],
) -> dict:
    record = require_fields(
        evidence.get(kind), {"member", "sha256"}, f"{kind} evidence for {package_name}"
    )
    member = safe_evidence_name(record["member"], kind)
    digest = require_sha256(record["sha256"], f"{kind} evidence")
    reference = associations.get((package_name, kind))
    if reference is None:
        raise PublicationError(f"missing evidence index association: {package_name}:{kind}")
    if reference["member"] != member or reference["sha256"] != digest:
        raise PublicationError(f"evidence binding mismatch: {package_name}:{kind}")
    content = receipt_bytes.get(member)
    if content is None or hashlib.sha256(content).hexdigest() != digest:
        raise PublicationError(f"evidence receipt hash mismatch: {package_name}:{kind}")
    try:
        value = json.loads(content.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise PublicationError(
            f"invalid {kind} evidence JSON for {package_name}: {error}"
        ) from error
    if not isinstance(value, dict):
        raise PublicationError(f"invalid {kind} evidence root for {package_name}")
    return value


def verify_signed_receipt_v2(
    receipt: dict,
    qualification: dict,
    package_name: str,
    package_hash: str,
    kind: str,
) -> None:
    if receipt.get("receipt_kind") != kind or receipt.get("status") != "passed":
        raise PublicationError(f"invalid {kind} receipt: {package_name}")
    identity_fields = ("identity",) if kind == "signing" else ("team_id", "submission_id")
    if any(
        not isinstance(receipt.get(field), str) or not receipt[field].strip()
        for field in identity_fields
    ):
        raise PublicationError(f"{kind} receipt identity is missing: {package_name}")
    if receipt.get("target") != qualification.get("target"):
        raise PublicationError(f"{kind} receipt target mismatch: {package_name}")
    if receipt.get("package_sha256", receipt.get("dmg_sha256")) != package_hash:
        raise PublicationError(f"{kind} receipt package hash mismatch: {package_name}")
    if receipt.get("source_helper_sha256") != qualification.get("source_helper_sha256"):
        raise PublicationError(f"{kind} receipt source-helper binding mismatch: {package_name}")
    if receipt.get("signed_helper_sha256") != qualification.get("helper_sha256"):
        raise PublicationError(f"{kind} receipt helper binding mismatch: {package_name}")


def verify_sha256sums(
    assets_root: pathlib.Path, record: object, expected_assets: set[str]
) -> None:
    checksums = require_fields(
        record, {"file_name", "covered_asset_count", "format"}, "checksums binding"
    )
    if checksums["file_name"] != "SHA256SUMS" or checksums["format"] != "sha256sum-binary-v1":
        raise PublicationError("invalid SHA256SUMS binding")
    checksum_path = regular_asset(assets_root, checksums["file_name"], "SHA256SUMS")
    try:
        lines = checksum_path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise PublicationError("malformed checksum file encoding") from error
    parsed: dict[str, str] = {}
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64}) \*([^/\\]+)", line)
        if match is None:
            if ".." in line or "/" in line or "\\" in line:
                raise PublicationError("unsafe checksum path")
            raise PublicationError("malformed checksum format")
        digest, name = match.groups()
        regular_asset_name(name, "checksum")
        if name in parsed:
            raise PublicationError(f"duplicate checksum record: {name}")
        parsed[name] = digest
    if list(parsed) != sorted(parsed):
        raise PublicationError("SHA256SUMS records are not sorted")
    if checksums["covered_asset_count"] != len(expected_assets):
        raise PublicationError("checksum coverage count mismatch")
    if set(parsed) != expected_assets:
        missing = sorted(expected_assets - set(parsed))
        extra = sorted(set(parsed) - expected_assets)
        raise PublicationError(
            f"checksum coverage mismatch; missing={missing}, unlisted={extra}"
        )
    for name, digest in parsed.items():
        path = regular_asset(assets_root, name, "checksummed asset")
        if sha256(path) != digest:
            raise PublicationError(f"checksum hash mismatch: {name}")


def verify_component_v2(
    component: str,
    record: object,
    assets_root: pathlib.Path,
    tag: str,
    base_url: str,
    support_records: dict[str, dict],
) -> tuple[str, set[str]]:
    component_record = require_fields(
        record,
        {"display_name", "source_set_receipt", "source_archive"},
        f"source component {component}",
    )
    expected_source_display, expected_receipt_display = COMPONENT_DISPLAY_NAMES[component]
    if component_record["display_name"] != expected_source_display:
        raise PublicationError(f"invalid source component display name: {component}")
    receipt_record = require_fields(
        component_record["source_set_receipt"],
        {"display_name", "file_name", "sha256"},
        f"{component} source-set receipt binding",
    )
    if receipt_record["display_name"] != expected_receipt_display:
        raise PublicationError(f"invalid source-set receipt display name: {component}")
    archive_record = require_fields(
        component_record["source_archive"],
        {"file_name", "source_set_file_name", "sha256", "url"},
        f"{component} source archive binding",
    )
    receipt_path = regular_asset(
        assets_root, receipt_record["file_name"], f"{component} source-set receipt"
    )
    receipt_hash = require_hash(
        receipt_path, receipt_record["sha256"], f"{component} source-set receipt"
    )
    receipt = read_json(receipt_path, f"{component} source-set receipt")
    require_schema_version(
        receipt.get("schema_version"), 1, f"{component} source-set receipt"
    )
    if (
        receipt.get("receipt_kind") != "component-source-set"
        or receipt.get("component") != component
    ):
        raise PublicationError(f"invalid source-set receipt identity: {component}")
    for field in ("lock_sha256", "patch_chain_sha256"):
        require_sha256(receipt.get(field), f"{component} source-set {field}")
    identity = receipt.get("source_identity")
    if not isinstance(identity, dict) or identity.get("tree_hash_algorithm") != "sha256":
        raise PublicationError(f"invalid source tree identity: {component}")
    for field in ("manifest_or_closure_sha256", "final_tree_sha256"):
        require_sha256(identity.get(field), f"{component} source tree {field}")
    require_source_distribution(receipt.get("distribution"), component)

    archive_path = regular_asset(
        assets_root, archive_record["file_name"], f"{component} source archive"
    )
    archive_hash = require_hash(
        archive_path, archive_record["sha256"], f"{component} source archive"
    )
    receipt_archive = receipt.get("archive")
    if not isinstance(receipt_archive, dict) or receipt_archive.get("sha256") != archive_hash:
        raise PublicationError(f"source receipt/archive hash mismatch: {component}")
    if archive_record["source_set_file_name"] != receipt_archive.get("file_name"):
        raise PublicationError(f"source receipt/archive file-name mapping mismatch: {component}")
    if not archive_path.name.endswith(f"-{archive_hash[:12]}.tar.gz"):
        raise PublicationError(f"source archive is not content-addressed: {component}")
    if archive_record["url"] != source_asset_url(base_url, tag, archive_path.name):
        raise PublicationError(f"source archive repository/tag URL mismatch: {component}")

    support = receipt.get("package_support_assets")
    if not isinstance(support, list) or not support:
        raise PublicationError(f"source receipt package support coverage is missing: {component}")
    referenced_support: set[str] = set()
    for offset, item in enumerate(support):
        if not isinstance(item, dict):
            raise PublicationError(f"invalid source support record {component}:{offset}")
        relative = pathlib.PurePosixPath(str(item.get("file_name", "")))
        if relative.is_absolute() or ".." in relative.parts or not relative.parts:
            raise PublicationError(f"unsafe source support path {component}:{offset}")
        if len(relative.parts) != 1:
            continue
        support_record = support_records.get(relative.name)
        if support_record is None:
            raise PublicationError(f"missing manifest source support asset: {relative.name}")
        path = regular_asset(assets_root, relative.name, f"{component} support asset")
        digest = require_hash(path, item.get("sha256"), f"{component} support asset")
        if support_record["sha256"] != digest:
            raise PublicationError(f"support manifest hash mismatch: {relative.name}")
        referenced_support.add(relative.name)
    return receipt_hash, {receipt_path.name, archive_path.name, *referenced_support}


def verify_manifest_v2(
    manifest_path: pathlib.Path,
    assets_root: pathlib.Path,
    expected_channel: str,
    expected_evidence_level: str,
    expected_tag: str,
    expected_version: str,
    expected_commit: str,
    expected_base_url: str,
) -> None:
    document = read_json(manifest_path, "release publication manifest")
    require_fields(document, MANIFEST_V2_FIELDS, "release publication manifest")
    require_schema_version(
        document["schema_version"], 2, "release publication manifest"
    )
    if document["manifest_kind"] != "klogg-release-publication":
        raise PublicationError("unsupported release publication manifest schema")
    if document["channel"] != expected_channel:
        raise PublicationError("publication channel mismatch")
    if expected_evidence_level not in EVIDENCE_LEVELS or document["evidence_level"] != expected_evidence_level:
        raise PublicationError("publication evidence level mismatch")
    release = require_fields(document["release"], RELEASE_V2_FIELDS, "release identity")
    base_url = normalize_base_url(expected_base_url)
    expected_release = {
        "tag": expected_tag,
        "version": expected_version,
        "commit": expected_commit,
        "mutable": expected_channel == "continuous",
        "public_name": (
            f"Continuous Build {expected_version}"
            if expected_channel == "continuous"
            else f"Release v{expected_version}"
        ),
        "page_url": f"{base_url}/releases/tag/{expected_tag}",
        "direct_asset_urls_are_archival": expected_channel == "stable",
    }
    for field, value in expected_release.items():
        if field in {"mutable", "direct_asset_urls_are_archival"}:
            require_boolean(release[field], value, f"publication {field}")
        elif release[field] != value:
            if field == "public_name" and "candidate" in str(release[field]).lower():
                raise PublicationError("candidate name must not be used as the public release name")
            raise PublicationError(f"publication {field} mismatch")

    support_assets = document["support_assets"]
    if not isinstance(support_assets, list):
        raise PublicationError("invalid support asset list")
    support_records: dict[str, dict] = {}
    for offset, raw_record in enumerate(support_assets):
        record = require_fields(
            raw_record, {"display_name", "file_name", "sha256"}, f"support asset {offset}"
        )
        name = regular_asset_name(record["file_name"], "support asset")
        if name in support_records:
            raise PublicationError(f"duplicate support asset: {name}")
        if SUPPORT_DISPLAY_NAMES.get(name) != record["display_name"]:
            raise PublicationError(f"invalid support asset display or inventory: {name}")
        path = regular_asset(assets_root, name, "support asset")
        require_hash(path, record["sha256"], f"support asset {name}")
        support_records[name] = record
    if set(support_records) != set(SUPPORT_DISPLAY_NAMES):
        raise PublicationError("publication support asset coverage mismatch")

    components = document["components"]
    if not isinstance(components, dict) or set(components) != set(COMPONENT_DISPLAY_NAMES):
        raise PublicationError("publication source component coverage mismatch")
    component_results = {
        component: verify_component_v2(
            component, record, assets_root, expected_tag, base_url, support_records
        )
        for component, record in components.items()
    }
    if set().union(*(result[1] & set(support_records) for result in component_results.values())) != set(support_records):
        raise PublicationError("source component support association coverage mismatch")
    source_hashes = {name: result[0] for name, result in component_results.items()}

    evidence_record = require_fields(
        document["evidence_archive"],
        {"file_name", "sha256", "index_member", "index_sha256"},
        "evidence archive binding",
    )
    evidence_path = regular_asset(
        assets_root, evidence_record["file_name"], "release evidence archive"
    )
    receipt_bytes, associations = parse_evidence_archive(evidence_path, evidence_record)

    packages = document["packages"]
    if not isinstance(packages, list):
        raise PublicationError("invalid publication package list")
    expected_displays = supported_package_display(expected_version)
    package_records: dict[str, dict] = {}
    manifest_associations: set[tuple[str, str]] = set()
    for offset, raw_package in enumerate(packages):
        package = require_fields(
            raw_package,
            {"file_name", "sha256", "display", "source_sets", "evidence"},
            f"package {offset}",
        )
        name = regular_asset_name(package["file_name"], "package")
        if name in package_records:
            raise PublicationError(f"duplicate publication package: {name}")
        expected_display = expected_displays.get(name)
        display = require_fields(package["display"], {"section", "label"}, f"package display {name}")
        if expected_display is None or (display["section"], display["label"]) != expected_display:
            raise PublicationError(f"unsupported or ambiguous package mapping: {name}")
        package_path = regular_asset(assets_root, name, "package")
        package_hash = require_hash(package_path, package["sha256"], f"package {name}")
        required_sources = {"adb-helper", "ios-native"} if name.endswith(".dmg") else {"adb-helper"}
        source_sets = package["source_sets"]
        if not isinstance(source_sets, dict) or set(source_sets) != required_sources:
            raise PublicationError(f"package source component mismatch: {name}")
        for component, digest in source_sets.items():
            if digest != source_hashes[component]:
                raise PublicationError(f"package source-set hash mismatch: {name} -> {component}")

        evidence = package["evidence"]
        required_evidence = {"qualification"}
        if name.endswith(".dmg"):
            required_evidence.update({"ios-package", "ios-qualification"})
            if expected_evidence_level == "signed":
                required_evidence.update({"signing", "notarization"})
        if not isinstance(evidence, dict) or set(evidence) != required_evidence:
            raise PublicationError(f"package evidence coverage mismatch: {name}")
        manifest_associations.update((name, kind) for kind in evidence)

        qualification = evidence_json(
            name, "qualification", evidence, receipt_bytes, associations
        )
        if qualification.get("receipt_kind") != "package-verification":
            raise PublicationError(f"invalid qualification receipt kind: {name}")
        verify_package_receipt_evidence(
            qualification,
            name,
            expected_evidence_level,
            (
                SIGNED_MACOS_PACKAGE_RECEIPTS
                if expected_evidence_level == "signed" and name.endswith(".dmg")
                else None
            ),
        )
        if qualification.get("source_set_receipt_sha256") != source_sets["adb-helper"]:
            raise PublicationError(f"ADB qualification source-set binding mismatch: {name}")
        qualified_packages = qualification.get("packages")
        matches = (
            [
                item
                for item in qualified_packages
                if isinstance(item, dict)
                and item.get("name") == name
                and item.get("sha256") == package_hash
            ]
            if isinstance(qualified_packages, list)
            else []
        )
        if len(matches) != 1:
            raise PublicationError(f"qualification receipt package filename/hash mismatch: {name}")

        if name.endswith(".dmg"):
            ios_package = evidence_json(
                name, "ios-package", evidence, receipt_bytes, associations
            )
            ios_qualification = evidence_json(
                name, "ios-qualification", evidence, receipt_bytes, associations
            )
            ios_package_digest = evidence["ios-package"]["sha256"]
            if (
                ios_package.get("receipt_kind") != "ios-native-package"
                or ios_package.get("status") != "passed"
                or ios_package.get("source_set_receipt_sha256") != source_sets["ios-native"]
                or ios_qualification.get("ios_native_package_receipt_sha256") != ios_package_digest
            ):
                raise PublicationError(f"iOS package receipt evidence mismatch: {name}")
            if (
                ios_qualification.get("receipt_kind") != "ios-native-package-verification"
                or ios_qualification.get("qualification") != expected_evidence_level
                or not isinstance(ios_qualification.get("release_qualified"), bool)
                or (
                    expected_evidence_level == "signed"
                    and ios_qualification.get("release_qualified") is not True
                )
                or ios_qualification.get("source_set_receipt_sha256") != source_sets["ios-native"]
            ):
                raise PublicationError(f"iOS qualification source-set binding mismatch: {name}")
            ios_packages = ios_qualification.get("packages")
            ios_matches = (
                [
                    item
                    for item in ios_packages
                    if isinstance(item, dict)
                    and item.get("name") == name
                    and item.get("sha256") == package_hash
                ]
                if isinstance(ios_packages, list)
                else []
            )
            if len(ios_matches) != 1:
                raise PublicationError(f"iOS qualification package filename/hash mismatch: {name}")
            if expected_evidence_level == "signed":
                for kind in ("signing", "notarization"):
                    signed_receipt = evidence_json(
                        name, kind, evidence, receipt_bytes, associations
                    )
                    verify_signed_receipt_v2(
                        signed_receipt, qualification, name, package_hash, kind
                    )
        package_records[name] = package

    if set(package_records) != set(expected_displays):
        missing = sorted(set(expected_displays) - set(package_records))
        extra = sorted(set(package_records) - set(expected_displays))
        raise PublicationError(
            f"publication package mapping coverage mismatch; missing={missing}, extra={extra}"
        )
    if set(associations) != manifest_associations:
        missing = sorted(manifest_associations - set(associations))
        extra = sorted(set(associations) - manifest_associations)
        raise PublicationError(
            f"evidence logical association coverage mismatch; missing={missing}, extra={extra}"
        )

    logical_assets = {
        manifest_path.name,
        evidence_path.name,
        "SHA256SUMS",
        *support_records,
        *package_records,
    }
    for _, referenced in component_results.values():
        logical_assets.update(referenced)
    actual_regular_assets: set[str] = set()
    for path in assets_root.iterdir():
        if path.is_symlink():
            raise PublicationError(f"release publication contains symlink: {path.name}")
        if path.is_file():
            regular_asset_name(path.name, "top-level release asset")
            if path.name.endswith(".sha256"):
                raise PublicationError(f"individual checksum sidecar is forbidden: {path.name}")
            actual_regular_assets.add(path.name)
    if actual_regular_assets != logical_assets:
        missing = sorted(logical_assets - actual_regular_assets)
        extra = sorted(actual_regular_assets - logical_assets)
        raise PublicationError(
            f"unlisted or unreferenced release asset; missing={missing}, extra={extra}"
        )
    verify_sha256sums(assets_root, document["checksums"], logical_assets - {"SHA256SUMS"})


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
    root_manifest = regular_asset(
        assets_root, manifest_path.name, "source publication manifest"
    )
    if manifest_path.is_symlink() or manifest_path.resolve() != root_manifest.resolve():
        raise PublicationError(
            "source publication manifest must be the checksummed asset inside the publication root"
        )
    document = read_json(root_manifest, "source publication manifest")
    schema = document.get("schema_version")
    kind = document.get("manifest_kind")
    if type(schema) is not int:
        raise PublicationError("source publication manifest schema version must be an integer")
    if schema == 1 and kind == "klogg-source-publication":
        verify_manifest_v1(
            root_manifest,
            assets_root,
            expected_channel,
            expected_evidence_level,
            expected_tag,
            expected_version,
            expected_commit,
            expected_base_url,
        )
        return
    if schema == 2 and kind == "klogg-release-publication":
        verify_manifest_v2(
            root_manifest,
            assets_root,
            expected_channel,
            expected_evidence_level,
            expected_tag,
            expected_version,
            expected_commit,
            expected_base_url,
        )
        return
    raise PublicationError("unsupported source publication manifest schema")


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
