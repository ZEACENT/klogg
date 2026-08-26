#!/usr/bin/env python3
"""Generate source, license, notice, SPDX SBOM, and receipt assets for the iOS stack."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import pathlib
import shutil
import tarfile


class LegalAssetError(RuntimeError):
    pass


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def add_bytes(archive: tarfile.TarFile, name: str, value: bytes) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(value)
    info.mode = 0o644
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    info.mtime = 0
    archive.addfile(info, io.BytesIO(value))


def add_file(archive: tarfile.TarFile, source: pathlib.Path, name: str) -> None:
    add_bytes(archive, name, source.read_bytes())


def write_deterministic_source_archive(
    output: pathlib.Path,
    lock_path: pathlib.Path,
    lock: dict,
    archive_root: pathlib.Path,
    repository_root: pathlib.Path,
) -> None:
    with output.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                add_file(archive, lock_path, "libimobiledevice.lock.json")
                for source in lock["sources"]:
                    path = archive_root / source["archive_file"]
                    if not path.is_file() or sha256(path) != source["archive_sha256"]:
                        raise LegalAssetError(f"missing or mismatched source archive: {path}")
                    add_file(archive, path, f"archives/{path.name}")
                for patch in lock["patches"]:
                    path = repository_root / "3rdparty/libimobiledevice" / patch["path"]
                    if not path.is_file() or sha256(path) != patch["sha256"]:
                        raise LegalAssetError(f"missing or mismatched patch: {path}")
                    add_file(archive, path, patch["path"])
                for relative in (
                    "packaging/ios-native/superbuild/CMakeLists.txt",
                    "scripts/prefetch_ios_native_sources.py",
                    "scripts/build_ios_native_stack.py",
                    "scripts/verify_ios_native_stack.py",
                    "scripts/build_ios_native_legal_assets.py",
                ):
                    add_file(archive, repository_root / relative, relative)


def extract_legal_file(source_archive: pathlib.Path, wanted: str) -> bytes:
    with tarfile.open(source_archive, "r:*") as archive:
        matches = [
            member
            for member in archive.getmembers()
            if member.isfile()
            and len(pathlib.PurePosixPath(member.name).parts) == 2
            and pathlib.PurePosixPath(member.name).name == wanted
        ]
        if len(matches) != 1:
            raise LegalAssetError(f"expected one {wanted} in {source_archive}, found {len(matches)}")
        stream = archive.extractfile(matches[0])
        if stream is None:
            raise LegalAssetError(f"cannot read {wanted} from {source_archive}")
        return stream.read()


def write_json(path: pathlib.Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def validated_build_receipt(
    lock_path: pathlib.Path, lock: dict, stack_root: pathlib.Path
) -> tuple[dict, list[dict]]:
    receipt_path = stack_root / lock["receipts"]["build"]
    if not receipt_path.is_file():
        raise LegalAssetError(f"missing iOS native build receipt: {receipt_path}")
    try:
        receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise LegalAssetError(f"invalid iOS native build receipt: {error}") from error
    if not isinstance(receipt, dict) or receipt.get("receipt_kind") != "ios-native-build":
        raise LegalAssetError("invalid iOS native build receipt kind")
    if receipt.get("lock_sha256") != sha256(lock_path):
        raise LegalAssetError("iOS native build receipt is not bound to the current lock")
    architecture = receipt.get("architecture")
    thin = lock.get("artifact_contract", {}).get("thin_artifacts", {}).get(architecture)
    if not isinstance(thin, dict):
        raise LegalAssetError("iOS native build receipt architecture is not locked")
    expected_target = str(thin.get("deployment_target"))
    if str(receipt.get("deployment_target")) != expected_target:
        raise LegalAssetError("iOS native build receipt deployment target mismatch")
    if receipt.get("native_qualified") is not True or receipt.get("qualification") != "native":
        raise LegalAssetError("inspection-only iOS native builds cannot produce release legal assets")

    dylibs = receipt.get("dylibs")
    if not isinstance(dylibs, list) or not dylibs:
        raise LegalAssetError("iOS native build receipt has no dylib closure")
    names: set[str] = set()
    validated = []
    for item in dylibs:
        if not isinstance(item, dict):
            raise LegalAssetError("invalid dylib record in iOS native build receipt")
        name = item.get("name")
        expected_sha256 = item.get("sha256")
        if (
            not isinstance(name, str)
            or pathlib.PurePath(name).name != name
            or name in names
            or not isinstance(expected_sha256, str)
        ):
            raise LegalAssetError("invalid or duplicate dylib identity in build receipt")
        path = stack_root / "lib" / name
        if not path.is_file() or path.is_symlink() or sha256(path) != expected_sha256:
            raise LegalAssetError(f"build receipt does not bind shipped dylib: {name}")
        names.add(name)
        validated.append({"name": name, "sha256": expected_sha256})
    return receipt, validated


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--archive-root", required=True, type=pathlib.Path)
    parser.add_argument("--repository-root", required=True, type=pathlib.Path)
    parser.add_argument("--stack-root", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    args.output.mkdir(parents=True, exist_ok=True)
    sources = lock["sources"]
    expected_ids = {
        "openssl",
        "curl",
        "libplist",
        "libimobiledevice-glue",
        "libusbmuxd",
        "libtatsu",
        "libimobiledevice",
    }
    if {source["id"] for source in sources} != expected_ids:
        raise LegalAssetError("legal source closure does not match the locked native stack")

    build_receipt, dylibs = validated_build_receipt(args.lock, lock, args.stack_root)

    source_offer = args.output / "ios-native-corresponding-source.tar.gz"
    write_deterministic_source_archive(
        source_offer, args.lock, lock, args.archive_root, args.repository_root
    )
    (args.output / "ios-native-source-offer.txt").write_text(
        "Klogg iOS Native Stack Corresponding Source Offer\n"
        "================================================\n\n"
        "The accompanying ios-native-corresponding-source.tar.gz package contains every locked\n"
        "source archive, the mandatory upstream 5ca453f9 leak patch, and the disconnected build,\n"
        "verification, legal, receipt, and package scripts needed to reproduce the shipped dylibs.\n"
        "No Homebrew runtime, usbmuxd daemon, MobileDevice private framework, or command-line tool\n"
        "is included in the application package. The corresponding source remains available with\n"
        "the release package for at least three years. See ios-native-lgpl-replacement.txt for\n"
        "rebuild, replacement, and ad-hoc re-signing instructions.\n",
        encoding="utf-8",
    )
    replacement_guide = args.output / "ios-native-lgpl-replacement.txt"
    replacement_guide.write_text(
        "Klogg iOS Native LGPL Replacement Guide\n"
        "========================================\n\n"
        "The libplist, libtatsu, libimobiledevice-glue, libusbmuxd, and libimobiledevice\n"
        "dylibs are\n"
        "dynamically loaded from Contents/Frameworks/ios-native/lib. You may rebuild or modify\n"
        "them from ios-native-corresponding-source.tar.gz and replace the matching dylib files.\n\n"
        "1. Extract ios-native-corresponding-source.tar.gz and the seven locked source archives.\n"
        "2. Install CMake, Ninja, autoconf, automake, libtool, pkg-config, Perl, and Clang.\n"
        "3. Run scripts/build_ios_native_stack.py with the included lock, archives, repository\n"
        "   root, a fresh work root, an artifact root, and the architecture of your Klogg build.\n"
        "4. Replace the dylibs in Klogg.app/Contents/Frameworks/ios-native/lib, preserving names.\n"
        "5. Re-sign the modified local application ad hoc:\n"
        "     codesign --force --deep --sign - /path/to/Klogg.app\n"
        "6. Verify it before launch:\n"
        "     codesign --verify --deep --strict --verbose=2 /path/to/Klogg.app\n\n"
        "Replacing code invalidates the upstream notarized signature; the ad-hoc signature is for\n"
        "your locally modified copy. Klogg imposes no restriction on debugging or reverse\n"
        "engineering these LGPL libraries for the purpose of installing modified versions.\n",
        encoding="utf-8",
    )

    licenses = args.output / "licenses"
    if licenses.exists():
        shutil.rmtree(licenses)
    licenses.mkdir()
    legal_files = []
    notice_lines = ["Klogg iOS native dependency notices", ""]
    for source in sources:
        archive_path = args.archive_root / source["archive_file"]
        for license_name in source["legal"]["license_files"]:
            destination = licenses / f"{source['id']}-{license_name}"
            destination.write_bytes(extract_legal_file(archive_path, license_name))
            legal_files.append(
                {"source": source["id"], "path": destination.relative_to(args.output).as_posix(),
                 "sha256": sha256(destination), "license": source["legal"]["spdx_license"]}
            )
        notice_lines.append(f"{source['id']} {source['version']}: {source['legal']['notice_label']}")
    notice = args.output / "NOTICE-ios-native.txt"
    notice.write_text("\n".join(notice_lines) + "\n", encoding="utf-8")

    package_ids = [f"SPDXRef-Package-{source['id'].replace('_', '-')}" for source in sources]
    file_ids = [f"SPDXRef-File-{index}" for index in range(1, len(dylibs) + 1)]
    sbom = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": "klogg-ios-native-stack",
        "documentNamespace": f"https://github.com/ZEACENT/klogg/ios-native/{sha256(args.lock)}",
        "creationInfo": {
            "created": "2026-06-09T00:00:00Z",
            "creators": ["Tool: klogg-build-ios-native-legal-assets"],
        },
        "documentDescribes": package_ids + file_ids,
        "packages": [
            {
                "name": source["id"],
                "SPDXID": package_id,
                "versionInfo": source["version"],
                "downloadLocation": source["archive_url"],
                "checksums": [{"algorithm": "SHA256", "checksumValue": source["archive_sha256"]}],
                "licenseDeclared": source["legal"]["spdx_license"],
                "licenseConcluded": source["legal"]["spdx_license"],
                "copyrightText": "NOASSERTION",
                "filesAnalyzed": False,
            }
            for source, package_id in zip(sources, package_ids)
        ],
        "files": [
            {
                "fileName": f"lib/{item['name']}",
                "SPDXID": f"SPDXRef-File-{index}",
                "checksums": [{"algorithm": "SHA256", "checksumValue": item["sha256"]}],
                "licenseConcluded": "NOASSERTION",
                "licenseInfoInFiles": ["NOASSERTION"],
                "copyrightText": "NOASSERTION",
            }
            for index, item in enumerate(dylibs, start=1)
        ],
        "relationships": [
            {
                "spdxElementId": file_id,
                "relationshipType": "DEPENDS_ON",
                "relatedSpdxElement": package_id,
            }
            for file_id in file_ids
            for package_id in package_ids
        ],
    }
    sbom_path = args.output / lock["receipts"]["sbom"]
    write_json(sbom_path, sbom)

    source_receipt = {
        "schema_version": 1,
        "receipt_kind": "source",
        "lock_sha256": sha256(args.lock),
        "architecture": build_receipt["architecture"],
        "deployment_target": build_receipt["deployment_target"],
        "source_offer": {"path": source_offer.name, "sha256": sha256(source_offer)},
        "archives": [
            {"id": source["id"], "file": source["archive_file"], "sha256": source["archive_sha256"]}
            for source in sources
        ],
        "patches": lock["patches"],
    }
    write_json(args.output / lock["receipts"]["source"], source_receipt)

    legal_receipt = {
        "schema_version": 1,
        "receipt_kind": "legal",
        "lock_sha256": sha256(args.lock),
        "architecture": build_receipt["architecture"],
        "deployment_target": build_receipt["deployment_target"],
        "license_files": legal_files,
        "notice": {"path": notice.name, "sha256": sha256(notice)},
        "replacement_guide": {
            "path": replacement_guide.name,
            "sha256": sha256(replacement_guide),
        },
        "sbom": {"path": sbom_path.name, "sha256": sha256(sbom_path)},
        "package": "app-bundled dynamic dylib closure",
    }
    write_json(args.output / lock["receipts"]["legal"], legal_receipt)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
