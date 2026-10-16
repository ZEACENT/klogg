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
import os
import pathlib
import re
import shutil
import tarfile
import tempfile

from source_publication_identity import (
    normalize_base_url,
    published_source_name,
    validate_version,
)


RAW_ARCHIVE_IDENTITY = "raw-sha256"
CANONICAL_TAR_GZ_IDENTITY = "canonical-tar-gz-v1"
SUPPORTED_ARCHIVE_IDENTITIES = {RAW_ARCHIVE_IDENTITY, CANONICAL_TAR_GZ_IDENTITY}
# Assets whose bytes embed the application version or publication URLs. They
# are generated per release, never referenced by the core source-set receipt.
OVERLAY_KINDS = {"source-offer", "overlay-receipt"}


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


def release_asset_plan(lock: dict) -> list[dict]:
    assets = lock.get("release_assets")
    if not isinstance(assets, list) or not assets:
        raise RuntimeError("ADB lock release_assets must be a non-empty array")

    seen_kinds: set[str] = set()
    seen_paths: set[str] = set()
    validated = []
    for asset in assets:
        if not isinstance(asset, dict):
            raise RuntimeError("ADB release asset must be an object")
        kind = asset.get("kind")
        if not isinstance(kind, str) or not kind or kind in seen_kinds:
            raise RuntimeError(f"invalid or duplicate ADB release asset kind: {kind}")
        seen_kinds.add(kind)
        if asset.get("required") is not True:
            raise RuntimeError(f"ADB release asset must be required: {kind}")

        # Core assets are deterministic functions of the lock; overlay assets
        # are the only ones allowed to embed the application version or
        # publication URLs, so version bumps never invalidate the core
        # identity that binary-build receipts bind.
        ownership = asset.get("ownership")
        if ownership not in ("core", "overlay"):
            raise RuntimeError(f"invalid ADB release asset ownership: {kind}")
        expected_ownership = "overlay" if kind in OVERLAY_KINDS else "core"
        if ownership != expected_ownership:
            raise RuntimeError(f"unexpected ADB release asset ownership: {kind}")

        distribution = asset.get("distribution")
        if not isinstance(distribution, dict) or set(distribution) != {
            "package_required",
            "release_required",
        }:
            raise RuntimeError(f"invalid ADB release asset distribution: {kind}")
        if not all(
            isinstance(distribution[field], bool)
            for field in ("package_required", "release_required")
        ):
            raise RuntimeError(f"non-boolean ADB release asset distribution: {kind}")
        if distribution["release_required"] is not True:
            raise RuntimeError(f"ADB release asset is outside the full release closure: {kind}")

        for field in ("file_name", "sha256_file"):
            value = asset.get(field)
            if not isinstance(value, str):
                raise RuntimeError(f"ADB release asset lacks {field}: {kind}")
            relative = safe_archive_member_name(value, f"ADB release asset {field}")
            if pathlib.PurePosixPath(relative).name != relative:
                raise RuntimeError(
                    f"ADB release asset {field} must be a flat file name: {kind}"
                )
            if relative in seen_paths:
                raise RuntimeError(f"duplicate ADB release asset path: {relative}")
            seen_paths.add(relative)
        validated.append(asset)
    return validated


def materialize_package_support(
    release_root: pathlib.Path,
    package_support_root: pathlib.Path,
    release_assets: list[dict],
) -> None:
    release_resolved = release_root.resolve()
    support_resolved = package_support_root.resolve()
    if (
        release_resolved == support_resolved
        or release_resolved in support_resolved.parents
        or support_resolved in release_resolved.parents
    ):
        raise RuntimeError("ADB release and package-support outputs must be disjoint")
    if package_support_root.is_symlink():
        raise RuntimeError("ADB package-support output must not be a symlink")
    if package_support_root.exists():
        if not package_support_root.is_dir():
            raise RuntimeError("ADB package-support output must be a directory")
        shutil.rmtree(package_support_root)
    package_support_root.mkdir(parents=True)

    for asset in release_assets:
        if asset["distribution"]["package_required"] is not True:
            continue
        for field in ("file_name", "sha256_file"):
            relative = pathlib.PurePosixPath(asset[field])
            source = release_root.joinpath(*relative.parts)
            if not source.is_file() or source.is_symlink():
                raise RuntimeError(f"missing package-support projection source: {source}")
            destination = package_support_root.joinpath(*relative.parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)


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


def write_source_offer(
    path: pathlib.Path, version: str, base_url: str, source_archive_hash: str
) -> None:
    published_archive = published_source_name(version, "adb-helper", source_archive_hash)
    versioned_releases_page = f"{base_url}/releases"
    continuous_release_page = f"{base_url}/releases/tag/continuous"
    path.write_text(
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
        "The release-level SHA256SUMS file covers every published asset, and the publication\n"
        "manifest binds this source archive to its source-set receipt and release identity. Verify\n"
        "downloaded bytes with:\n"
        f"  shasum -a 256 {published_archive}\n"
        "and compare the result with both SHA256SUMS and the SHA-256 above. Build instructions are included in the\n"
        "archive at packaging/adb/README.md.\n\n"
        "Linux packages place the LGPL-2.1-or-later libusb shared library beside adb and resolve it\n"
        "through the relative $ORIGIN runpath. You may replace that libusb file with a modified,\n"
        "ABI-compatible version. The source archive also contains the material needed to rebuild\n"
        "and relink adb against a modified libusb. No libusb library is shipped on macOS.\n",
        encoding="utf-8",
    )


def write_overlay_receipt(
    output: pathlib.Path, release_assets: list[dict], version: str, base_url: str,
    *, source_archive_sha256: str | None = None,
) -> None:
    by_kind = {asset["kind"]: asset for asset in release_assets}
    source_set_path = output / by_kind["source-set-receipt"]["file_name"]
    if source_archive_sha256 is None:
        source_archive_sha256 = sha256(output / by_kind["source-archive"]["file_name"])
    write_source_offer(
        output / by_kind["source-offer"]["file_name"],
        version, base_url, source_archive_sha256,
    )
    overlay_records = []
    for asset in release_assets:
        if asset["ownership"] != "overlay" or asset["kind"] == "overlay-receipt":
            continue
        path = output / asset["file_name"]
        if not path.is_file():
            raise RuntimeError(f"required ADB overlay asset was not generated: {path}")
        overlay_records.append(
            {"kind": asset["kind"], "file_name": path.name, "sha256": sha256(path)}
        )
    overlay_receipt = {
        "schema_version": 1,
        "receipt_kind": "component-source-overlay",
        "component": "adb-helper",
        "version": version,
        "base_url": base_url,
        "source_set_receipt_sha256": sha256(source_set_path),
        "assets": overlay_records,
        "distribution": {"package_required": True, "release_required": True},
    }
    overlay_path = output / by_kind["overlay-receipt"]["file_name"]
    overlay_path.write_text(
        json.dumps(overlay_receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def write_release_hashes(
    output: pathlib.Path, release_assets: list[dict], *, preserve_core_sidecars: bool = False,
) -> None:
    receipt_assets = []
    for asset in release_assets:
        path = output / asset["file_name"]
        if not path.is_file() or path.is_symlink():
            raise RuntimeError(f"required ADB release asset was not generated: {path}")
        if not preserve_core_sidecars or asset["ownership"] != "core":
            write_hash(path)
        receipt_assets.append({"kind": asset["kind"], "path": path.name, "sha256": sha256(path)})
    (output / "adb-helper-release-assets.json").write_text(
        json.dumps(receipt_assets, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def check_overlay_paths(
    outputs: list[pathlib.Path], inputs: list[pathlib.Path],
) -> None:
    for path in [*inputs, *outputs]:
        if any(parent.is_symlink() for parent in (path, *path.parents)):
            raise RuntimeError("ADB immutable inputs and overlay outputs must not use symlinks")
    for index, output in enumerate(outputs):
        resolved = output.resolve()
        for other in [*inputs, *outputs[:index]]:
            other_resolved = other.resolve()
            if (resolved == other_resolved or resolved in other_resolved.parents
                    or other_resolved in resolved.parents):
                raise RuntimeError("ADB overlay outputs must be separate and disjoint from inputs")


def check_overlay_outputs(outputs: list[pathlib.Path], inputs: list[pathlib.Path]) -> None:
    check_overlay_paths(outputs, inputs)
    for output in outputs:
        if output.exists() and (not output.is_dir() or any(output.iterdir())):
            raise RuntimeError("ADB overlay output must be a fresh empty directory")
        if not output.parent.is_dir():
            raise RuntimeError("ADB overlay output parent must already exist")


def copy_checked_asset(
    source: pathlib.Path, destination: pathlib.Path, expected: str,
    created: dict[pathlib.Path, tuple[int, int]], *,
    output_identity: tuple[int, int] | None = None,
) -> None:
    from verify_adb_helper_artifact import require_regular_file

    # Recheck acquired ownership before each copy. Portable path checks do
    # not provide atomic isolation against arbitrary concurrent renames.
    if output_identity is not None:
        parent = destination.parent
        if any(path.is_symlink() for path in (parent, *parent.parents)):
            raise RuntimeError("ADB acquired output must not use symlinks")
        identity = parent.stat()
        if (identity.st_dev, identity.st_ino) != output_identity:
            raise RuntimeError("ADB acquired output directory ownership identity changed")
    require_regular_file(source, "ADB immutable copy input")
    digest = hashlib.sha256()
    with source.open("rb") as original, destination.open("xb") as copied:
        identity = os.fstat(copied.fileno())
        created[destination] = (identity.st_dev, identity.st_ino)
        for chunk in iter(lambda: original.read(1024 * 1024), b""):
            digest.update(chunk)
            copied.write(chunk)
    if digest.hexdigest() != expected or sha256(destination) != expected:
        raise RuntimeError(f"ADB immutable asset changed while copying: {source.name}")


def materialize_package_core(
    release_root: pathlib.Path, output: pathlib.Path, release_assets: list[dict],
) -> None:
    from verify_adb_helper_artifact import require_regular_file

    check_overlay_outputs([output], [release_root])
    files = {}
    for asset in release_assets:
        if asset["ownership"] != "core" or not asset["distribution"]["package_required"]:
            continue
        for field in ("file_name", "sha256_file"):
            path = release_root / asset[field]
            require_regular_file(path, "ADB package core projection input")
            files[path.name] = sha256(path)
    output.mkdir(exist_ok=True)
    identity = output.stat()
    output_identity = (identity.st_dev, identity.st_ino)
    for name, digest in files.items():
        copy_checked_asset(release_root / name, output / name, digest, {},
                           output_identity=output_identity)
    check_overlay_paths([output], [release_root])
    current = output.stat()
    if ((current.st_dev, current.st_ino) != output_identity
            or {path.name for path in output.iterdir()} != set(files)):
        raise RuntimeError("ADB package core projection changed before completion")
    for name, digest in files.items():
        for path in (release_root / name, output / name):
            require_regular_file(path, "ADB package core projection entry")
            if sha256(path) != digest:
                raise RuntimeError("ADB package core projection changed before completion")


def materialize_overlay_from_core(
    lock_path: pathlib.Path, lock: dict, release_assets: list[dict],
    core_root: pathlib.Path, output: pathlib.Path,
    package_support_output: pathlib.Path | None, version: str, base_url: str,
    build_receipt_path: pathlib.Path, source_set_sha256: str, build_receipt_sha256: str,
    *, asset_scope: str = "release",
) -> None:
    from verify_adb_helper_artifact import (
        BINARY_BUILD_RECEIPT, VerificationError, find_receipt_asset, has_exact_schema,
        read_json, require_regular_file,
        validate_overlay_receipt, validate_source_set_receipt, verify_hash_sidecar,
    )

    if asset_scope not in ("release", "package"):
        raise RuntimeError("unsupported ADB overlay asset scope")
    if asset_scope == "package" and package_support_output is not None:
        raise RuntimeError("package overlay accepts only one package output")
    output_assets = [asset for asset in release_assets
                     if asset_scope == "release" or asset["distribution"]["package_required"]]
    # External expected-byte pins and legal checks are not signature
    # authentication, native binary qualification, or publication authority.
    outputs = [output] + ([package_support_output] if package_support_output is not None else [])
    inputs = [core_root, lock_path, build_receipt_path]
    check_overlay_outputs(outputs, inputs)
    if not core_root.is_dir():
        raise RuntimeError("ADB immutable core must be a real directory")
    core_assets = [asset for asset in output_assets if asset["ownership"] == "core"]
    source_set_asset = next(asset for asset in core_assets if asset["kind"] == "source-set-receipt")

    def validate_core(root: pathlib.Path) -> tuple[dict, dict[str, str]]:
        source_set_path = root / source_set_asset["file_name"]
        for path, expected in ((source_set_path, source_set_sha256),
                               (build_receipt_path, build_receipt_sha256)):
            require_regular_file(path, "ADB immutable receipt")
            if sha256(path) != expected:
                raise RuntimeError(f"external immutable receipt digest mismatch: {path.name}")
        build_receipt = read_json(build_receipt_path, "ADB original binary-build receipt")
        if not has_exact_schema(build_receipt, 1):
            raise RuntimeError("ADB original binary-build receipt has an unsupported schema")
        if build_receipt.get("receipt_kind", BINARY_BUILD_RECEIPT) != BINARY_BUILD_RECEIPT:
            raise RuntimeError("ADB original receipt is not binary-build evidence")
        if build_receipt.get("lock_sha256") != sha256(lock_path):
            raise RuntimeError("ADB original binary-build receipt lock sha256 mismatch")
        validate_source_set_receipt(lock, lock_path, build_receipt, root, asset_scope)
        files = {}
        for asset in core_assets:
            path = root / asset["file_name"]
            require_regular_file(path, f"ADB core asset {asset['kind']}")
            digest = sha256(path)
            sidecar = root / asset["sha256_file"]
            verify_hash_sidecar(sidecar, digest, path.name)
            if asset["distribution"]["package_required"]:
                record = find_receipt_asset(build_receipt, asset["kind"])
                if record.get("path") != asset["file_name"] or record.get("sha256") != digest:
                    raise RuntimeError(f"ADB original build support sha256 or path mismatch: {asset['kind']}")
            files[path.name] = digest
            files[sidecar.name] = sha256(sidecar)
        return build_receipt, files

    # No output exists until pins, original bindings, archive and support
    # checks have all succeeded. Never use the destructive default projection.
    created_roots: list[tuple[pathlib.Path, int, int, bool]] = []
    created: dict[pathlib.Path, tuple[int, int]] = {}
    try:
        original_build, files = validate_core(core_root)
        with tempfile.TemporaryDirectory(prefix=".adb-overlay-", dir=output.parent) as directory:
            stage = pathlib.Path(directory)
            for name, digest in files.items():
                copy_checked_asset(core_root / name, stage / name, digest, {})
            if validate_core(stage) != (original_build, files):
                raise RuntimeError("ADB copied immutable core differs from original inputs")
            archive_hash = None
            if asset_scope == "package":
                source_set = read_json(stage / source_set_asset["file_name"], "ADB source-set receipt")
                archive_hash = source_set["archive"]["sha256"]
                if archive_hash == "0" * 64:
                    raise RuntimeError("ADB package overlay source archive identity must be nonzero")
            write_overlay_receipt(stage, release_assets, version, base_url,
                                  source_archive_sha256=archive_hash)
            write_release_hashes(stage, output_assets, preserve_core_sidecars=True)
            expected_names = {asset[field] for asset in output_assets
                              for field in ("file_name", "sha256_file")}
            index_path = stage / "adb-helper-release-assets.json"
            if {path.name for path in stage.iterdir()} != expected_names | {index_path.name}:
                raise RuntimeError("ADB release output file coverage mismatch")
            expected_index = [{"kind": asset["kind"], "path": asset["file_name"],
                               "sha256": sha256(stage / asset["file_name"])}
                              for asset in output_assets]
            require_regular_file(index_path, "ADB release asset index")
            if json.loads(index_path.read_text(encoding="utf-8")) != expected_index:
                raise RuntimeError("ADB release asset index mismatch")
            if validate_core(stage) != (original_build, files):
                raise RuntimeError("ADB staged immutable core differs from original inputs")
            validate_overlay_receipt(lock, stage, asset_scope, source_set_sha256)
            release_files = {path.name: sha256(path) for path in stage.iterdir()}
            support_files = {asset[field]: release_files[asset[field]]
                             for asset in release_assets if asset["distribution"]["package_required"]
                             for field in ("file_name", "sha256_file")}
            for root in outputs:
                # The other projection may have changed during the preceding
                # root's copies. Check this root immediately before acquisition.
                boundaries = [*inputs, *(other for other in outputs if other != root)]
                check_overlay_outputs([root], boundaries)
                existed = root.exists()
                root.mkdir(exist_ok=existed)
                check_overlay_outputs([root], boundaries)
                identity = root.stat()
                output_identity = (identity.st_dev, identity.st_ino)
                created_roots.append((root, *output_identity, existed))
                expected = release_files if root == output and asset_scope == "release" else support_files
                for name, digest in expected.items():
                    copy_checked_asset(stage / name, root / name, digest, created,
                                       output_identity=output_identity)
            check_overlay_paths(outputs, inputs)
            if (validate_core(core_root) != (original_build, files)
                    or validate_core(output) != (original_build, files)):
                raise RuntimeError("ADB materialized immutable core differs from original inputs")
            validate_overlay_receipt(lock, output, asset_scope, source_set_sha256)
            if package_support_output is not None:
                validate_source_set_receipt(lock, lock_path, original_build,
                                            package_support_output, "package")
                validate_overlay_receipt(lock, package_support_output, "package", source_set_sha256)
            for root in outputs:
                expected = release_files if root == output and asset_scope == "release" else support_files
                for name in expected:
                    require_regular_file(root / name, "ADB materialized output asset")
                if {path.name: sha256(path) for path in root.iterdir()} != expected:
                    raise RuntimeError("ADB materialized output changed before completion")
    except BaseException as error:
        # These roots were checked empty, disjoint and nonsymlink before we
        # acquired them. Do not remove a replacement root or immutable input.
        for root, device, inode, existed in reversed(created_roots):
            if not root.is_symlink() and root.exists():
                identity = root.stat()
                if (identity.st_dev, identity.st_ino) == (device, inode):
                    for path, expected_identity in created.items():
                        if path.parent == root and not path.is_symlink() and path.is_file():
                            file_identity = path.stat()
                            if (file_identity.st_dev, file_identity.st_ino) == expected_identity:
                                path.unlink()
                    if not existed and not any(root.iterdir()):
                        root.rmdir()
        if isinstance(error, VerificationError):
            raise RuntimeError(f"ADB core asset verification failed: {error}") from error
        raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=pathlib.Path)
    parser.add_argument("--archive-root", type=pathlib.Path)
    parser.add_argument("--repository-root", type=pathlib.Path)
    parser.add_argument("--version")
    parser.add_argument("--base-url")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--package-support-output", type=pathlib.Path)
    parser.add_argument("--package-core-output", type=pathlib.Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--core-only", action="store_true")
    mode.add_argument("--overlay-only", action="store_true")
    mode.add_argument("--package-overlay-only", action="store_true")
    parser.add_argument("--core-root", type=pathlib.Path)
    parser.add_argument("--build-receipt", type=pathlib.Path)
    parser.add_argument("--source-set-sha256")
    parser.add_argument("--build-receipt-sha256")
    args = parser.parse_args()
    overlay_only = args.overlay_only or args.package_overlay_only
    pins = (args.source_set_sha256, args.build_receipt_sha256)
    if overlay_only:
        if not args.build_receipt or any(
            not isinstance(pin, str) or re.fullmatch(r"[0-9a-f]{64}", pin) is None
            or pin == "0" * 64 for pin in pins
        ):
            parser.error("overlay-only requires original build-receipt and two external immutable receipt SHA-256 pins")
    elif args.build_receipt or any(pin is not None for pin in pins):
        parser.error("original build receipt and external pins are overlay-only inputs")

    if args.core_only:
        if args.version or args.base_url or args.core_root or args.package_support_output:
            parser.error("core-only accepts neither version, publication URL, nor overlay outputs")
    elif not args.version or not args.base_url:
        parser.error("version and base-url are required when generating an overlay")
    if overlay_only:
        if not args.core_root or args.archive_root or args.repository_root:
            parser.error("overlay-only requires core-root and forbids source acquisition inputs")
        if args.package_core_output:
            parser.error("package-core-output is only available during core generation")
        if args.package_overlay_only and args.package_support_output:
            parser.error("package-overlay-only accepts only one package output")
    elif not args.archive_root or not args.repository_root or args.core_root:
        parser.error("core generation requires archive-root and repository-root, not core-root")

    version = validate_version(args.version) if not args.core_only else None
    base_url = normalize_base_url(args.base_url) if not args.core_only else None
    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    release_assets = release_asset_plan(lock)
    if overlay_only:
        materialize_overlay_from_core(
            args.lock, lock, release_assets, args.core_root, args.output,
            args.package_support_output, version, base_url,
            args.build_receipt, args.source_set_sha256, args.build_receipt_sha256,
            asset_scope="package" if args.package_overlay_only else "release",
        )
        return 0
    args.output.mkdir(parents=True, exist_ok=True)
    if args.package_core_output is not None:
        boundaries = [args.output, args.archive_root, args.lock]
        if args.package_support_output is not None:
            boundaries.append(args.package_support_output)
        check_overlay_outputs([args.package_core_output], boundaries)
    if args.core_only and any(args.output.iterdir()):
        raise RuntimeError("ADB core output must be empty; stale overlay assets are forbidden")
    by_kind = {asset["kind"]: asset for asset in release_assets}

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
            "scripts/prefetch_adb_manifest_fallback.py",
            "scripts/prefetch_adb_source_context.py",
            "scripts/prefetch_adb_libusb_fallback.py",
            "scripts/prefetch_adb_source_closure.py",
            "scripts/ci_adb_source_transport.py",
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


    source_set_asset = by_kind["source-set-receipt"]
    package_support_assets = []
    for asset in release_assets:
        distribution = asset["distribution"]
        if (
            asset["kind"] == "source-set-receipt"
            or asset["ownership"] == "overlay"
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

    if args.core_only:
        write_release_hashes(
            args.output, [asset for asset in release_assets if asset["ownership"] == "core"]
        )
    else:
        write_overlay_receipt(args.output, release_assets, version, base_url)
        write_release_hashes(args.output, release_assets)
        if args.package_support_output is not None:
            materialize_package_support(args.output, args.package_support_output, release_assets)
    if args.package_core_output is not None:
        materialize_package_core(args.output, args.package_core_output, release_assets)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
