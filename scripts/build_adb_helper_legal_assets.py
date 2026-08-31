#!/usr/bin/env python3
"""Create the locked ADB source, legal, notice, SBOM, and source-offer assets."""

from __future__ import annotations

import argparse
import contextlib
import datetime
import gzip
import hashlib
import io
import json
import pathlib
import re
import tarfile

from source_publication_identity import (
    normalize_base_url,
    published_source_name,
    validate_version,
)


RAW_ARCHIVE_IDENTITY = "raw-sha256"
CANONICAL_TAR_GZ_IDENTITY = "canonical-tar-gz-v1"
SUPPORTED_ARCHIVE_IDENTITIES = {RAW_ARCHIVE_IDENTITY, CANONICAL_TAR_GZ_IDENTITY}


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def add_bytes(tar: tarfile.TarFile, name: str, content: bytes) -> None:
    name = safe_archive_member_name(name, "generated archive member path")
    info = tarfile.TarInfo(name)
    info.size = len(content)
    info.mode = 0o644
    info.mtime = 0
    info.uid = info.gid = 0
    info.uname = info.gname = "root"
    tar.addfile(info, io.BytesIO(content))


def add_file(tar: tarfile.TarFile, path: pathlib.Path, name: str) -> None:
    add_bytes(tar, name, path.read_bytes())


@contextlib.contextmanager
def deterministic_tar_gz(path: pathlib.Path):
    with path.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as tar:
                yield tar


def safe_archive_member_name(value: str, label: str) -> str:
    path = pathlib.PurePosixPath(value)
    if (
        not value
        or "\\" in value
        or path.is_absolute()
        or ".." in path.parts
        or not path.parts
    ):
        raise RuntimeError(f"unsafe {label}: {value}")
    return path.as_posix()


def add_upstream_legal_material(
    output: tarfile.TarFile,
    archive: pathlib.Path,
    source_id: str,
    include_notices: bool,
) -> int:
    count = 0
    source_id = safe_archive_member_name(source_id, "ADB source id")
    if "/" in source_id:
        raise RuntimeError(f"unsafe ADB source id: {source_id}")
    legal_name = re.compile(r"^(?:licen[sc]es?|copying|module_license|notice)(?:[._-].*)?$", re.I)
    with tarfile.open(archive, "r:*") as source:
        for member in source.getmembers():
            if not member.isfile() or member.size > 2 * 1024 * 1024:
                continue
            member_name = safe_archive_member_name(member.name, "upstream legal member path")
            base = pathlib.PurePosixPath(member_name).name
            if not legal_name.match(base):
                continue
            is_notice = base.lower().startswith("notice")
            if is_notice != include_notices:
                continue
            stream = source.extractfile(member)
            if stream is None:
                continue
            add_bytes(output, f"upstream/{source_id}/{member_name}", stream.read())
            count += 1
    return count


def write_hash(path: pathlib.Path) -> None:
    path.with_name(path.name + ".sha256").write_text(
        f"{sha256(path)}  {path.name}\n", encoding="utf-8"
    )


def canonical_sha256(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def spdx_package(item: dict) -> dict:
    archive_identity = str(item.get("archive_identity", RAW_ARCHIVE_IDENTITY))
    if archive_identity not in SUPPORTED_ARCHIVE_IDENTITIES:
        raise RuntimeError(
            f"unsupported ADB archive identity for {item.get('id')}: {archive_identity}"
        )
    package = {
        "name": item["id"],
        "SPDXID": "SPDXRef-Package-" + item["id"].replace("_", "-"),
        "versionInfo": item.get("version", item.get("commit")),
        "downloadLocation": item["archive_url"],
        "checksums": [{"algorithm": "SHA256", "checksumValue": item["archive_sha256"]}],
        "licenseConcluded": item.get(
            "license", " AND ".join(item.get("legal", {}).get("licenses", [])) or "NOASSERTION"
        ),
        "licenseDeclared": item.get(
            "license", " AND ".join(item.get("legal", {}).get("licenses", [])) or "NOASSERTION"
        ),
        "filesAnalyzed": False,
    }
    if archive_identity == CANONICAL_TAR_GZ_IDENTITY:
        package["downloadLocation"] = "NOASSERTION"
        package["sourceInfo"] = (
            f"{item['archive_file']} is included under archives/ in the klogg ADB helper "
            f"corresponding-source asset. It was canonicalized from {item['archive_url']} "
            f"using {archive_identity}; the SHA-256 identifies those canonical bytes, not "
            "the provider's transport-time tar.gz response."
        )
    return package


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--archive-root", required=True, type=pathlib.Path)
    parser.add_argument("--repository-root", required=True, type=pathlib.Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    version = validate_version(args.version)
    base_url = normalize_base_url(args.base_url)
    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    args.output.mkdir(parents=True, exist_ok=True)
    by_kind = {asset["kind"]: asset for asset in lock["release_assets"]}

    source_manifest = {
        "schema_version": 1,
        "baseline": lock["helper"],
        "sources": lock["sources"],
        "dependencies": lock.get("dependencies", []),
        "patches": lock["patches"],
        "toolchain_packages": lock.get("toolchain_packages", []),
        "toolchains": lock["toolchains"],
        "targets": lock["targets"],
    }
    manifest_path = args.output / by_kind["source-manifest"]["file_name"]
    manifest_path.write_text(
        json.dumps(source_manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    source_archive = args.output / by_kind["source-archive"]["file_name"]
    with deterministic_tar_gz(source_archive) as tar:
        add_file(tar, args.lock, "adb-helper.lock.json")
        add_file(tar, manifest_path, manifest_path.name)
        build_material = (
            "packaging/adb/README.md",
            "packaging/adb/superbuild/CMakeLists.txt",
            "scripts/prefetch_adb_helper_sources.py",
            "scripts/build_adb_helper.py",
            "scripts/build_adb_helper_legal_assets.py",
            "scripts/source_publication_identity.py",
            "scripts/extract_verified_tar.py",
            "scripts/verify_adb_helper_toolchain.py",
            "scripts/verify_adb_helper_artifact.py",
            "scripts/verify_adb_helper_envelope.py",
            "scripts/smoke_adb_helper.py",
        )
        for relative in build_material:
            add_file(tar, args.repository_root / relative, relative)
        for record in [*lock["sources"], *lock.get("dependencies", [])]:
            archive = args.archive_root / record["archive_file"]
            if not archive.is_file() or sha256(archive) != record["archive_sha256"]:
                raise RuntimeError(f"missing or mismatched locked source archive: {archive}")
            add_file(tar, archive, f"archives/{archive.name}")
        for patch in lock["patches"]:
            patch_path = args.repository_root / patch["path"]
            if sha256(patch_path) != patch["sha256"]:
                raise RuntimeError(f"locked ADB patch changed: {patch_path}")
            add_file(tar, patch_path, patch["path"])

    licenses_text = [
        "Klogg ADB helper locked license inventory",
        "",
    ]
    for source in lock["sources"]:
        licenses_text.append(f"{source['id']}: {', '.join(source['legal']['licenses'])}")
    for dependency in lock.get("dependencies", []):
        licenses_text.append(f"{dependency['id']} {dependency['version']}: {dependency['license']}")
    locked_material = [*lock["sources"], *lock.get("dependencies", [])]
    licenses_archive = args.output / by_kind["licenses"]["file_name"]
    with deterministic_tar_gz(licenses_archive) as tar:
        add_bytes(tar, "LICENSE-INVENTORY.txt", ("\n".join(licenses_text) + "\n").encode())
        add_file(tar, args.repository_root / "COPYING", "COPYING")
        upstream_license_count = 0
        for record in locked_material:
            upstream_license_count += add_upstream_legal_material(
                tar,
                args.archive_root / record["archive_file"],
                record["id"],
                include_notices=False,
            )
        if upstream_license_count == 0:
            raise RuntimeError("locked source closure yielded no upstream license texts")

    notices_archive = args.output / by_kind["notices"]["file_name"]
    with deterministic_tar_gz(notices_archive) as tar:
        add_file(tar, args.repository_root / "NOTICE", "NOTICE")
        notice_summary = "\n".join(
            f"{source['id']}: " + "; ".join(source["legal"]["notices"])
            for source in lock["sources"]
        )
        add_bytes(tar, "ADB-HELPER-NOTICES.txt", (notice_summary + "\n").encode())
        for record in locked_material:
            add_upstream_legal_material(
                tar,
                args.archive_root / record["archive_file"],
                record["id"],
                include_notices=True,
            )

    package_ids = [
        "SPDXRef-Package-" + item["id"].replace("_", "-")
        for item in locked_material
    ]
    source_date_epoch = int(lock.get("release_policy", {}).get("source_date_epoch", 0))
    created = datetime.datetime.fromtimestamp(
        source_date_epoch, tz=datetime.timezone.utc
    ).strftime("%Y-%m-%dT%H:%M:%SZ")
    sbom = {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": "klogg-adb-helper-source-closure",
        "documentNamespace": "https://github.com/ZEACENT/klogg/adb-helper/sbom/37.0.0",
        "creationInfo": {
            "created": created,
            "creators": ["Tool: klogg-build-adb-helper-legal-assets"],
        },
        "documentDescribes": package_ids,
        "packages": [spdx_package(item) for item in locked_material],
        "relationships": [
            {
                "spdxElementId": "SPDXRef-DOCUMENT",
                "relationshipType": "DESCRIBES",
                "relatedSpdxElement": package_id,
            }
            for package_id in package_ids
        ],
    }
    sbom_path = args.output / by_kind["sbom"]["file_name"]
    sbom_path.write_text(json.dumps(sbom, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    source_offer = args.output / by_kind["source-offer"]["file_name"]
    source_archive_hash = sha256(source_archive)
    published_archive = published_source_name(version, "adb-helper", source_archive_hash)
    versioned_releases_page = f"{base_url}/releases"
    continuous_release_page = f"{base_url}/releases/tag/continuous"
    source_offer.write_text(
        "Klogg ADB Helper Corresponding Source Offer\n"
        "==========================================\n\n"
        "The complete, source-built ADB helper distributed with klogg is built from the immutable\n"
        "source closure recorded in adb-helper-source-manifest.json. The corresponding-source\n"
        "archive is not included in the installer; it is a separate GitHub Release asset. No\n"
        "Google Platform-Tools binary is redistributed.\n\n"
        f"Published archive: {published_archive}\n"
        f"SHA-256: {source_archive_hash}\n"
        f"Versioned releases page: {versioned_releases_page}\n"
        f"Rolling continuous release page: {continuous_release_page}\n\n"
        "Stable versioned releases retain their matching source asset for at least three years.\n"
        "The continuous endpoint is rolling and mutable: each successful continuous publication\n"
        "replaces its packages and source assets together and does not provide archival retention.\n"
        "Use the release page above to locate the content-addressed source asset included in the\n"
        "current rolling publication; a stable-only asset is not promised under the mutable tag.\n"
        "Verify downloaded bytes with:\n"
        f"  shasum -a 256 {published_archive}\n"
        "and compare the result with the SHA-256 above. Build instructions are included in the\n"
        "archive at packaging/adb/README.md.\n\n"
        "Linux packages place the LGPL-2.1-or-later libusb shared library beside adb and resolve it\n"
        "through the relative $ORIGIN runpath. You may replace that libusb file with a modified,\n"
        "ABI-compatible version. The source archive also contains the material needed to rebuild\n"
        "and relink adb against a modified libusb. No libusb library is shipped on macOS.\n",
        encoding="utf-8",
    )

    source_set_asset = by_kind["source-set-receipt"]
    package_support_assets = []
    for asset in lock["release_assets"]:
        distribution = asset.get("distribution", {})
        if (
            asset["kind"] == "source-set-receipt"
            or distribution.get("package_required") is not True
        ):
            continue
        path = args.output / asset["file_name"]
        if not path.is_file():
            raise RuntimeError(f"required ADB package support asset was not generated: {path}")
        package_support_assets.append(
            {"kind": asset["kind"], "file_name": path.name, "sha256": sha256(path)}
        )

    closure_identity = [
        {
            "id": record["id"],
            "archive_file": record["archive_file"],
            "archive_sha256": record["archive_sha256"],
            "archive_identity": record.get("archive_identity", RAW_ARCHIVE_IDENTITY),
            "revision": record.get("commit", record.get("version")),
        }
        for record in [*lock["sources"], *lock.get("dependencies", [])]
    ]
    patch_identity = [
        {
            "path": patch["path"],
            "sha256": patch["sha256"],
            "applies_to": patch.get("applies_to"),
            "target": patch.get("target"),
        }
        for patch in lock["patches"]
    ]
    source_set_receipt = {
        "schema_version": 1,
        "receipt_kind": "component-source-set",
        "component": "adb-helper",
        "lock_sha256": sha256(args.lock),
        "archive": {"file_name": source_archive.name, "sha256": sha256(source_archive)},
        "source_identity": {
            "manifest_or_closure_sha256": sha256(manifest_path),
            "tree_hash_algorithm": "sha256",
            "final_tree_sha256": canonical_sha256(closure_identity),
        },
        "patch_chain_sha256": canonical_sha256(patch_identity),
        "package_support_assets": package_support_assets,
        "distribution": {"package_required": False, "release_required": True},
    }
    source_set_path = args.output / source_set_asset["file_name"]
    source_set_path.write_text(
        json.dumps(source_set_receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    receipt_assets = []
    for asset in lock["release_assets"]:
        path = args.output / asset["file_name"]
        if not path.is_file():
            raise RuntimeError(f"required ADB release asset was not generated: {path}")
        write_hash(path)
        receipt_assets.append({"kind": asset["kind"], "path": path.name, "sha256": sha256(path)})
    (args.output / "adb-helper-release-assets.json").write_text(
        json.dumps(receipt_assets, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
