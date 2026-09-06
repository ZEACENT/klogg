#!/usr/bin/env python3
"""Promote one downloaded Continuous publication into an immutable Stable set."""

from __future__ import annotations

import argparse
import copy
import json
import os
import pathlib
import re
import shutil
import sys
import tempfile

from prepare_source_publication import CHECKSUMS_NAME, MANIFEST_NAME, write_checksums
from source_publication_identity import normalize_base_url, source_asset_url
from verify_source_publication_manifest import (
    PublicationError,
    read_json,
    regular_asset_name,
    require_fields,
    require_sha256,
    sha256,
    verify_manifest,
)


SOURCE_METADATA_FIELDS = {
    "schema_version",
    "metadata_kind",
    "source_release_id",
    "source_tag_sha",
    "source_assets",
}
SOURCE_ASSET_FIELDS = {"id", "name", "sha256"}
SOURCE_METADATA_KIND = "klogg-continuous-publication-source"


def infer_base_url(page_url: object) -> str:
    suffix = "/releases/tag/continuous"
    if not isinstance(page_url, str) or not page_url.endswith(suffix):
        raise PublicationError("Continuous release page URL is malformed")
    return normalize_base_url(page_url[: -len(suffix)])


def read_source_metadata(path: pathlib.Path) -> tuple[int, str, dict[str, dict]]:
    document = require_fields(
        read_json(path, "Continuous release metadata"),
        SOURCE_METADATA_FIELDS,
        "Continuous release metadata",
    )
    if type(document["schema_version"]) is not int or document["schema_version"] != 1:
        raise PublicationError("invalid Continuous release metadata schema version")
    if document["metadata_kind"] != SOURCE_METADATA_KIND:
        raise PublicationError("invalid Continuous release metadata identity")
    release_id = document["source_release_id"]
    if type(release_id) is not int or release_id <= 0:
        raise PublicationError("invalid Continuous source release ID")
    tag_sha = document["source_tag_sha"]
    if not isinstance(tag_sha, str) or re.fullmatch(r"[0-9a-f]{40}", tag_sha) is None:
        raise PublicationError("invalid Continuous source tag SHA")

    raw_assets = document["source_assets"]
    if not isinstance(raw_assets, list):
        raise PublicationError("invalid Continuous source asset list")
    assets: dict[str, dict] = {}
    asset_ids: set[int] = set()
    for offset, raw_asset in enumerate(raw_assets):
        asset = require_fields(
            raw_asset, SOURCE_ASSET_FIELDS, f"Continuous source asset {offset}"
        )
        asset_id = asset["id"]
        if type(asset_id) is not int or asset_id <= 0:
            raise PublicationError(f"invalid Continuous source asset ID: {offset}")
        if asset_id in asset_ids:
            raise PublicationError(f"duplicate Continuous source asset ID: {asset_id}")
        asset_ids.add(asset_id)
        name = regular_asset_name(asset["name"], "Continuous source asset")
        if name in assets:
            raise PublicationError(f"duplicate Continuous source asset name: {name}")
        digest = require_sha256(asset["sha256"], f"Continuous source asset {name}")
        assets[name] = {"id": asset_id, "name": name, "sha256": digest}
    return release_id, tag_sha, assets


def verify_source_inventory(
    source_root: pathlib.Path, source_assets: dict[str, dict]
) -> None:
    actual_assets: set[str] = set()
    for path in source_root.iterdir():
        if path.is_symlink():
            raise PublicationError(f"Continuous publication contains symlink: {path.name}")
        if not path.is_file():
            raise PublicationError(
                f"Continuous publication contains non-asset entry: {path.name}"
            )
        name = regular_asset_name(path.name, "Continuous release asset")
        actual_assets.add(name)
    if actual_assets != set(source_assets):
        missing = sorted(set(source_assets) - actual_assets)
        extra = sorted(actual_assets - set(source_assets))
        raise PublicationError(
            f"Continuous release metadata coverage mismatch; missing={missing}, extra={extra}"
        )
    for name, record in source_assets.items():
        path = source_root / name
        if sha256(path) != record["sha256"]:
            raise PublicationError(f"Continuous source asset hash mismatch: {name}")


def stable_manifest(
    source_document: dict,
    base_url: str,
    source_release_id: int,
    source_tag_sha: str,
    source_manifest_sha256: str,
    source_assets: dict[str, dict],
) -> dict:
    document = copy.deepcopy(source_document)
    release = document["release"]
    version = release["version"]
    stable_tag = f"v{version}"
    document["schema_version"] = 3
    document["channel"] = "stable"
    release.update(
        {
            "tag": stable_tag,
            "mutable": False,
            "public_name": f"Release v{version}",
            "page_url": f"{base_url}/releases/tag/{stable_tag}",
            "direct_asset_urls_are_archival": True,
        }
    )
    for component in document["components"].values():
        archive = component["source_archive"]
        archive["url"] = source_asset_url(base_url, stable_tag, archive["file_name"])
    document["promotion"] = {
        "source_release_id": source_release_id,
        "source_tag_sha": source_tag_sha,
        "source_manifest_sha256": source_manifest_sha256,
        "source_commit": release["commit"],
        "source_assets": [source_assets[name] for name in sorted(source_assets)],
    }
    return document


def promote_publication(
    source_manifest: pathlib.Path,
    source_assets_root: pathlib.Path,
    source_metadata: pathlib.Path,
    output: pathlib.Path,
) -> None:
    if source_manifest.name != MANIFEST_NAME:
        raise PublicationError(f"invalid Continuous manifest file name: {source_manifest.name}")
    if source_assets_root.is_symlink():
        raise PublicationError("Continuous publication root must not be a symlink")
    if output.exists() or output.is_symlink():
        raise PublicationError(f"promotion output already exists: {output}")

    source_document = read_json(source_manifest, "Continuous publication manifest")
    release = source_document.get("release")
    if not isinstance(release, dict):
        raise PublicationError("Continuous publication release identity is missing")
    version = release.get("version")
    commit = release.get("commit")
    if not isinstance(version, str) or not version or not isinstance(commit, str) or not commit:
        raise PublicationError("Continuous publication release identity is malformed")
    base_url = infer_base_url(release.get("page_url"))
    verify_manifest(
        source_manifest,
        source_assets_root,
        "continuous",
        "validation",
        "continuous",
        version,
        commit,
        base_url,
    )
    if source_document.get("schema_version") != 2:
        raise PublicationError("promotion source must use publication schema v2")

    source_release_id, source_tag_sha, source_assets = read_source_metadata(
        source_metadata
    )
    if source_tag_sha != commit:
        raise PublicationError("Continuous source tag SHA does not match source commit")
    verify_source_inventory(source_assets_root, source_assets)
    source_manifest_sha256 = sha256(source_manifest)
    manifest_record = source_assets.get(MANIFEST_NAME)
    if manifest_record is None or manifest_record["sha256"] != source_manifest_sha256:
        raise PublicationError("Continuous source manifest metadata binding mismatch")

    output.parent.mkdir(parents=True, exist_ok=True)
    staging = pathlib.Path(
        tempfile.mkdtemp(prefix=f".{output.name}.promotion-", dir=str(output.parent))
    )
    try:
        for name, record in source_assets.items():
            if name in {MANIFEST_NAME, CHECKSUMS_NAME}:
                continue
            source = source_assets_root / name
            destination = staging / name
            if source.is_symlink() or not source.is_file():
                raise PublicationError(f"Continuous source asset changed during promotion: {name}")
            shutil.copy2(str(source), str(destination))
            if (
                source.is_symlink()
                or sha256(source) != record["sha256"]
                or destination.is_symlink()
                or sha256(destination) != record["sha256"]
            ):
                raise PublicationError(f"promoted payload differs from source asset: {name}")

        document = stable_manifest(
            source_document,
            base_url,
            source_release_id,
            source_tag_sha,
            source_manifest_sha256,
            source_assets,
        )
        promoted_manifest = staging / MANIFEST_NAME
        promoted_manifest.write_text(
            json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
        write_checksums(staging)
        verify_manifest(
            promoted_manifest,
            staging,
            "stable",
            "validation",
            f"v{version}",
            version,
            commit,
            base_url,
        )
        os.replace(str(staging), str(output))
    finally:
        if staging.exists():
            shutil.rmtree(str(staging))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-manifest", required=True, type=pathlib.Path)
    parser.add_argument("--source-assets-root", required=True, type=pathlib.Path)
    parser.add_argument("--source-metadata", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    try:
        promote_publication(
            args.source_manifest,
            args.source_assets_root,
            args.source_metadata,
            args.output,
        )
        print(f"promoted Continuous publication to Stable in {args.output}")
        return 0
    except (OSError, PublicationError, ValueError) as error:
        print(f"release publication promotion failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
