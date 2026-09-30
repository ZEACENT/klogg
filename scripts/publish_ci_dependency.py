#!/usr/bin/env python3
"""Publish seven independently qualified, original binary-core archives.

This is a protected-workflow library, not a standalone credentialed CLI. The
mandatory verify_gate callback must independently authenticate the successful
same-run gate job, immutable Actions artifact ID/SHA, and signed full-build,
legal/source material for each target before returning its verified gate JSON.
Neither candidate JSON nor a caller-supplied bool can authorize publication.
Until that workflow and its signed gate-artifact contract exist, no production
caller can provide this callback. Detached Sigstore signing and lock activation
belong to later protected steps; this module returns actual raw manifest bytes.
"""

from __future__ import annotations

import datetime
import gzip
import hashlib
import io
import json
import os
import pathlib
import re
import stat
import subprocess
import tarfile
import tempfile

from ci_dependency_artifact import ARTIFACT_TYPE, CORE_LAYER_MEDIA_TYPE, inspect_manifest
from ci_dependency_catalog import TARGETS, validate_catalog
from ci_dependency_pipeline import qualify_from_repo
from ci_environment_registry import DEPENDENCY_REGISTRY, _json
from prefetch_adb_helper_sources import download, safe_extract

CHUNK = 1024 * 1024
MATERIAL_LIMIT = 10 * 1024 * 1024
GATE_LIMIT = 1024 * 1024
SHA = re.compile(r"[0-9a-f]{64}\Z")
ACTOR = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]*(?:\[bot\])?\Z")


class PublicationError(ValueError):
    """A native dependency publication cannot be authorized or verified."""


def _require(condition, message):
    if not condition:
        raise PublicationError(message)


def _sha(raw):
    return hashlib.sha256(raw).hexdigest()


def _file_sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tool_pin(repo_root):
    """Only the checked-in SHA-256 pinned official ORAS 1.3.4 Linux archive."""
    try:
        raw = (pathlib.Path(repo_root) / "ci/environments/publisher-tools.json").read_bytes()
        lock = _json(raw)
    except (OSError, RuntimeError, ValueError):
        raise PublicationError("invalid ORAS tool lock") from None
    _require(isinstance(lock, dict) and set(lock) == {"schema_version", "oras"}
             and type(lock["schema_version"]) is int and lock["schema_version"] == 1,
             "invalid ORAS tool lock")
    pin = lock["oras"]
    _require(isinstance(pin, dict) and set(pin) == {"version", "url", "sha256", "executable"}
             and pin["version"] == "1.3.4"
             and pin["url"] == "https://github.com/oras-project/oras/releases/download/v1.3.4/oras_1.3.4_linux_amd64.tar.gz"
             and pin["executable"] == "oras"
             and isinstance(pin["sha256"], str) and SHA.fullmatch(pin["sha256"]) is not None
             and pin["sha256"] != "0" * 64, "unreviewed ORAS tool pin")
    return pin


def _acquire_oras(directory, pin, downloader=None):
    archive = directory / "oras.tar.gz"
    try:
        (downloader or download)(pin["url"], archive)
        _require(archive.is_file() and not archive.is_symlink()
                 and archive.stat().st_size <= 128 * 1024 * 1024
                 and _file_sha(archive) == pin["sha256"], "pinned ORAS archive checksum mismatch")
        tool_root = directory / "tool"
        safe_extract(archive, tool_root)
        executable = tool_root / pin["executable"]
        info = executable.lstat()
        _require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1
                 and info.st_size <= 128 * 1024 * 1024
                 and os.access(executable, os.X_OK), "unsafe pinned ORAS executable")
        return str(executable)
    except (OSError, RuntimeError, tarfile.TarError, ValueError):
        raise PublicationError("cannot acquire SHA-verified pinned ORAS") from None


def _run_oras(argv, runner, *, stdin=None, cwd=None):
    try:
        result = (runner or subprocess.run)(argv, input=stdin, check=True,
                                             text=True, capture_output=True,
                                             timeout=1800, cwd=cwd)
        _require(result.returncode == 0, "pinned ORAS command failed")
    except (OSError, subprocess.SubprocessError, ValueError, TypeError, AttributeError):
        # Never display subprocess stderr, arguments or a registry credential.
        raise PublicationError("pinned ORAS command failed") from None


def _gate_document(archive):
    """Read exact qualified.json and seven compact materials from a locked tar.gz."""
    try:
        with gzip.GzipFile(fileobj=io.BytesIO(archive)) as compressed:
            expanded = compressed.read(GATE_LIMIT + 1)
        _require(len(expanded) <= GATE_LIMIT, "oversized qualification gate archive")
        with tarfile.open(fileobj=io.BytesIO(expanded), mode="r:") as tar:
            members = tar.getmembers()
            material_names = {f"materials/{target}.json" for target in TARGETS}
            _require(len(members) == len(TARGETS) + 1
                     and [member.name for member in members]
                     == ["qualified.json", *sorted(material_names)]
                     and all(member.isfile() and 0 < member.size <= GATE_LIMIT
                             for member in members),
                     "qualification artifact needs exact regular receipt and materials")
            content = {}
            for member in members:
                source = tar.extractfile(member)
                _require(source is not None, "unreadable qualification member")
                with source:
                    raw = source.read(member.size + 1)
                _require(len(raw) == member.size, "truncated qualification member")
                content[member.name] = raw
        return _json(content["qualified.json"]), {
            target: content[f"materials/{target}.json"] for target in TARGETS
        }
    except (OSError, EOFError, ValueError, RuntimeError, tarfile.TarError):
        raise PublicationError("invalid qualification gate artifact") from None


def _check_material(raw, target, source, receipt, environment):
    try:
        record = _json(raw)
        canonical = (json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n").encode()
    except (ValueError, RuntimeError, TypeError, UnicodeError) as error:
        raise PublicationError("invalid qualification material JSON") from error
    fields = {"schema_version", "kind", "target", "source", "full_artifact_id",
              "full_tar_sha256", "full_tar_bundle_sha256", "candidate_artifact_id",
              "core_sha256", "trusted_environment"}
    if target.startswith("adb-"):
        fields.add("legal")
    _require(isinstance(record, dict) and set(record) == fields and raw == canonical
             and type(record["schema_version"]) is int and record["schema_version"] == 1
             and record["kind"] == "native-dependency-signed-material"
             and record["target"] == target and record["source"] == source
             and type(record["full_artifact_id"]) is int and record["full_artifact_id"] > 0
             and type(record["candidate_artifact_id"]) is int
             and record["candidate_artifact_id"] == receipt["artifact_id"]
             and record["core_sha256"] == receipt["archive"]["sha256"]
             and record["trusted_environment"] == environment,
             "qualification material is not bound to reviewed source and candidate")
    for field in ("full_tar_sha256", "full_tar_bundle_sha256", "core_sha256"):
        _require(isinstance(record[field], str) and SHA.fullmatch(record[field]) is not None
                 and record[field] != "0" * 64, "invalid qualification material digest")
    if target.startswith("adb-"):
        legal = record["legal"]
        _require(isinstance(legal, dict) and set(legal) == {
            "support_artifact_id", "support_zip_sha256", "full_release_artifact_id",
            "full_release_zip_sha256", "source_set_receipt_sha256",
            "overlay_receipt_sha256", "source_archive_sha256"},
            "missing authenticated ADB legal material")
        for field in ("support_artifact_id", "full_release_artifact_id"):
            _require(type(legal[field]) is int and legal[field] > 0,
                     "invalid ADB legal artifact identity")
        for field in ("support_zip_sha256", "full_release_zip_sha256",
                      "source_set_receipt_sha256", "overlay_receipt_sha256",
                      "source_archive_sha256"):
            _require(isinstance(legal[field], str) and SHA.fullmatch(legal[field]) is not None
                     and legal[field] != "0" * 64, "invalid ADB legal material digest")
    return record


def _qualified(repo_root, source, gate_artifact_id, gate_archive_sha256,
               gate_bytes, verify_gate, signed_material, builders, artifacts,
               downloads, trusted_environment):
    _require(callable(verify_gate), "external successful signed qualification gate is required")
    _require(type(gate_artifact_id) is int and gate_artifact_id > 0
             and isinstance(gate_archive_sha256, str)
             and SHA.fullmatch(gate_archive_sha256) is not None
             and gate_archive_sha256 != "0" * 64
             and isinstance(gate_bytes, bytes) and 0 < len(gate_bytes) <= GATE_LIMIT
             and _sha(gate_bytes) == gate_archive_sha256,
             "invalid externally pinned qualification gate artifact")
    try:
        # This callback must authenticate the independent Actions job/artifact
        # and verify the signed source/legal/full-build evidence, not parse only
        # self-reported candidate contents. It must return that gate's document.
        trusted = verify_gate(gate_artifact_id, gate_archive_sha256, source,
                              gate_bytes, signed_material)
        gate, archived_material = _gate_document(gate_bytes)
        _require(isinstance(trusted, dict) and trusted == gate,
                 "qualification gate was not independently authenticated")
        receipts = qualify_from_repo(repo_root, source, builders, artifacts, downloads,
                                     trusted_environment=trusted_environment)
    except (OSError, RuntimeError, ValueError, TypeError, KeyError):
        raise PublicationError("trusted qualification or current source policy failed") from None
    _require(isinstance(gate, dict) and set(gate) == {
        "schema_version", "kind", "result", "source", "targets"}
             and type(gate["schema_version"]) is int and gate["schema_version"] == 1
             and gate["kind"] == "native-dependency-qualified-run"
             and gate["result"] == "success" and gate["source"] == source
             and isinstance(gate["targets"], dict)
             and set(gate["targets"]) == set(receipts)
             and isinstance(signed_material, dict)
             and set(signed_material) == set(receipts),
             "incomplete, failed or cross-attempt qualification gate")
    full_ids = set()
    for target, receipt in receipts.items():
        entry = gate["targets"][target]
        material = signed_material[target]
        _require(isinstance(entry, dict) and set(entry) == {
            "receipt", "signed_material_sha256"}
                 and entry["receipt"] == receipt
                 and isinstance(material, bytes) and 0 < len(material) <= MATERIAL_LIMIT
                 and archived_material[target] == material
                 and entry["signed_material_sha256"] == _sha(material),
                 "gate receipt or signed source/policy material differs from qualified target")
        record = _check_material(material, target, source, receipt, trusted_environment[target])
        _require(record["full_artifact_id"] not in full_ids,
                 "original full artifact reused across native targets")
        full_ids.add(record["full_artifact_id"])
    return receipts


def publish(*, repo_root, source, gate_artifact_id, gate_archive_sha256,
            gate_bytes, verify_gate, signed_material, builders, artifacts,
            downloads, trusted_environment, runner=None, downloader=None,
            registry_client=None, environment=None):
    """Return prospective target records including unmodified remote manifest bytes.

    The required verifier owns the not-yet-established signed gate artifact
    contract and independent GitHub Actions API authentication. No credential,
    ORAS tool acquisition, or registry write occurs before *all seven* inputs,
    archive contents, provenance, current core/policy identities, and material
    hashes have been checked. The anonymous registry client must expose _read
    and retrieve; private publications fail closed until publicly retrievable.
    """
    receipts = _qualified(repo_root, source, gate_artifact_id,
                          gate_archive_sha256, gate_bytes, verify_gate,
                          signed_material, builders, artifacts, downloads,
                          trusted_environment)
    try:
        catalog = _json((pathlib.Path(repo_root) / "ci/dependencies/catalog.json").read_bytes())
        targets = validate_catalog(catalog)
    except (OSError, RuntimeError, ValueError):
        raise PublicationError("invalid checked-out native target catalog") from None
    # All core bytes are already independently checked by qualify_from_repo.
    archives = {target: downloads[receipt["artifact_id"]]["archive"]
                for target, receipt in receipts.items()}
    pin = tool_pin(repo_root)
    env = os.environ if environment is None else environment
    token, actor = env.get("GH_TOKEN"), env.get("GITHUB_ACTOR")
    _require(isinstance(token, str) and bool(token)
             and not any(char.isspace() or ord(char) < 33 for char in token),
             "registry write credentials are required")
    _require(isinstance(actor, str) and ACTOR.fullmatch(actor) is not None,
             "valid registry actor is required")
    # Deliberately create the anonymous client without a write credential.
    if registry_client is None:
        from ci_dependency_registry import DependencyRegistryClient
        registry_client = DependencyRegistryClient()
    result = {"schema_version": 1, "kind": "native-dependency-publication",
              "source": source, "qualified_artifact_id": gate_artifact_id,
              "qualified_archive_sha256": gate_archive_sha256, "targets": {}}
    with tempfile.TemporaryDirectory(prefix="native-dependency-publication-") as temporary:
        root = pathlib.Path(temporary).resolve()
        executable = _acquire_oras(root, pin, downloader)
        auth = root / "registry-config.json"
        with os.fdopen(os.open(auth, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as stream:
            stream.write("{}")
        _run_oras([executable, "login", "ghcr.io", "--username", actor,
                   "--password-stdin", "--registry-config", str(auth)], runner,
                  stdin=token + "\n")
        for target, row in sorted(targets.items()):
            receipt = receipts[target]
            archive = archives[target]
            name = row["archive_name"]
            _require(len(archive) == receipt["archive"]["size"]
                     and _sha(archive) == receipt["archive"]["sha256"],
                     "qualified archive changed before registry push")
            source_path = root / name
            with source_path.open("xb") as stream:
                stream.write(archive)
            # Give ORAS a single *existing* opaque archive file. ORAS creates
            # the OCI envelope; there is no Linux platform or archive rebuild.
            tag = target + "-" + receipt["archive"]["sha256"]
            reference = DEPENDENCY_REGISTRY + ":" + tag
            created = ("org.opencontainers.image.created="
                       + datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"))
            _run_oras([executable, "push", "--artifact-type", ARTIFACT_TYPE,
                       "--annotation", created, "--registry-config", str(auth),
                       reference, name + ":" + CORE_LAYER_MEDIA_TYPE], runner, cwd=root)
            _require(_file_sha(source_path) == receipt["archive"]["sha256"],
                     "qualified archive changed during registry push")
            # Fetch anonymously by tag only to learn the *actual* remote raw
            # manifest digest. The digest/descriptor and public blob are then
            # independently fetched and validated by immutable digest.
            url = "https://ghcr.io/v2/" + DEPENDENCY_REGISTRY.partition("/")[2] + "/manifests/" + tag
            try:
                manifest = registry_client._read(url)
                manifest_digest = "sha256:" + _sha(manifest)
                identity = inspect_manifest(
                    manifest, manifest_digest,
                    expected_blob_digest="sha256:" + receipt["archive"]["sha256"],
                    expected_blob_size=receipt["archive"]["size"],
                    expected_archive_name=name)
                destination = root / (target + ".remote.tar.gz")
                observed = registry_client.retrieve(
                    manifest_digest, identity["blob_digest"], identity["blob_size"],
                    name, destination)
                _require(observed["manifest_bytes"] == manifest
                         and _file_sha(destination) == receipt["archive"]["sha256"]
                         and destination.stat().st_size == receipt["archive"]["size"],
                         "anonymous registry bytes differ from qualified archive")
            except (OSError, RuntimeError, ValueError, KeyError, TypeError):
                raise PublicationError("anonymous registry manifest or blob verification failed") from None
            result["targets"][target] = {
                **identity, "manifest_bytes": manifest,
                "core_identity": receipt["core_identity"],
                "policy_identity": receipt["policy_identity"],
                "qualification_receipt": receipt,
            }
            source_path.unlink()
    return result
