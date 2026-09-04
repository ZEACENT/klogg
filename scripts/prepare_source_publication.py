#!/usr/bin/env python3
"""Assemble one coherent package and external component-source publication set."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import pathlib
import re
import shutil
import tarfile

from source_publication_identity import (
    normalize_base_url,
    published_source_name,
    source_asset_url,
    validate_asset_file_name,
    validate_version,
)
from verify_source_publication_manifest import (
    COMPONENT_DISPLAY_NAMES,
    EVIDENCE_LEVELS,
    PublicationError,
    SIGNED_MACOS_PACKAGE_RECEIPTS,
    SUPPORT_DISPLAY_NAMES,
    supported_package_display,
    verify_manifest,
    verify_package_receipt_evidence,
)

EVIDENCE_ARCHIVE_NAME = "klogg-release-evidence.tar"
CHECKSUMS_NAME = "SHA256SUMS"
MANIFEST_NAME = "klogg-source-publication-manifest.json"


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def json_bytes(document: object) -> bytes:
    return (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode(
        "utf-8"
    )


def read_json(path: pathlib.Path, label: str) -> dict:
    if not path.is_file() or path.is_symlink():
        raise PublicationError(f"missing or invalid {label}: {path}")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise PublicationError(f"invalid {label}: {error}") from error
    if not isinstance(value, dict):
        raise PublicationError(f"invalid {label}: root must be an object")
    return value


def copy_regular(source: pathlib.Path, destination: pathlib.Path, label: str) -> pathlib.Path:
    if not source.is_file() or source.is_symlink():
        raise PublicationError(f"missing or invalid {label}: {source}")
    if destination.exists():
        if destination.is_symlink() or not destination.is_file():
            raise PublicationError(f"publication file-name collision: {destination.name}")
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
) -> tuple[dict, str, list[dict]]:
    receipt_source = root / receipt_name
    receipt = read_json(receipt_source, f"{component} source-set receipt")
    if receipt.get("receipt_kind") != "component-source-set" or receipt.get(
        "component"
    ) != component:
        raise PublicationError(f"invalid component source-set receipt: {component}")
    archive_record = receipt.get("archive")
    if not isinstance(archive_record, dict):
        raise PublicationError(f"source-set receipt lacks archive: {component}")
    archive_name = validate_asset_file_name(archive_record.get("file_name"))
    archive_source = root / archive_name
    archive_hash = sha256(archive_source)
    if archive_record.get("sha256") != archive_hash:
        raise PublicationError(f"source-set archive hash mismatch: {component}")
    published_archive_name = published_source_name(version, component, archive_hash)
    published_archive = copy_regular(
        archive_source, output / published_archive_name, f"{component} source archive"
    )
    published_receipt = copy_regular(
        receipt_source, output / receipt_name, f"{component} source-set receipt"
    )

    support_records: list[dict] = []
    support = receipt.get("package_support_assets")
    if not isinstance(support, list):
        raise PublicationError(f"invalid package support asset list: {component}")
    for item in support:
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
            raise PublicationError(
                f"package support asset hash mismatch: {component}:{relative}"
            )
        published_support = copy_regular(
            source, output / relative.name, f"{component} package support asset"
        )
        display_name = SUPPORT_DISPLAY_NAMES.get(published_support.name)
        if display_name is None:
            raise PublicationError(
                f"unsupported top-level package support asset: {published_support.name}"
            )
        support_records.append(
            {
                "display_name": display_name,
                "file_name": published_support.name,
                "sha256": sha256(published_support),
            }
        )

    receipt_hash = sha256(published_receipt)
    source_display, receipt_display = COMPONENT_DISPLAY_NAMES[component]
    return (
        {
            "display_name": source_display,
            "source_set_receipt": {
                "display_name": receipt_display,
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
        support_records,
    )


class EvidenceBuilder:
    """Collect original receipt bytes into one content-addressed deterministic tar."""

    def __init__(self) -> None:
        self._contents: dict[str, bytes] = {}
        self._references: list[dict] = []
        self._logical_associations: set[tuple[str, str]] = set()

    def add(
        self, package_name: str, kind: str, source: pathlib.Path, label: str
    ) -> dict:
        if not source.is_file() or source.is_symlink():
            raise PublicationError(f"missing or invalid {label}: {source}")
        content = source.read_bytes()
        digest = sha256_bytes(content)
        member = f"receipts/{digest}.json"
        existing = self._contents.get(member)
        if existing is not None and existing != content:
            raise PublicationError("evidence SHA-256 collision")
        self._contents[member] = content
        association = (package_name, kind)
        if association in self._logical_associations:
            raise PublicationError(
                f"duplicate package evidence association: {package_name}:{kind}"
            )
        self._logical_associations.add(association)
        self._references.append(
            {
                "package": package_name,
                "kind": kind,
                "source_file_name": source.name,
                "member": member,
                "sha256": digest,
            }
        )
        return {"member": member, "sha256": digest}

    def write(self, destination: pathlib.Path) -> tuple[str, str]:
        references = sorted(
            self._references,
            key=lambda item: (
                item["package"],
                item["kind"],
                item["source_file_name"],
            ),
        )
        index = {
            "schema_version": 1,
            "index_kind": "klogg-release-evidence",
            "receipts": [
                {
                    "member": member,
                    "sha256": sha256_bytes(content),
                    "size": len(content),
                }
                for member, content in sorted(self._contents.items())
            ],
            "references": references,
        }
        index_content = json_bytes(index)
        members = [("index.json", index_content), *sorted(self._contents.items())]
        with tarfile.open(destination, "w", format=tarfile.USTAR_FORMAT) as archive:
            for name, content in members:
                info = tarfile.TarInfo(name)
                info.size = len(content)
                info.type = tarfile.REGTYPE
                info.mode = 0o644
                info.uid = 0
                info.gid = 0
                info.mtime = 0
                info.uname = ""
                info.gname = ""
                archive.addfile(info, io.BytesIO(content))
        return sha256(destination), sha256_bytes(index_content)


def write_checksums(output: pathlib.Path) -> None:
    assets = sorted(
        path
        for path in output.iterdir()
        if path.is_file() and not path.is_symlink() and path.name != CHECKSUMS_NAME
    )
    (output / CHECKSUMS_NAME).write_text(
        "".join(f"{sha256(path)} *{path.name}\n" for path in assets),
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--channel", choices=("stable", "continuous"), required=True)
    parser.add_argument(
        "--evidence-level", choices=tuple(sorted(EVIDENCE_LEVELS)), required=True
    )
    parser.add_argument("--tag", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--adb-source-root", required=True, type=pathlib.Path)
    parser.add_argument("--ios-source-root", required=True, type=pathlib.Path)
    parser.add_argument(
        "--qualification-receipt", type=pathlib.Path, action="append", default=[]
    )
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

    components: dict[str, dict] = {}
    source_hashes: dict[str, str] = {}
    support_by_name: dict[str, dict] = {}
    for component, root, receipt_name in (
        (
            "adb-helper",
            args.adb_source_root,
            "adb-helper-source-set-receipt.json",
        ),
        (
            "ios-native",
            args.ios_source_root,
            "ios-native-source-set-receipt.json",
        ),
    ):
        component_record, source_hash, support_records = publish_component(
            component,
            root,
            receipt_name,
            args.version,
            args.tag,
            args.base_url,
            args.output,
        )
        components[component] = component_record
        source_hashes[component] = source_hash
        for support_record in support_records:
            previous = support_by_name.get(support_record["file_name"])
            if previous is not None and previous != support_record:
                raise PublicationError(
                    f"conflicting support asset record: {support_record['file_name']}"
                )
            support_by_name[support_record["file_name"]] = support_record

    ios_qualifications: dict[str, tuple[pathlib.Path, dict, dict]] = {}
    for receipt_path in args.ios_qualification_receipt:
        receipt = read_json(receipt_path, "iOS package qualification receipt")
        if (
            receipt.get("receipt_kind") != "ios-native-package-verification"
            or receipt.get("qualification") != args.evidence_level
            or not isinstance(receipt.get("release_qualified"), bool)
            or (
                args.evidence_level == "signed"
                and receipt.get("release_qualified") is not True
            )
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
                raise PublicationError(f"duplicate iOS qualification for package: {name}")
            ios_qualifications[name] = (receipt_path, receipt, record)

    evidence_builder = EvidenceBuilder()
    packages_by_name: dict[str, dict] = {}
    package_displays = supported_package_display(args.version)
    for receipt_path in args.qualification_receipt:
        receipt = read_json(receipt_path, "package qualification receipt")
        if receipt.get("receipt_kind") != "package-verification":
            raise PublicationError(f"invalid package qualification receipt: {receipt_path}")
        verify_package_receipt_evidence(receipt, receipt_path.name, args.evidence_level)
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
            if name in packages_by_name:
                raise PublicationError(f"duplicate qualified package: {name}")
            display = package_displays.get(name)
            if display is None:
                raise PublicationError(f"unsupported qualified package mapping: {name}")
            source = receipt_path.parent / name
            package = copy_regular(source, args.output / name, "qualified package")
            package_hash = sha256(package)
            if package_hash != record.get("sha256"):
                raise PublicationError(f"qualified package hash mismatch: {package.name}")
            package_evidence = {
                "qualification": evidence_builder.add(
                    package.name,
                    "qualification",
                    receipt_path,
                    f"qualification receipt for {package.name}",
                )
            }
            applicable = {"adb-helper": source_hashes["adb-helper"]}
            if package.suffix.lower() == ".dmg":
                if args.evidence_level == "signed":
                    verify_package_receipt_evidence(
                        receipt,
                        package.name,
                        args.evidence_level,
                        SIGNED_MACOS_PACKAGE_RECEIPTS,
                    )
                applicable["ios-native"] = source_hashes["ios-native"]
                ios_evidence = ios_qualifications.pop(package.name, None)
                if ios_evidence is None:
                    raise PublicationError(
                        f"missing external iOS qualification for DMG: {package.name}"
                    )
                ios_path, ios_receipt, ios_package = ios_evidence
                if ios_package.get("sha256") != package_hash:
                    raise PublicationError(
                        f"iOS qualification package hash mismatch: {package.name}"
                    )
                ios_package_source = ios_path.parent / "ios-native-package-receipt.json"
                ios_package_receipt = read_json(
                    ios_package_source, f"iOS package receipt for {package.name}"
                )
                if (
                    ios_package_receipt.get("receipt_kind") != "ios-native-package"
                    or ios_package_receipt.get("status") != "passed"
                    or ios_package_receipt.get("source_set_receipt_sha256")
                    != source_hashes["ios-native"]
                    or sha256(ios_package_source)
                    != ios_receipt.get("ios_native_package_receipt_sha256")
                ):
                    raise PublicationError(
                        f"iOS package receipt evidence mismatch: {package.name}"
                    )
                package_evidence["ios-package"] = evidence_builder.add(
                    package.name,
                    "ios-package",
                    ios_package_source,
                    f"iOS package receipt for {package.name}",
                )
                package_evidence["ios-qualification"] = evidence_builder.add(
                    package.name,
                    "ios-qualification",
                    ios_path,
                    f"iOS qualification receipt for {package.name}",
                )
                if args.evidence_level == "signed":
                    for kind in ("signing", "notarization"):
                        source_receipt = (
                            receipt_path.parent / f"adb-helper-{kind}-receipt.json"
                        )
                        package_evidence[kind] = evidence_builder.add(
                            package.name,
                            kind,
                            source_receipt,
                            f"{kind} receipt for {package.name}",
                        )
            packages_by_name[name] = {
                "file_name": package.name,
                "sha256": package_hash,
                "display": {"section": display[0], "label": display[1]},
                "source_sets": applicable,
                "evidence": package_evidence,
            }

    if ios_qualifications:
        raise PublicationError(
            "iOS qualification receipts do not match published DMGs: "
            + ", ".join(sorted(ios_qualifications))
        )
    if set(packages_by_name) != set(package_displays):
        missing = sorted(set(package_displays) - set(packages_by_name))
        extra = sorted(set(packages_by_name) - set(package_displays))
        raise PublicationError(
            f"qualified package coverage mismatch; missing={missing}, extra={extra}"
        )
    if set(support_by_name) != set(SUPPORT_DISPLAY_NAMES):
        missing = sorted(set(SUPPORT_DISPLAY_NAMES) - set(support_by_name))
        extra = sorted(set(support_by_name) - set(SUPPORT_DISPLAY_NAMES))
        raise PublicationError(
            f"support asset coverage mismatch; missing={missing}, extra={extra}"
        )

    evidence_path = args.output / EVIDENCE_ARCHIVE_NAME
    evidence_hash, index_hash = evidence_builder.write(evidence_path)
    public_name = (
        f"Continuous Build {args.version}"
        if args.channel == "continuous"
        else f"Release v{args.version}"
    )
    covered_asset_count = len(
        [path for path in args.output.iterdir() if path.is_file()]
    ) + 1  # The manifest is written next and is covered too.
    manifest = {
        "schema_version": 2,
        "manifest_kind": "klogg-release-publication",
        "channel": args.channel,
        "evidence_level": args.evidence_level,
        "release": {
            "tag": args.tag,
            "version": args.version,
            "commit": args.commit,
            "mutable": args.channel == "continuous",
            "public_name": public_name,
            "page_url": f"{args.base_url}/releases/tag/{args.tag}",
            "direct_asset_urls_are_archival": args.channel == "stable",
        },
        "components": components,
        "packages": [packages_by_name[name] for name in package_displays],
        "support_assets": [support_by_name[name] for name in SUPPORT_DISPLAY_NAMES],
        "evidence_archive": {
            "file_name": evidence_path.name,
            "sha256": evidence_hash,
            "index_member": "index.json",
            "index_sha256": index_hash,
        },
        "checksums": {
            "file_name": CHECKSUMS_NAME,
            "covered_asset_count": covered_asset_count,
            "format": "sha256sum-binary-v1",
        },
    }
    manifest_path = args.output / MANIFEST_NAME
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    write_checksums(args.output)
    verify_manifest(
        manifest_path,
        args.output,
        args.channel,
        args.evidence_level,
        args.tag,
        args.version,
        args.commit,
        args.base_url,
    )
    print(
        f"prepared coherent {args.channel}/{args.evidence_level} publication in {args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
