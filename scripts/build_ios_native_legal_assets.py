#!/usr/bin/env python3
"""Generate source, license, notice, SPDX SBOM, and receipt assets for the iOS stack."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import pathlib
import re
import shutil
import stat
import tarfile
import tempfile

from source_publication_identity import (
    normalize_base_url,
    published_source_name,
    validate_version,
)


class LegalAssetError(RuntimeError):
    pass


SOURCE_SET_RECEIPT = "ios-native-source-set-receipt.json"
OVERLAY_RECEIPT = "ios-native-overlay-receipt.json"


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validated_relative_path(value: object, label: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or "\\" in value
        or value.startswith("/")
        or any(part in ("", ".", "..") for part in value.split("/"))
    ):
        raise LegalAssetError(f"invalid {label} path: {value}")
    return value


def add_bytes(archive: tarfile.TarFile, name: str, value: bytes) -> None:
    name = validated_relative_path(name, "source archive member")
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
                    archive_file = validated_relative_path(
                        source.get("archive_file"), "locked source archive"
                    )
                    path = archive_root.joinpath(*pathlib.PurePosixPath(archive_file).parts)
                    if not path.is_file() or sha256(path) != source["archive_sha256"]:
                        raise LegalAssetError(f"missing or mismatched source archive: {path}")
                    add_file(archive, path, f"archives/{archive_file}")
                for patch in lock["patches"]:
                    patch_path = validated_relative_path(
                        patch.get("path"), "locked patch"
                    )
                    path = (repository_root / "3rdparty/libimobiledevice").joinpath(
                        *pathlib.PurePosixPath(patch_path).parts
                    )
                    if not path.is_file() or sha256(path) != patch["sha256"]:
                        raise LegalAssetError(f"missing or mismatched patch: {path}")
                    add_file(
                        archive,
                        path,
                        f"3rdparty/libimobiledevice/{patch_path}",
                    )
                for relative in (
                    "packaging/ios-native/superbuild/CMakeLists.txt",
                    "scripts/prefetch_ios_native_sources.py",
                    "scripts/build_ios_native_stack.py",
                    "scripts/verify_ios_native_stack.py",
                    "scripts/build_ios_native_legal_assets.py",
                    "scripts/source_publication_identity.py",
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


def canonical_sha256(value: object) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


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


def write_overlay(output: pathlib.Path, source: dict, version: str, base_url: str) -> None:
    source_archive_hash = source["archive"]["sha256"]
    published_archive = published_source_name(version, "ios-native", source_archive_hash)
    versioned_releases_page = f"{base_url}/releases"
    continuous_release_page = f"{base_url}/releases/tag/continuous"
    (output / "ios-native-source-offer.txt").write_text(
        "Klogg iOS Native Stack Corresponding Source Offer\n"
        "================================================\n\n"
        "The corresponding-source archive is not included in the installer; it is a separate\n"
        "GitHub Release asset containing every locked source archive, all mandatory patches, and\n"
        "the disconnected build, verification, legal, receipt, and package scripts needed to\n"
        "reproduce the shipped dylibs. No Homebrew runtime, usbmuxd daemon, MobileDevice private\n"
        "framework, or command-line tool is included in the application package.\n\n"
        f"Published archive: {published_archive}\n"
        f"SHA-256: {source_archive_hash}\n"
        f"Versioned releases page: {versioned_releases_page}\n"
        f"Rolling continuous release page: {continuous_release_page}\n\n"
        "Stable versioned releases retain their matching source asset for at least three years.\n"
        "The continuous endpoint is rolling and mutable: each successful continuous publication\n"
        "replaces its packages and source assets together and does not provide archival retention.\n"
        "Use the release page above to locate the content-addressed source asset included in the\n"
        "current rolling publication; a stable-only asset is not promised under the mutable tag.\n"
        "The release-level SHA256SUMS file covers every published asset, and the publication\n"
        "manifest binds this source archive to its source-set receipt and release identity. Verify\n"
        "downloaded bytes with:\n"
        f"  shasum -a 256 {published_archive}\n"
        "and compare the result with both SHA256SUMS and the SHA-256 above. See ios-native-lgpl-replacement.txt for\n"
        "rebuild, replacement, and ad-hoc re-signing instructions.\n",
        encoding="utf-8",
    )
    replacement_guide = output / "ios-native-lgpl-replacement.txt"
    replacement_guide.write_text(
        "Klogg iOS Native LGPL Replacement Guide\n"
        "========================================\n\n"
        "The libplist, libtatsu, libimobiledevice-glue, libusbmuxd, and libimobiledevice\n"
        "dylibs are dynamically loaded from Contents/Frameworks/ios-native/lib. You may rebuild\n"
        f"or modify them from {published_archive} and replace the matching dylib files.\n\n"
        f"1. Download and verify {published_archive} using ios-native-source-offer.txt.\n"
        f"2. Extract {published_archive} and the seven locked source archives.\n"
        "3. Install CMake, Ninja, autoconf, automake, libtool, pkg-config, Perl, and Clang.\n"
        "4. Run scripts/build_ios_native_stack.py with the included lock, archives, repository\n"
        "   root, a fresh work root, an artifact root, and the architecture of your Klogg build.\n"
        "5. Replace the dylibs in Klogg.app/Contents/Frameworks/ios-native/lib, preserving names.\n"
        "6. Re-sign the modified local application ad hoc:\n"
        "     codesign --force --deep --sign - /path/to/Klogg.app\n"
        "7. Verify it before launch:\n"
        "     codesign --verify --deep --strict --verbose=2 /path/to/Klogg.app\n\n"
        "Replacing code invalidates the upstream notarized signature; the ad-hoc signature is for\n"
        "your locally modified copy. Klogg imposes no restriction on debugging or reverse\n"
        "engineering these LGPL libraries for the purpose of installing modified versions.\n",
        encoding="utf-8",
    )
    write_json(output / OVERLAY_RECEIPT, {
        "schema_version": 1,
        "receipt_kind": "component-source-overlay",
        "component": "ios-native",
        "version": version,
        "base_url": base_url,
        "source_set_receipt_sha256": sha256(output / SOURCE_SET_RECEIPT),
        "assets": [
            {"kind": "source-offer", "file_name": "ios-native-source-offer.txt",
             "sha256": sha256(output / "ios-native-source-offer.txt")},
            {"kind": "replacement-guide", "file_name": replacement_guide.name,
             "sha256": sha256(replacement_guide)},
        ],
        "distribution": {"package_required": True, "release_required": True},
    })


def generate_assets(args, lock: dict, version: str | None, base_url: str | None) -> None:
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

    source_archive = args.output / "ios-native-corresponding-source.tar.gz"
    write_deterministic_source_archive(
        source_archive, args.lock, lock, args.archive_root, args.repository_root
    )
    source_archive_hash = sha256(source_archive)

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

    closure_identity = [
        {
            "id": source["id"],
            "archive_file": source["archive_file"],
            "archive_sha256": source["archive_sha256"],
            "commit": source["commit"],
        }
        for source in sources
    ]
    patch_identity = [
        {
            "source_id": patch["source_id"],
            "path": patch["path"],
            "sha256": patch["sha256"],
            "clean_tree_sha256": patch["clean_tree_sha256"],
            "patched_tree_sha256": patch["patched_tree_sha256"],
        }
        for patch in lock["patches"]
    ]
    source_set_receipt = {
        "schema_version": 1,
        "receipt_kind": "component-source-set",
        "component": "ios-native",
        "lock_sha256": sha256(args.lock),
        "archive": {"file_name": source_archive.name, "sha256": source_archive_hash},
        "source_identity": {
            "manifest_or_closure_sha256": canonical_sha256(closure_identity),
            "tree_hash_algorithm": "sha256",
            "final_tree_sha256": lock["patches"][-1]["patched_tree_sha256"],
        },
        "patch_chain_sha256": canonical_sha256(patch_identity),
        "package_support_assets": [
            {"kind": "notices", "file_name": notice.name, "sha256": sha256(notice)},
            *[
                {
                    "kind": f"license:{item['source']}",
                    "file_name": item["path"],
                    "sha256": item["sha256"],
                }
                for item in legal_files
            ],
        ],
        "distribution": {"package_required": False, "release_required": True},
    }
    if lock["receipts"]["source"] != SOURCE_SET_RECEIPT:
        raise LegalAssetError("iOS native lock names an unsupported source-set receipt")
    source_set_path = args.output / SOURCE_SET_RECEIPT
    write_json(source_set_path, source_set_receipt)
    # Preserve the historical thin-artifact filename while consumers migrate to
    # the architecture-independent component source-set receipt.
    write_json(args.output / "ios-native-source-receipt.json", source_set_receipt)
    legal_receipt = {
        "schema_version": 1,
        "receipt_kind": "legal",
        "lock_sha256": sha256(args.lock),
        "architecture": build_receipt["architecture"],
        "deployment_target": build_receipt["deployment_target"],
        "build_receipt_sha256": sha256(args.stack_root / lock["receipts"]["build"]),
        "source_set_receipt_sha256": sha256(source_set_path),
        "license_files": legal_files,
        "notice": {"path": notice.name, "sha256": sha256(notice)},
        "sbom": {"path": sbom_path.name, "sha256": sha256(sbom_path)},
        "package": "app-bundled dynamic dylib closure",
    }
    legal_path = args.output / lock["receipts"]["legal"]
    write_json(legal_path, legal_receipt)
    if version is not None:
        write_overlay(args.output, source_set_receipt, version, base_url)


def checked_input(root: pathlib.Path, relative: str) -> pathlib.Path:
    relative = validated_relative_path(relative, "immutable core asset")
    path = root / relative
    for parent in path.parents:
        if parent == root:
            break
        if parent.is_symlink() or not parent.is_dir():
            raise LegalAssetError("immutable core asset has an unsafe parent")
    if path.is_symlink() or not stat.S_ISREG(path.stat().st_mode):
        raise LegalAssetError(f"missing regular immutable core asset: {relative}")
    return path


def check_output(output: pathlib.Path, inputs: tuple[pathlib.Path, ...]) -> None:
    for root in inputs:
        if root.is_symlink() or not root.is_dir():
            raise LegalAssetError("immutable input must be a real directory")
        resolved, destination = root.resolve(), output.resolve()
        if resolved == destination or resolved in destination.parents or destination in resolved.parents:
            raise LegalAssetError("legal output must be separate from immutable inputs")
    if output.is_symlink() or (output.exists() and (not output.is_dir() or any(output.iterdir()))):
        raise LegalAssetError("legal output must be a fresh empty directory")
    if not output.parent.is_dir():
        raise LegalAssetError("legal output parent must already exist")


def strict_build_receipt(lock_path: pathlib.Path, lock: dict, stack: pathlib.Path) -> tuple[dict, list[dict]]:
    from verify_ios_native_stack import validate_build_receipt

    checked_input(stack, lock["receipts"]["build"])
    receipt, dylibs = validated_build_receipt(lock_path, lock, stack)
    validate_build_receipt(lock, lock_path, receipt, receipt["architecture"])
    for item in dylibs:
        checked_input(stack, "lib/" + item["name"])
    return receipt, dylibs


def validate_materialization_core(args, lock: dict, core: pathlib.Path) -> dict[str, str]:
    """Check caller-pinned bytes and local bindings, not signature authenticity."""
    from verify_ios_native_stack import (
        read_json, validate_build_source_set_binding, verify_legal_core,
    )

    for name, expected in ((SOURCE_SET_RECEIPT, args.source_set_sha256),
                           (lock["receipts"]["legal"], args.legal_receipt_sha256)):
        path = checked_input(core, name)
        if sha256(path) != expected:
            raise LegalAssetError(f"external immutable receipt digest mismatch: {name}")
    build_path = checked_input(args.stack_root, lock["receipts"]["build"])
    if sha256(build_path) != args.build_receipt_sha256:
        raise LegalAssetError("external immutable build receipt digest mismatch")
    build, dylibs = strict_build_receipt(args.lock, lock, args.stack_root)
    validate_build_source_set_binding(build, args.source_set_sha256)
    source = read_json(core / SOURCE_SET_RECEIPT, "iOS native source set")
    legal = read_json(core / lock["receipts"]["legal"], "iOS native legal core")
    sbom = read_json(core / lock["receipts"]["sbom"], "iOS native SBOM")
    if any(item.get("kind") in ("source-offer", "replacement-guide")
           for item in source.get("package_support_assets", [])):
        raise LegalAssetError("overlay materialization requires an unversioned source core")
    verify_legal_core(args.lock, build["architecture"], core / SOURCE_SET_RECEIPT,
                      core / lock["receipts"]["legal"], core / lock["receipts"]["sbom"],
                      asset_scope="release", source_assets_root=core, build_receipt_path=build_path)
    expected_files = [{"fileName": "lib/" + item["name"],
                       "checksums": [{"algorithm": "SHA256", "checksumValue": item["sha256"]}]}
                      for item in dylibs]
    actual_files = [{"fileName": item.get("fileName"), "checksums": item.get("checksums")}
                    for item in sbom.get("files", []) if isinstance(item, dict)]
    if actual_files != expected_files:
        raise LegalAssetError("immutable SBOM does not bind the original dylib bytes")
    names = {SOURCE_SET_RECEIPT, lock["receipts"]["legal"], lock["receipts"]["sbom"],
             source["archive"]["file_name"], legal["notice"]["path"]}
    names.update(item["file_name"] for item in source["package_support_assets"])
    names.update(item["path"] for item in legal["license_files"])
    alias = "ios-native-source-receipt.json"
    if (core / alias).exists() or (core / alias).is_symlink():
        if checked_input(core, alias).read_bytes() != (core / SOURCE_SET_RECEIPT).read_bytes():
            raise LegalAssetError("historical source receipt differs from immutable source set")
        names.add(alias)
    return {name: sha256(checked_input(core, name)) for name in sorted(names)}


def copy_checked_files(source: pathlib.Path, output: pathlib.Path, files: dict[str, str],
                       created: list[pathlib.Path] | None = None) -> None:
    """Stream bounded immutable copies, rejecting changed inputs or copied bytes."""
    total = 0
    for name, expected in files.items():
        path = checked_input(source, name)
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        with path.open("rb") as original, destination.open("xb") as copied:
            if created is not None:
                created.append(destination)
            for chunk in iter(lambda: original.read(1024 * 1024), b""):
                total += len(chunk)
                if total > 2 * 1024**3:
                    raise LegalAssetError("immutable legal core exceeds copy budget")
                digest.update(chunk)
                copied.write(chunk)
        if digest.hexdigest() != expected or sha256(destination) != expected:
            raise LegalAssetError(f"immutable legal core changed while copying: {name}")


def materialize(args, lock: dict, version: str | None, base_url: str | None) -> None:
    inputs = (args.stack_root, args.core_root) if args.overlay_only else (args.stack_root,)
    check_output(args.output, inputs)
    if args.overlay_only:
        files = validate_materialization_core(args, lock, args.core_root)
    else:
        strict_build_receipt(args.lock, lock, args.stack_root)
    with tempfile.TemporaryDirectory(prefix=".ios-legal-", dir=args.output.parent) as directory:
        stage = pathlib.Path(directory)
        if args.overlay_only:
            copy_checked_files(args.core_root, stage, files)
            # Check the copied snapshot against the same external pins before
            # generating its new overlay, without rebuilding any source bytes.
            if validate_materialization_core(args, lock, stage) != files:
                raise LegalAssetError("copied immutable legal core differs from its input")
            source = json.loads((stage / SOURCE_SET_RECEIPT).read_text(encoding="utf-8"))
            write_overlay(stage, source, version, base_url)
        else:
            staged_args = argparse.Namespace(**{**vars(args), "output": stage})
            generate_assets(staged_args, lock, None, None)
        files = {path.relative_to(stage).as_posix(): sha256(path)
                 for path in stage.rglob("*") if path.is_file()}
        check_output(args.output, inputs)
        existed = args.output.exists()
        created = []
        try:
            args.output.mkdir(exist_ok=True)
            copy_checked_files(stage, args.output, files, created)
            if any(sha256(args.output / name) != expected for name, expected in files.items()):
                raise LegalAssetError("materialized legal core changed before completion")
        except BaseException:
            for path in reversed(created):
                path.unlink(missing_ok=True)
            for path in sorted(args.output.rglob("*"), reverse=True):
                if path.is_dir():
                    try:
                        path.rmdir()
                    except OSError:
                        pass
            if not existed:
                args.output.rmdir()
            raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--archive-root", type=pathlib.Path)
    parser.add_argument("--repository-root", type=pathlib.Path)
    parser.add_argument("--stack-root", required=True, type=pathlib.Path)
    parser.add_argument("--version")
    parser.add_argument("--base-url")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--core-only", action="store_true")
    mode.add_argument("--overlay-only", action="store_true")
    parser.add_argument("--core-root", type=pathlib.Path)
    parser.add_argument("--source-set-sha256")
    parser.add_argument("--legal-receipt-sha256")
    parser.add_argument("--build-receipt-sha256")
    args = parser.parse_args()
    pins = (args.source_set_sha256, args.legal_receipt_sha256, args.build_receipt_sha256)
    if args.overlay_only:
        if not args.core_root or args.archive_root or args.repository_root:
            parser.error("overlay-only requires core-root and forbids source acquisition inputs")
        if any(not isinstance(pin, str) or re.fullmatch(r"[0-9a-f]{64}", pin) is None
               or pin == "0" * 64 for pin in pins):
            parser.error("overlay-only requires three external immutable receipt SHA-256 pins")
    elif not args.archive_root or not args.repository_root or args.core_root or any(pins):
        parser.error("core generation requires archive-root and repository-root, not reused-core inputs")
    if args.core_only:
        if args.version or args.base_url:
            parser.error("core-only forbids version and publication URL")
        version, base_url = None, None
    else:
        if not args.version or not args.base_url:
            parser.error("version and base-url are required when generating an overlay")
        version, base_url = validate_version(args.version), normalize_base_url(args.base_url)
    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    if args.core_only or args.overlay_only:
        materialize(args, lock, version, base_url)
    else:
        args.output.mkdir(parents=True, exist_ok=True)
        generate_assets(args, lock, version, base_url)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
