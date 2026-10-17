#!/usr/bin/env python3
"""Inspect a cross-platform CI dependency archive carried as an OCI artifact.

A native ADB/iOS core is an opaque archive blob, not a linux/amd64 image. The
manifest/descriptor contract is separate from the environment-image parser;
publication must exercise the real pinned ORAS CLI before any consumer lock
is activated. This module validates raw registry bytes and never trusts a tag.
"""

from __future__ import annotations

import datetime
import hashlib
import pathlib
import re

from ci_environment_registry import RegistryError, _json

MANIFEST_MEDIA_TYPE = "application/vnd.oci.image.manifest.v1+json"
EMPTY_CONFIG_MEDIA_TYPE = "application/vnd.oci.empty.v1+json"
EMPTY_CONFIG_DIGEST = "sha256:" + hashlib.sha256(b"{}").hexdigest()
ARTIFACT_TYPE = "application/vnd.klogg.ci-dependency-core.v1"
CORE_LAYER_MEDIA_TYPE = "application/vnd.klogg.ci-dependency-core.v1+tar+gzip"
MAX_MANIFEST_BYTES = 2 * 1024 * 1024
MAX_CORE_BYTES = 2 * 1024**3
DIGEST_RE = re.compile(r"sha256:[0-9a-f]{64}\Z")
ARCHIVE_RE = re.compile(r"(?:adb-helper-(?:linux-(?:x86_64|arm64)|windows-x86_64|macos-(?:x86_64|arm64))|ios-native-(?:x86_64|arm64))\.tar\.gz\Z")


class ArtifactError(ValueError):
    """The supplied OCI artifact identity is unsafe or does not match its lock."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ArtifactError(message)


def require_digest(value: object, label: str) -> str:
    require(isinstance(value, str) and DIGEST_RE.fullmatch(value) is not None
            and value != "sha256:" + "0" * 64, f"invalid {label}")
    return value


def validate_archive_name(name: object) -> str:
    require(isinstance(name, str) and ARCHIVE_RE.fullmatch(name) is not None
            and pathlib.PurePosixPath(name).name == name,
            "archive name must identify one supported native dependency target")
    return name


def inspect_manifest(
    raw: bytes, expected_manifest_digest: str, *,
    expected_blob_digest: str, expected_blob_size: int,
    expected_archive_name: str,
) -> dict[str, object]:
    """Check every layer/config byte identity before fetching the blob."""
    require(isinstance(raw, bytes) and 0 < len(raw) <= MAX_MANIFEST_BYTES,
            "missing or oversized dependency manifest")
    require_digest(expected_manifest_digest, "manifest digest")
    require("sha256:" + hashlib.sha256(raw).hexdigest() == expected_manifest_digest,
            "dependency manifest digest mismatch")
    try:
        document = _json(raw)
    except RegistryError as error:
        raise ArtifactError("invalid dependency manifest JSON") from error
    require(isinstance(document, dict) and set(document) == {
        "schemaVersion", "mediaType", "artifactType", "config", "layers", "annotations"
    } and type(document["schemaVersion"]) is int and document["schemaVersion"] == 2
            and document["mediaType"] == MANIFEST_MEDIA_TYPE
            and document["artifactType"] == ARTIFACT_TYPE,
            "expected a single approved native-core artifact manifest")
    metadata = document["annotations"]
    require(isinstance(metadata, dict) and set(metadata) == {"org.opencontainers.image.created"},
            "unexpected dependency manifest annotations")
    created = metadata["org.opencontainers.image.created"]
    match = re.fullmatch(r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(?:\.\d{1,9})?Z", created) if isinstance(created, str) else None
    require(match is not None, "invalid OCI creation timestamp")
    try:
        datetime.datetime.strptime(match.group(1), "%Y-%m-%dT%H:%M:%S")
    except ValueError as error:
        raise ArtifactError("invalid OCI creation timestamp") from error
    config = document["config"]
    require(isinstance(config, dict) and set(config) == {"mediaType", "digest", "size", "data"}
            and config["mediaType"] == EMPTY_CONFIG_MEDIA_TYPE
            and config["digest"] == EMPTY_CONFIG_DIGEST
            and type(config["size"]) is int and config["size"] == 2
            and config["data"] == "e30=",
            "dependency artifact must use the empty OCI config without a platform")
    layers = document["layers"]
    require(isinstance(layers, list) and len(layers) == 1
            and isinstance(layers[0], dict), "dependency artifact requires exactly one archive blob")
    layer = layers[0]
    require(set(layer) == {"mediaType", "digest", "size", "annotations"}
            and layer["mediaType"] == CORE_LAYER_MEDIA_TYPE,
            "unexpected native dependency layer descriptor")
    annotations = layer["annotations"]
    require(isinstance(annotations, dict) and set(annotations) == {
        "org.opencontainers.image.title"
    }, "dependency layer requires one exact archive name")
    archive_name = validate_archive_name(expected_archive_name)
    require(annotations["org.opencontainers.image.title"] == archive_name,
            "dependency archive target substitution")
    blob_digest = require_digest(expected_blob_digest, "archive digest")
    require(layer["digest"] == blob_digest, "dependency blob digest mismatch")
    require(type(layer["size"]) is int and type(expected_blob_size) is int
            and 0 < layer["size"] <= MAX_CORE_BYTES
            and layer["size"] == expected_blob_size,
            "dependency blob size mismatch")
    return {
        "manifest_digest": expected_manifest_digest,
        "blob_digest": blob_digest,
        "blob_size": expected_blob_size,
        "archive_name": archive_name,
    }
