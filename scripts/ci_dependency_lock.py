#!/usr/bin/env python3
"""Structural contract for seven published native dependency core locks.

This does not authenticate the lock's evidence. Consumers must separately
verify both detached Sigstore bundles, the exact registry manifest/blob bytes,
and the qualification receipt before using a core.
"""

from __future__ import annotations

import re

from ci_dependency_catalog import validate_catalog
from ci_environment_registry import DEPENDENCY_REGISTRY, REPOSITORY

DIGEST = re.compile(r"sha256:[0-9a-f]{64}\Z")
CORE_KEY = re.compile(r"[0-9a-f]{64}\Z")
COMMIT = re.compile(r"[0-9a-f]{40}\Z")
BRANCH = re.compile(r"refs/heads/[A-Za-z0-9][A-Za-z0-9._/-]*\Z")


class DependencyLockError(ValueError):
    """A production dependency lock is missing or has unreviewed identities."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise DependencyLockError(message)


def _digest(value: object, label: str, *, prefix: bool = True) -> None:
    pattern = DIGEST if prefix else CORE_KEY
    _require(isinstance(value, str) and pattern.fullmatch(value) is not None
             and value[-64:] != "0" * 64, f"invalid locked {label}")


def _source(document: object, producer_workflow: str) -> None:
    _require(isinstance(document, dict) and set(document) == {
        "repository", "workflow", "sha", "ref", "run_id", "run_attempt"
    }, "invalid dependency source identity")
    sha, ref = document["sha"], document["ref"]
    _require(document["repository"] == REPOSITORY
             and document["workflow"] == producer_workflow
             and isinstance(sha, str) and COMMIT.fullmatch(sha) is not None
             and sha != "0" * 40
             and isinstance(ref, str) and BRANCH.fullmatch(ref) is not None
             and ".." not in ref and "//" not in ref and not ref.endswith(("/", "."))
             and type(document["run_id"]) is int and document["run_id"] > 0
             and type(document["run_attempt"]) is int and document["run_attempt"] > 0,
             "untrusted dependency producer source")


def _evidence(document: object, target: str) -> None:
    _require(isinstance(document, dict) and set(document) == {
        "digest", "receipt", "manifest_bundle", "receipt_bundle"
    }, "invalid dependency qualification evidence")
    _digest(document["digest"], "qualification digest")
    base = "ci/dependencies/evidence/" + target + "/"
    expected = {
        "receipt": "dependency-verification.json",
        "manifest_bundle": "manifest.sigstore.json",
        "receipt_bundle": "receipt.sigstore.json",
    }
    for field, filename in expected.items():
        _require(document[field] == base + filename,
                 f"dependency evidence path does not belong to {target}")


def validate_production_lock(document: object, catalog: object) -> dict:
    """Validate exact metadata only; a signed, fetched receipt is still mandatory."""
    try:
        targets = validate_catalog(catalog)
    except ValueError as error:
        raise DependencyLockError("invalid dependency catalog") from error
    _require(isinstance(document, dict) and set(document) == {
        "schema_version", "kind", "registry", "targets"
    } and type(document["schema_version"]) is int
             and document["schema_version"] == 1
             and document["kind"] == "production-lock"
             and document["registry"] == DEPENDENCY_REGISTRY,
             "invalid dependency production lock identity")
    records = document["targets"]
    _require(isinstance(records, dict) and set(records) == set(targets),
             "production lock must cover exactly seven approved targets")
    for target_id, record in records.items():
        _require(isinstance(record, dict) and set(record) == {
            "manifest_digest", "blob_digest", "blob_size", "archive_name",
            "core_identity", "policy_identity", "source", "qualification"
        }, f"invalid dependency target lock: {target_id}")
        for field in ("manifest_digest", "blob_digest"):
            _digest(record[field], field)
        for field in ("core_identity", "policy_identity"):
            _digest(record[field], field, prefix=False)
        _require(type(record["blob_size"]) is int and 0 < record["blob_size"] <= 2 * 1024**3
                 and record["archive_name"] == targets[target_id]["archive_name"],
                 f"invalid dependency blob descriptor: {target_id}")
        _source(record["source"], catalog["producer_workflow"])
        _evidence(record["qualification"], target_id)
    for field in ("manifest_digest", "blob_digest", "core_identity"):
        _require(len({record[field] for record in records.values()}) == len(records),
                 f"dependency {field} was copied across targets")
    _require(len({record["qualification"]["digest"] for record in records.values()})
             == len(records), "dependency qualification was copied across targets")
    return records
