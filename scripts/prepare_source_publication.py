#!/usr/bin/env python3
"""Assemble one coherent package and external component-source publication set."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import shutil

from source_publication_identity import (
    normalize_base_url,
    published_source_name,
    source_asset_url,
    validate_asset_file_name,
    validate_version,
)
from verify_source_publication_manifest import PublicationError, verify_manifest


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: pathlib.Path, label: str) -> dict:
    if not path.is_file() or path.is_symlink():
        raise PublicationError(f"missing or invalid {label}: {path}")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise PublicationError(f"invalid {label}: root must be an object")
    return value


def copy_regular(source: pathlib.Path, destination: pathlib.Path, label: str) -> pathlib.Path:
    if not source.is_file() or source.is_symlink():
        raise PublicationError(f"missing or invalid {label}: {source}")
    if destination.exists():
        if sha256(destination) != sha256(source):
            raise PublicationError(f"publication file-name collision: {destination.name}")
        return destination
    shutil.copy2(source, destination)
    return destination


def publish_component(
    component: str,
    root: pathlib.Path,
    receipt_name: str,
    version: str,
    tag: str,
    base_url: str,
    output: pathlib.Path,
) -> tuple[dict, str]:
    receipt_source = root / receipt_name
    receipt = read_json(receipt_source, f"{component} source-set receipt")
    if receipt.get("receipt_kind") != "component-source-set" or receipt.get("component") != component:
        raise PublicationError(f"invalid component source-set receipt: {component}")
    archive_record = receipt.get("archive")
    if not isinstance(archive_record, dict):
        raise PublicationError(f"source-set receipt lacks archive: {component}")
    archive_source = root / str(archive_record.get("file_name", ""))
    archive_hash = sha256(archive_source)
    if archive_record.get("sha256") != archive_hash:
        raise PublicationError(f"source-set archive hash mismatch: {component}")
    published_archive_name = published_source_name(version, component, archive_hash)
    published_archive = copy_regular(
        archive_source, output / published_archive_name, f"{component} source archive"
    )
    (output / f"{published_archive_name}.sha256").write_text(
        f"{archive_hash}  {published_archive_name}\n", encoding="utf-8"
    )
    published_receipt = copy_regular(
        receipt_source, output / receipt_name, f"{component} source-set receipt"
    )
    receipt_sidecar = output / f"{published_receipt.name}.sha256"
    receipt_sidecar.write_text(
        f"{sha256(published_receipt)}  {published_receipt.name}\n", encoding="utf-8"
    )

    for item in receipt.get("package_support_assets", []):
        if not isinstance(item, dict):
            raise PublicationError(f"invalid package support asset: {component}")
        relative = pathlib.PurePosixPath(str(item.get("file_name", "")))
        if relative.is_absolute() or ".." in relative.parts or not relative.parts:
            raise PublicationError(f"unsafe package support asset: {component}")
        # GitHub Release assets are flat. Publish top-level legal/source aids;
        # license subtrees remain in the corresponding-source archive and installers.
        if len(relative.parts) != 1:
            continue
        source = root / relative
        if sha256(source) != item.get("sha256"):
            raise PublicationError(f"package support asset hash mismatch: {component}:{relative}")
        published_support = copy_regular(
            source, output / relative.name, f"{component} package support asset"
        )
        (output / f"{published_support.name}.sha256").write_text(
            f"{sha256(published_support)}  {published_support.name}\n",
            encoding="utf-8",
        )

    receipt_hash = sha256(published_receipt)
    return (
        {
            "source_set_receipt": {
                "file_name": published_receipt.name,
                "sha256": receipt_hash,
            },
            "source_archive": {
                "file_name": published_archive.name,
                "source_set_file_name": archive_source.name,
                "sha256": archive_hash,
                "url": source_asset_url(base_url, tag, published_archive.name),
            },
        },
        receipt_hash,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--channel", choices=("stable", "continuous"), required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--adb-source-root", required=True, type=pathlib.Path)
    parser.add_argument("--ios-source-root", required=True, type=pathlib.Path)
    parser.add_argument("--qualification-receipt", type=pathlib.Path, action="append", default=[])
    parser.add_argument(
        "--ios-qualification-receipt", type=pathlib.Path, action="append", default=[]
    )
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    args.version = validate_version(args.version)
    args.base_url = normalize_base_url(args.base_url)
    expected_tag = "continuous" if args.channel == "continuous" else f"v{args.version}"
    if args.tag != expected_tag:
        raise SystemExit("publication channel/tag/version mismatch")
    shutil.rmtree(args.output, ignore_errors=True)
    args.output.mkdir(parents=True)

    components = {}
    source_hashes = {}
    components["adb-helper"], source_hashes["adb-helper"] = publish_component(
        "adb-helper",
        args.adb_source_root,
        "adb-helper-source-set-receipt.json",
        args.version,
        args.tag,
        args.base_url,
        args.output,
    )
    components["ios-native"], source_hashes["ios-native"] = publish_component(
        "ios-native",
        args.ios_source_root,
        "ios-native-source-set-receipt.json",
        args.version,
        args.tag,
        args.base_url,
        args.output,
    )

    ios_qualifications = {}
    for receipt_path in args.ios_qualification_receipt:
        receipt = read_json(receipt_path, "iOS package qualification receipt")
        if (
            receipt.get("receipt_kind") != "ios-native-package-verification"
            or receipt.get("release_qualified") is not True
            or receipt.get("source_set_receipt_sha256") != source_hashes["ios-native"]
            or re.fullmatch(
                r"[0-9a-f]{64}",
                str(receipt.get("ios_native_package_receipt_sha256", "")),
            )
            is None
        ):
            raise PublicationError(
                f"iOS package qualification source-set binding mismatch: {receipt_path}"
            )
        records = receipt.get("packages")
        if not isinstance(records, list) or not records:
            raise PublicationError(
                f"iOS qualification receipt has no packages: {receipt_path}"
            )
        for record in records:
            if not isinstance(record, dict):
                raise PublicationError(
                    f"invalid iOS qualified package record: {receipt_path}"
                )
            name = validate_asset_file_name(record.get("name"))
            if name in ios_qualifications:
                raise PublicationError(
                    f"duplicate iOS qualification for package: {name}"
                )
            ios_qualifications[name] = (receipt_path, receipt, record)

    packages = []
    for receipt_path in args.qualification_receipt:
        receipt = read_json(receipt_path, "package qualification receipt")
        if receipt.get("receipt_kind") != "package-verification" or receipt.get(
            "release_qualified"
        ) is not True:
            raise PublicationError(f"package is not release-qualified: {receipt_path}")
        if receipt.get("source_set_receipt_sha256") != source_hashes["adb-helper"]:
            raise PublicationError(
                f"ADB package qualification source-set binding mismatch: {receipt_path}"
            )
        records = receipt.get("packages")
        if not isinstance(records, list) or not records:
            raise PublicationError(f"qualification receipt has no packages: {receipt_path}")
        for record in records:
            if not isinstance(record, dict):
                raise PublicationError(f"invalid qualified package record: {receipt_path}")
            name = validate_asset_file_name(record.get("name"))
            source = receipt_path.parent / name
            package = copy_regular(source, args.output / name, "qualified package")
            if sha256(package) != record.get("sha256"):
                raise PublicationError(f"qualified package hash mismatch: {package.name}")
            qualification_name = f"{package.name}.qualification.json"
            qualification = copy_regular(
                receipt_path,
                args.output / qualification_name,
                f"qualification receipt for {package.name}",
            )
            applicable = {"adb-helper": source_hashes["adb-helper"]}
            ios_qualification_record = None
            if package.suffix.lower() == ".dmg":
                applicable["ios-native"] = source_hashes["ios-native"]
                ios_evidence = ios_qualifications.pop(package.name, None)
                if ios_evidence is None:
                    raise PublicationError(
                        f"missing external iOS qualification for DMG: {package.name}"
                    )
                ios_path, _, ios_package = ios_evidence
                if ios_package.get("sha256") != sha256(package):
                    raise PublicationError(
                        f"iOS qualification package hash mismatch: {package.name}"
                    )
                ios_name = f"{package.name}.ios-qualification.json"
                ios_copy = copy_regular(
                    ios_path,
                    args.output / ios_name,
                    f"iOS qualification receipt for {package.name}",
                )
                ios_qualification_record = {
                    "file_name": ios_copy.name,
                    "sha256": sha256(ios_copy),
                }
            package_record = {
                "file_name": package.name,
                "sha256": sha256(package),
                "qualification_receipt": {
                    "file_name": qualification.name,
                    "sha256": sha256(qualification),
                },
                "source_sets": applicable,
            }
            if ios_qualification_record is not None:
                package_record["ios_qualification_receipt"] = ios_qualification_record
            packages.append(package_record)

    if ios_qualifications:
        raise PublicationError(
            "iOS qualification receipts do not match published DMGs: "
            + ", ".join(sorted(ios_qualifications))
        )

    manifest = {
        "schema_version": 1,
        "manifest_kind": "klogg-source-publication",
        "channel": args.channel,
        "release": {
            "tag": args.tag,
            "version": args.version,
            "commit": args.commit,
            "mutable": args.channel == "continuous",
        },
        "components": components,
        "packages": packages,
    }
    manifest_path = args.output / "klogg-source-publication-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    verify_manifest(
        manifest_path,
        args.output,
        args.channel,
        args.tag,
        args.version,
        args.commit,
        args.base_url,
    )
    print(f"prepared coherent {args.channel} publication in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
