#!/usr/bin/env python3
"""Read-only, unpublished qualification of seven native-core CI candidates.

Inputs called `source`, `builders`, `artifacts`, and `trusted_environment` must
come from an independently trusted Actions API/workflow context, never from the
candidate download. This module neither fetches evidence nor signs, publishes,
or creates a production lock. Job success is accepted only from `builders`.
"""

from __future__ import annotations

import hashlib
import pathlib
import re
import tempfile

from ci_dependency_catalog import validate_catalog
from ci_dependency_artifact import MAX_CORE_BYTES
from ci_dependency_core import verify_core
from ci_dependency_identity import core_identity, policy_identity
from ci_dependency_lock import _source
from ci_environment_registry import RegistryError, _json

SHA256 = re.compile(r"[0-9a-f]{64}\Z")
ARTIFACT_ID = re.compile(r"[1-9][0-9]*\Z")
BUILDER_WORKFLOW = ".github/workflows/ci-build.yml"
BRANCH_PREFIX = "refs/heads/"
BUILDER_JOBS = {
    "adb-linux-x86_64": "BuildAdbLinuxX64",
    "adb-linux-arm64": "BuildAdbLinuxArm64",
    "adb-windows-x86_64": "BuildAdbWindowsX64",
    "adb-macos-x86_64": "BuildAdbMacX64",
    "adb-macos-arm64": "BuildAdbMacArm64",
    "ios-x86_64": "BuildIosNativeX64",
    "ios-arm64": "BuildIosNativeArm64",
}


class PipelineError(ValueError):
    """Trusted evidence or downloaded candidate fails the read-only gate."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise PipelineError(message)


def _digest(value: object) -> bool:
    return isinstance(value, str) and SHA256.fullmatch(value) is not None and value != "0" * 64


def _document(raw: object) -> dict:
    _require(isinstance(raw, bytes), "missing JSON bytes")
    _require(0 < len(raw) <= 1024 * 1024, "missing or oversized candidate JSON")
    try:
        document = _json(raw)
    except (ValueError, UnicodeError, RegistryError) as error:
        raise PipelineError("invalid or duplicate-key JSON") from error
    _require(isinstance(document, dict), "expected JSON object")
    return document


def compute_identities(repo_root: pathlib.Path, catalog: dict) -> dict[str, dict[str, str]]:
    """Recompute keys from checked-out recipes and source locks, not candidate JSON."""
    targets = validate_catalog(catalog)
    root = pathlib.Path(repo_root)
    locks = {
        "adb-helper": _document((root / "packaging/adb/adb-helper.lock.json").read_bytes()),
        "ios-native": _document((root / "3rdparty/libimobiledevice/libimobiledevice.lock.json").read_bytes()),
    }
    return {target_id: {
        "core_identity": core_identity(row["component"], row["target"],
                                       locks[row["component"]], root),
        "policy_identity": policy_identity(row["component"], root,
                                           lock=locks[row["component"]]),
    } for target_id, row in sorted(targets.items())}


def validate_evidence(source: dict, catalog: dict, catalog_bytes: bytes,
                      builders: list, artifacts: list, downloads: dict,
                      expected_identities: dict, *, trusted_environment: dict | None = None) -> dict:
    """Pure structural/byte validation; no filesystem, registry, or subprocess I/O.

    `builders` is the authoritative (not self-reported) set of job conclusions
    and outputs. `artifacts` is the independently fetched Actions metadata;
    `downloads` maps the numeric immutable artifact ID to raw candidate/archive
    bytes. The caller must authenticate all three before invoking this gate.
    """
    try:
        targets = validate_catalog(catalog)
        _require(isinstance(source, dict) and source.get("event_name") == "workflow_dispatch"
                 and set(source) == {"event_name", "repository", "workflow", "sha",
                                     "ref", "run_id", "run_attempt"},
                 "expected trusted workflow_dispatch source context")
        _source({key: value for key, value in source.items() if key != "event_name"},
                BUILDER_WORKFLOW)
    except ValueError as error:
        raise PipelineError("invalid trusted source or target catalog") from error
    _require(isinstance(catalog_bytes, bytes) and _document(catalog_bytes) == catalog,
             "catalog bytes differ from reviewed target policy")
    _require(set(targets) == set(BUILDER_JOBS), "unreviewed builder job mapping")
    _require(isinstance(expected_identities, dict) and set(expected_identities) == set(targets),
             "missing recomputed dependency identities")
    _require(isinstance(builders, list) and len(builders) == len(targets),
             "expected exactly seven authoritative builder jobs")
    _require(isinstance(artifacts, list) and len(artifacts) == len(targets),
             "expected exactly seven Actions artifacts")
    _require(isinstance(downloads, dict) and len(downloads) == len(targets),
             "expected exactly seven artifact downloads")
    _require(isinstance(trusted_environment, dict) and set(trusted_environment) == set(targets),
             "missing independent runner/toolchain evidence for seven targets")
    by_job = {}
    ids = set()
    for job in builders:
        _require(isinstance(job, dict) and set(job) == {"job_id", "conclusion", "outputs"},
                 "invalid authoritative builder result")
        name = job["job_id"]
        _require(isinstance(name, str) and name in BUILDER_JOBS.values()
                 and name not in by_job,
                 "missing, duplicate or unexpected builder job")
        _require(job["conclusion"] == "success", "builder did not succeed")
        output = job["outputs"]
        _require(isinstance(output, dict) and set(output) == {
            "artifact_id", "candidate_sha256", "archive_sha256"},
            "invalid immutable builder outputs")
        identifier = output["artifact_id"]
        _require(isinstance(identifier, str) and ARTIFACT_ID.fullmatch(identifier) is not None
                 and int(identifier) not in ids and
                 all(_digest(output[field]) for field in ("candidate_sha256", "archive_sha256")),
                 "duplicate or malformed authoritative artifact output")
        ids.add(int(identifier))
        by_job[name] = output
    _require(set(by_job) == set(BUILDER_JOBS.values()),
             "missing authoritative builder job")
    by_artifact = {}
    for artifact in artifacts:
        _require(isinstance(artifact, dict) and {
            "id", "name", "expired", "workflow_run"}.issubset(artifact),
            "invalid Actions artifact metadata")
        identifier = artifact["id"]
        _require(type(identifier) is int and identifier in ids and identifier not in by_artifact
                 and artifact["expired"] is False,
                 "missing, duplicated or expired immutable Actions artifact")
        ancestry = artifact["workflow_run"]
        _require(isinstance(ancestry, dict) and ancestry.get("id") == source["run_id"]
                 and type(ancestry.get("id")) is int
                 and ancestry.get("head_sha") == source["sha"]
                 and ancestry.get("head_branch") == source["ref"][len(BRANCH_PREFIX):],
                 "artifact run, commit or branch ancestry mismatch")
        by_artifact[identifier] = artifact
    _require(set(by_artifact) == ids and set(downloads) == ids,
             "missing or unexpected artifact identity/download")

    receipts = {}
    for target_id, row in sorted(targets.items()):
        output = by_job[BUILDER_JOBS[target_id]]
        identifier = int(output["artifact_id"])
        artifact = by_artifact[identifier]
        _require(artifact["name"] == (
            f"native-core-candidate-{target_id}-{source['run_id']}-{source['run_attempt']}"),
            "artifact name must bind target, run and attempt")
        observed = trusted_environment[target_id]
        expected = {"runner": row["runner"]}
        if "toolchain" in row:
            expected["toolchain"] = row["toolchain"]
        _require(isinstance(observed, dict) and observed == expected,
                 "trusted runner or toolchain differs from reviewed target")
        download = downloads[identifier]
        _require(isinstance(download, dict) and set(download) == {"candidate", "archive"}
                 and isinstance(download["candidate"], bytes)
                 and isinstance(download["archive"], bytes),
                 "missing candidate JSON or archive bytes")
        candidate_raw, archive_raw = download["candidate"], download["archive"]
        _require(0 < len(archive_raw) <= MAX_CORE_BYTES, "missing or oversized core archive")
        _require(sha(candidate_raw) == output["candidate_sha256"]
                 and sha(archive_raw) == output["archive_sha256"],
                 "download differs from authoritative builder SHA-256 outputs")
        candidate = _document(candidate_raw)
        fields = {"schema_version", "kind", "publication_status", "catalog_target",
                  "catalog_sha256", "component", "target", "runner",
                  "core_identity", "policy_identity", "archive"}
        if row["component"] == "adb-helper":
            fields.add("qualification")
        keys = expected_identities[target_id]
        _require(isinstance(keys, dict) and set(keys) == {"core_identity", "policy_identity"}
                 and all(_digest(value) for value in keys.values()),
                 "invalid recomputed dependency identities")
        _require(set(candidate) == fields and type(candidate["schema_version"]) is int
                 and candidate["schema_version"] == 1
                 and candidate["kind"] == "native-dependency-core-candidate"
                 and candidate["publication_status"] == "candidate-only"
                 and candidate["catalog_target"] == target_id
                 and candidate["catalog_sha256"] == sha(catalog_bytes)
                 and all(candidate[key] == row[key] for key in ("component", "target", "runner"))
                 and all(candidate[key] == value for key, value in keys.items()),
                 "stale, substituted or unreviewed native-core candidate")
        archive = candidate["archive"]
        _require(isinstance(archive, dict) and set(archive) == {"name", "sha256", "size"}
                 and archive["name"] == row["archive_name"]
                 and archive["sha256"] == output["archive_sha256"]
                 and type(archive["size"]) is int and archive["size"] == len(archive_raw),
                 "candidate archive descriptor differs from builder or catalog")
        if row["component"] == "adb-helper":
            qualification = candidate["qualification"]
            _require(isinstance(qualification, dict) and set(qualification) == {
                "legacy_package_receipt_sha256", "rechecked_package_receipt_sha256"}
                and all(_digest(value) for value in qualification.values()),
                "missing ADB offline qualification reference")
        receipts[target_id] = {
            "schema_version": 1, "kind": "native-dependency-candidate-qualification",
            "publication_status": "candidate-only", "source": dict(source),
            "catalog_target": target_id, "artifact_id": identifier,
            "candidate_sha256": output["candidate_sha256"],
            "core_identity": keys["core_identity"], "policy_identity": keys["policy_identity"],
            "archive": dict(archive),
        }
    return receipts


def sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def qualify_from_repo(repo_root: pathlib.Path, source: dict, builders: list,
                      artifacts: list, downloads: dict, *,
                      trusted_environment: dict | None = None) -> dict:
    """Read the current checkout's reviewed catalog/locks and qualify candidates.

    The caller must independently establish that this checkout is the trusted
    `source['sha']` and that builders/artifacts came from that workflow run.
    """
    root = pathlib.Path(repo_root)
    raw = (root / "ci/dependencies/catalog.json").read_bytes()
    catalog = _document(raw)
    identities = compute_identities(root, catalog)
    return qualify_candidates(source, catalog, raw, builders, artifacts, downloads,
                              identities, trusted_environment=trusted_environment)


def qualify_candidates(source: dict, catalog: dict, catalog_bytes: bytes,
                       builders: list, artifacts: list, downloads: dict,
                       expected_identities: dict, *, trusted_environment: dict | None = None) -> dict:
    """Validate everything first, then verify all seven archives via verify_core."""
    receipts = validate_evidence(source, catalog, catalog_bytes, builders, artifacts,
                                 downloads, expected_identities,
                                 trusted_environment=trusted_environment)
    with tempfile.TemporaryDirectory(prefix="native-core-qualification-") as temporary:
        for target_id, receipt in receipts.items():
            row = catalog["targets"][target_id]
            archive = receipt["archive"]
            path = pathlib.Path(temporary) / archive["name"]
            path.write_bytes(downloads[receipt["artifact_id"]]["archive"])
            try:
                verify_core(path, component=row["component"], target=row["target"],
                            core_identity=receipt["core_identity"],
                            expected_sha256=archive["sha256"], expected_size=archive["size"])
            except (OSError, ValueError) as error:
                raise PipelineError(f"invalid {target_id} binary-only core") from error
    return receipts
