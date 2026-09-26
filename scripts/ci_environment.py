#!/usr/bin/env python3
"""Schema-v1 offline CI environment contracts, not provenance verification.

The OCI inspector accepts an uncompressed, single-image OCI-layout USTAR archive
with linux/amd64 OCI manifests and plain/gzip/zstd layer media types. It never
extracts or runs an image. Layers are hashed as opaque stored bytes: diffIDs are
bound through the config, not independently recomputed by decompressing layers.
The caller MUST compare the Docker-loaded image config and diffIDs before tests.
PAX/GNU extensions, Docker-save archives and compressed outer tars are unsupported.

Archive bytes, not a later Docker export, are publication authority. Candidate
artifact IDs come from upload outputs and are deliberately absent from candidate
documents. Qualification inputs (job results and artifact ID) must be obtained
from a trusted external authority; matching self-reported JSON is not provenance.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
import tarfile

SCHEMA_VERSION = 1
REGISTRY = "ghcr.io/zeacent/klogg-ci-env"
PLATFORM = "linux/amd64"
OCI = "application/vnd.oci.image."
MAX_METADATA_BYTES = 4 * 1024 * 1024
MAX_ARCHIVE_MEMBERS = 10000
CHUNK_BYTES = 1024 * 1024


class ContractError(ValueError):
    """An environment document or archive violates the offline contract."""


def _require(condition, message):
    if not condition:
        raise ContractError(message)


def _object(value, required, label, optional=()):
    _require(isinstance(value, dict), label + " must be an object")
    _require(set(required) <= set(value) <= set(required) | set(optional),
             label + " has missing or unknown fields")
    return value


def _version(value, label):
    _require(type(value.get("schema_version")) is int and value["schema_version"] == SCHEMA_VERSION,
             label + " requires schema_version 1")


def _positive_int(value, label):
    _require(type(value) is int and value > 0, label + " must be a positive integer")


def _digest(value, label, prefix=True):
    pattern = r"sha256:[0-9a-f]{64}" if prefix else r"[0-9a-f]{64}"
    _require(isinstance(value, str) and re.fullmatch(pattern, value) is not None,
             label + " must be a lowercase SHA-256 digest")
    _require(value[-64:] != "0" * 64, label + " must not be a zero placeholder")
    return value


def _identifier(value, label):
    _require(isinstance(value, str) and re.fullmatch(r"[a-z0-9]+(?:[._-][a-z0-9]+)*", value) is not None,
             label + " must be a lowercase identifier")


def _relative_path(value, label, allow_dot=False):
    _require(isinstance(value, str) and bool(value), label + " must be a relative path")
    if allow_dot and value == ".":
        return value
    _require(not value.startswith("/") and "\\" not in value and ":" not in value
             and all(part not in ("", ".", "..") for part in value.split("/"))
             and not any(ord(char) < 32 or ord(char) == 127 for char in value),
             label + " must be a canonical safe relative path")
    return value


def _unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        _require(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def _invalid_constant(value):
    raise ContractError("nonfinite JSON value: " + value)


def _parse_json(data, label):
    try:
        result = json.loads(data.decode("utf-8"), object_pairs_hook=_unique_pairs,
                            parse_constant=_invalid_constant)
        # Also reject overflowing exponents parsed by Python as infinity.
        canonical_digest(result)
        return result
    except (UnicodeError, ValueError, RecursionError) as error:
        raise ContractError(label + ": " + str(error)) from error


def load_json(path):
    """Read bounded UTF-8 JSON, rejecting duplicate keys and nonfinite numbers."""
    with pathlib.Path(path).open("rb") as stream:
        data = stream.read(MAX_METADATA_BYTES + 1)
    _require(len(data) <= MAX_METADATA_BYTES, "JSON document exceeds metadata size limit")
    return _parse_json(data, str(path))


def canonical_digest(document):
    """Hash sorted, compact, ASCII-escaped JSON (not an RFC 8785 implementation)."""
    def check(value):
        if isinstance(value, dict):
            _require(all(isinstance(key, str) for key in value), "JSON object keys must be strings")
            for item in value.values():
                check(item)
        elif isinstance(value, list):
            for item in value:
                check(item)
        else:
            _require(value is None or type(value) in (str, int, float, bool), "not a JSON value")

    try:
        check(document)
        data = json.dumps(document, sort_keys=True, separators=(",", ":"),
                          ensure_ascii=True, allow_nan=False).encode("ascii")
    except (TypeError, ValueError, RecursionError) as error:
        raise ContractError("cannot canonicalize JSON: " + str(error)) from error
    return "sha256:" + hashlib.sha256(data).hexdigest()


def _document_identity(document, kind):
    _require(isinstance(document, dict), kind + " must be an object")
    _version(document, kind)
    _require(len(document) > 1, kind + " must not be empty")
    return canonical_digest({"schema_version": 1, "kind": kind, "document": document})


def input_identity(resolved_manifest):
    """Hash caller-supplied resolved inputs; do not invent or resolve pins."""
    return _document_identity(resolved_manifest, "resolved-inputs")


def policy_identity(policy):
    """Hash qualification policy independently from the image build recipe."""
    return _document_identity(policy, "qualification-policy")


def validate_catalog(catalog):
    """Validate the explicit family catalog without reading recipe contents."""
    _object(catalog, {"schema_version", "registry", "families"}, "catalog")
    _version(catalog, "catalog")
    _require(catalog["registry"] == REGISTRY, "catalog registry is not the canonical package")
    _require(isinstance(catalog["families"], dict) and bool(catalog["families"]), "catalog families must be nonempty")
    for family, entry in catalog["families"].items():
        _identifier(family, "family")
        _object(entry, {"platform", "dockerfile", "context", "recipe_files", "profiles"},
                "catalog family", {"build_args"})
        _require(entry["platform"] == PLATFORM, "catalog platform must be linux/amd64")
        _relative_path(entry["dockerfile"], "dockerfile")
        _relative_path(entry["context"], "build context", allow_dot=True)
        for field in ("recipe_files", "profiles"):
            values = entry[field]
            _require(isinstance(values, list) and bool(values) and all(isinstance(value, str) for value in values),
                     "catalog " + field + " must be a nonempty string list")
            _require(len(values) == len(set(values)), "duplicate catalog " + field)
        for path in entry["recipe_files"]:
            _relative_path(path, "recipe file")
        _require(entry["dockerfile"] in entry["recipe_files"], "recipe files must include the Dockerfile")
        for profile in entry["profiles"]:
            _identifier(profile, "profile")
        if "build_args" in entry:
            _require(isinstance(entry["build_args"], dict)
                     and all(isinstance(key, str) and isinstance(value, str) for key, value in entry["build_args"].items()),
                     "build_args must map strings to strings")


def _regular_recipe_file(root, relative):
    path = root
    for part in relative.split("/"):
        path = path / part
        _require(not path.is_symlink(), "recipe path must not traverse a symlink: " + relative)
    _require(path.is_file(), "recipe file is missing or not regular: " + relative)
    return path


def recipe_identity(catalog, family, repo_root):
    """Hash only declared recipe files and build config; profiles are policy."""
    validate_catalog(catalog)
    _require(family in catalog["families"], "unknown environment family")
    entry = catalog["families"][family]
    root = pathlib.Path(repo_root).resolve()
    files = []
    for relative in sorted(entry["recipe_files"]):
        path = _regular_recipe_file(root, relative)
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(CHUNK_BYTES), b""):
                digest.update(chunk)
        files.append({"path": relative, "digest": "sha256:" + digest.hexdigest()})
    config = {key: entry[key] for key in ("platform", "dockerfile", "context")}
    config["build_args"] = entry.get("build_args", {})
    return canonical_digest({"schema_version": 1, "kind": "recipe", "family": family,
                             "build": config, "files": files})


def _scan_tar(stream):
    """Read raw USTAR headers before payloads, avoiding eager PAX expansion."""
    archive_hash = hashlib.sha256()
    members = {}
    names = set()

    def read(size):
        data = stream.read(size)
        archive_hash.update(data)
        return data

    while True:
        header = read(512)
        _require(len(header) == 512, "truncated archive header or missing end marker")
        if header == b"\0" * 512:
            _require(read(512) == b"\0" * 512, "archive requires two zero end blocks")
            while True:
                trailing = read(CHUNK_BYTES)
                if not trailing:
                    break
                _require(not any(trailing), "nonzero data after archive end marker")
            break
        _require(header[257:263] == b"ustar\0", "only uncompressed USTAR OCI archives are supported")
        try:
            member = tarfile.TarInfo.frombuf(header, "utf-8", "strict")
        except (tarfile.TarError, UnicodeError, ValueError) as error:
            raise ContractError("invalid tar header: " + str(error)) from error
        _require(member.type in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE),
                 "archive links, special members and PAX/GNU extensions are forbidden")
        name = member.name[:-1] if member.isdir() and member.name.endswith("/") else member.name
        _relative_path(name, "archive member")
        _require(name not in names, "duplicate archive member: " + name)
        names.add(name)
        _require(len(names) <= MAX_ARCHIVE_MEMBERS, "archive has too many members")
        _require(member.size >= 0, "negative tar member size")
        if member.isdir():
            _require(name in ("blobs", "blobs/sha256") and member.size == 0,
                     "unsupported OCI archive directory")
            continue
        is_blob = re.fullmatch(r"blobs/sha256/[0-9a-f]{64}", name) is not None
        _require(name in ("oci-layout", "index.json") or is_blob, "unsupported OCI archive member: " + name)
        if not is_blob:
            _require(member.size <= MAX_METADATA_BYTES, "archive metadata exceeds size limit")
        offset = stream.tell()
        remaining = member.size
        digest = hashlib.sha256()
        while remaining:
            chunk = read(min(remaining, CHUNK_BYTES))
            _require(bool(chunk), "truncated archive member: " + name)
            digest.update(chunk)
            remaining -= len(chunk)
        actual_digest = "sha256:" + digest.hexdigest()
        if is_blob:
            _require(actual_digest[7:] == name.split("/")[-1], "blob digest mismatch: " + name)
        padding_size = (-member.size) % 512
        padding = read(padding_size)
        _require(len(padding) == padding_size and not any(padding), "invalid or truncated tar padding")
        members[name] = {"offset": offset, "size": member.size, "digest": actual_digest}
    return members, archive_hash.hexdigest()


def inspect_oci_archive(path):
    """Inspect authoritative bytes; this does NOT verify decompressed diffIDs."""
    path = pathlib.Path(path)
    _require(not path.is_symlink() and path.is_file(), "archive must be a regular file, not a symlink")
    try:
        with path.open("rb") as stream:
            before = path.stat()
            members, archive_sha256 = _scan_tar(stream)
            used = {"oci-layout", "index.json"}

            def metadata(name):
                _require(name in members, "missing OCI member: " + name)
                record = members[name]
                _require(record["size"] <= MAX_METADATA_BYTES, "OCI metadata exceeds size limit")
                stream.seek(record["offset"])
                data = stream.read(record["size"])
                _require(len(data) == record["size"] and "sha256:" + hashlib.sha256(data).hexdigest() == record["digest"],
                         "OCI metadata changed during inspection")
                return _parse_json(data, name)

            def descriptor(value, media_types):
                _object(value, {"mediaType", "digest", "size"}, "OCI descriptor", {"annotations", "platform"})
                _require(isinstance(value["mediaType"], str) and value["mediaType"] in media_types,
                         "unsupported OCI descriptor media type")
                _digest(value["digest"], "OCI descriptor digest")
                _require(type(value["size"]) is int and value["size"] >= 0, "invalid OCI descriptor size")
                if "platform" in value:
                    platform = value["platform"]
                    _require(isinstance(platform, dict) and platform.get("os") == "linux"
                             and platform.get("architecture") == "amd64" and not platform.get("variant"),
                             "OCI descriptor platform must be linux/amd64")
                name = "blobs/sha256/" + value["digest"][7:]
                _require(name in members and members[name]["size"] == value["size"]
                         and members[name]["digest"] == value["digest"], "OCI descriptor size or digest mismatch")
                used.add(name)
                return name

            layout = metadata("oci-layout")
            _require(layout == {"imageLayoutVersion": "1.0.0"}, "unsupported OCI layout version")
            index = metadata("index.json")
            _object(index, {"schemaVersion", "manifests"}, "OCI index", {"mediaType", "annotations"})
            _require(type(index["schemaVersion"]) is int and index["schemaVersion"] == 2
                     and index.get("mediaType", OCI + "index.v1+json") == OCI + "index.v1+json", "unsupported OCI index")
            _require(isinstance(index["manifests"], list) and len(index["manifests"]) == 1,
                     "OCI archive must contain exactly one image manifest")
            manifest_descriptor = index["manifests"][0]
            manifest = metadata(descriptor(manifest_descriptor, {OCI + "manifest.v1+json"}))
            _object(manifest, {"schemaVersion", "mediaType", "config", "layers"}, "OCI manifest", {"annotations"})
            _require(type(manifest["schemaVersion"]) is int and manifest["schemaVersion"] == 2
                     and manifest["mediaType"] == OCI + "manifest.v1+json", "unsupported OCI image manifest")
            config = metadata(descriptor(manifest["config"], {OCI + "config.v1+json"}))
            _require(isinstance(config, dict) and config.get("os") == "linux" and config.get("architecture") == "amd64"
                     and not config.get("variant"), "OCI config platform must be linux/amd64")
            _require(isinstance(manifest["layers"], list), "OCI layers must be a list")
            rootfs = config.get("rootfs")
            _require(isinstance(rootfs, dict) and rootfs.get("type") == "layers"
                     and isinstance(rootfs.get("diff_ids"), list), "OCI rootfs requires layers and diff_ids")
            _require(len(rootfs["diff_ids"]) == len(manifest["layers"]), "OCI rootfs diff_ids count does not match layers")
            for value in rootfs["diff_ids"]:
                _digest(value, "OCI rootfs diffID")
            layer_types = {OCI + "layer.v1.tar", OCI + "layer.v1.tar+gzip", OCI + "layer.v1.tar+zstd"}
            for layer in manifest["layers"]:
                descriptor(layer, layer_types)
            _require(set(members) == used, "OCI archive contains unreferenced content")
            after = path.stat()
            _require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
                     == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), "archive changed during inspection")
            return {"schema_version": 1, "platform": PLATFORM, "archive_sha256": archive_sha256,
                    "manifest_digest": manifest_descriptor["digest"], "config_digest": manifest["config"]["digest"],
                    "diff_ids": rootfs["diff_ids"], "layer_digests": [layer["digest"] for layer in manifest["layers"]]}
    except (OSError, tarfile.TarError) as error:
        raise ContractError("cannot inspect OCI archive: " + str(error)) from error


def _validate_image(image):
    _object(image, {"schema_version", "platform", "archive_sha256", "manifest_digest", "config_digest", "diff_ids", "layer_digests"}, "image identity")
    _version(image, "image identity")
    _require(image["platform"] == PLATFORM, "image platform must be linux/amd64")
    _digest(image["archive_sha256"], "archive SHA-256", prefix=False)
    for key in ("manifest_digest", "config_digest"):
        _digest(image[key], key)
    for key in ("diff_ids", "layer_digests"):
        _require(isinstance(image[key], list), key + " must be a list")
        for value in image[key]:
            _digest(value, key)
    _require(len(image["diff_ids"]) == len(image["layer_digests"]), "image diffIDs and layer counts differ")


def compare_loaded_image(image_identity, docker_inspect):
    """Require the tested Docker image to match the authoritative OCI config."""
    _validate_image(image_identity)
    if isinstance(docker_inspect, list):
        _require(len(docker_inspect) == 1, "Docker inspect must contain exactly one image")
        docker_inspect = docker_inspect[0]
    _require(isinstance(docker_inspect, dict), "Docker inspect must describe an image")
    _require(docker_inspect.get("Id") == image_identity["config_digest"], "loaded image config digest mismatch")
    _require(docker_inspect.get("Architecture") == "amd64" and docker_inspect.get("Os") == "linux",
             "loaded image platform mismatch")
    rootfs = docker_inspect.get("RootFS")
    _require(isinstance(rootfs, dict) and rootfs.get("Type") == "layers"
             and rootfs.get("Layers", []) == image_identity["diff_ids"], "loaded image rootfs diffIDs mismatch")


def _validate_source(source):
    _object(source, {"repository", "sha", "ref", "workflow", "run_id", "run_attempt"}, "source identity")
    _require(source["repository"] == "ZEACENT/klogg", "source repository must be ZEACENT/klogg")
    _require(isinstance(source["sha"], str) and re.fullmatch(r"[0-9a-f]{40}", source["sha"]) is not None
             and source["sha"] != "0" * 40, "source SHA must be a full nonzero commit ID")
    ref = source["ref"]
    _require(isinstance(ref, str) and ref.startswith("refs/") and len(ref) > 5
             and not any(char.isspace() or char in "\\~^:?*[" for char in ref)
             and ".." not in ref and "//" not in ref and "@{" not in ref, "source ref must be a full Git ref")
    workflow = source["workflow"]
    _require(isinstance(workflow, str) and re.fullmatch(r"\.github/workflows/[A-Za-z0-9_-]+\.ya?ml", workflow) is not None,
             "source workflow must be a repository workflow path")
    _positive_int(source["run_id"], "source run_id")
    _positive_int(source["run_attempt"], "source run_attempt")


def validate_candidate(candidate, catalog):
    """Validate pre-upload candidate schema; no self-referential artifact ID."""
    validate_catalog(catalog)
    _object(candidate, {"schema_version", "kind", "family", "recipe_digest", "input_digest", "source", "image"}, "candidate")
    _version(candidate, "candidate")
    _require(candidate["kind"] == "candidate", "candidate kind must be candidate")
    _require(isinstance(candidate["family"], str) and candidate["family"] in catalog["families"], "unknown candidate family")
    for key in ("recipe_digest", "input_digest"):
        _digest(candidate[key], key)
    _validate_source(candidate["source"])
    _validate_image(candidate["image"])


def aggregate_qualification(catalog, candidate, receipts, job_results, *, candidate_artifact_id, policy):
    """Bind exact successful profiles to externally trusted upload/job results.

    This validates consistency only. The caller authenticates the source of all
    receipts, job results, policy and the external candidate artifact ID.
    """
    validate_candidate(candidate, catalog)
    _positive_int(candidate_artifact_id, "trusted candidate artifact ID")
    policy_digest = policy_identity(policy)
    profiles = set(catalog["families"][candidate["family"]]["profiles"])
    _require(policy.get("family") == candidate["family"], "qualification policy family mismatch")
    _require(isinstance(policy.get("profiles"), dict) and set(policy["profiles"]) == profiles,
             "qualification policy profile set mismatch")
    _require(isinstance(job_results, dict) and set(job_results) == profiles, "authoritative job profile set mismatch")
    _require(all(value == "success" for value in job_results.values()), "all authoritative jobs must succeed")
    _require(isinstance(receipts, list), "qualification receipts must be a list")
    by_profile = {}
    for receipt in receipts:
        _object(receipt, {"schema_version", "kind", "profile", "result", "candidate", "candidate_artifact_id", "policy_digest"},
                "qualification receipt")
        _version(receipt, "qualification receipt")
        profile = receipt["profile"]
        _require(isinstance(profile, str) and profile in profiles, "unexpected qualification profile")
        _require(profile not in by_profile, "duplicate qualification profile")
        _require(receipt["kind"] == "qualification-receipt" and receipt["result"] == "passed", "qualification receipt must pass")
        _positive_int(receipt["candidate_artifact_id"], "receipt candidate artifact ID")
        _require(receipt["candidate_artifact_id"] == candidate_artifact_id, "candidate artifact ID substitution")
        # Validate independently: Python dict equality alone considers True == 1.
        validate_candidate(receipt["candidate"], catalog)
        _require(canonical_digest(receipt["candidate"]) == canonical_digest(candidate), "qualification candidate identity mismatch")
        _require(receipt["policy_digest"] == policy_digest, "qualification policy identity mismatch")
        by_profile[profile] = receipt
    _require(set(by_profile) == profiles, "missing qualification profiles")
    return {"schema_version": 1, "kind": "qualification", "candidate": candidate,
            "candidate_artifact_id": candidate_artifact_id, "policy_digest": policy_digest,
            "profiles": sorted(profiles), "receipts_digest": canonical_digest([by_profile[key] for key in sorted(profiles)])}


def validate_production_lock(lock, catalog):
    """Check schema and canonical pointers only, NOT cryptographic provenance.

    Evidence is stored in checked-in files; an optional artifact ID is historical
    audit metadata, never a requirement to download an expiring Actions artifact.
    """
    validate_catalog(catalog)
    _object(lock, {"schema_version", "kind", "families"}, "production lock")
    _version(lock, "production lock")
    _require(lock["kind"] == "production-lock", "production lock must not be a candidate")
    _require(isinstance(lock["families"], dict) and set(lock["families"]) == set(catalog["families"]),
             "production lock family set mismatch")
    for family, entry in lock["families"].items():
        _object(entry, {"image", "platform", "config_digest", "recipe_digest", "input_digest", "qualification", "source"}, "locked family")
        image = entry["image"]
        _require(isinstance(image, str) and image.startswith(catalog["registry"] + "@"),
                 "locked image must use the canonical package without a mutable tag")
        _digest(image[len(catalog["registry"]) + 1:], "locked image manifest digest")
        _require(entry["platform"] == catalog["families"][family]["platform"], "locked platform mismatch")
        for key in ("config_digest", "recipe_digest", "input_digest"):
            _digest(entry[key], "locked " + key)
        _validate_source(entry["source"])
        evidence = _object(entry["qualification"], {"digest", "receipt", "image_bundle", "receipt_bundle"},
                           "qualification evidence", {"artifact_id"})
        _digest(evidence["digest"], "qualification evidence digest")
        paths = []
        for key in ("receipt", "image_bundle", "receipt_bundle"):
            path = _relative_path(evidence[key], "qualification " + key)
            _require(path.startswith("ci/environments/evidence/" + family + "/"), "evidence path must belong to its family")
            paths.append(path)
        _require(len(set(paths)) == len(paths), "qualification evidence paths must be distinct")
        if "artifact_id" in evidence:
            _positive_int(evidence["artifact_id"], "historical qualification artifact ID")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    identity = commands.add_parser("identity", help="Compute declared recipe, resolved input and policy identities")
    identity.add_argument("--catalog", required=True, type=pathlib.Path)
    identity.add_argument("--family", required=True)
    identity.add_argument("--repo-root", required=True, type=pathlib.Path)
    identity.add_argument("--inputs", required=True, type=pathlib.Path)
    identity.add_argument("--policy", required=True, type=pathlib.Path)
    inspect = commands.add_parser("inspect-oci", help="Inspect uncompressed OCI USTAR archive bytes without extraction")
    inspect.add_argument("--archive", required=True, type=pathlib.Path)
    loaded = commands.add_parser("compare-loaded", help="Compare Docker image inspect JSON against OCI identity")
    loaded.add_argument("--image", required=True, type=pathlib.Path)
    loaded.add_argument("--docker-inspect", required=True, type=pathlib.Path)
    qualify = commands.add_parser("qualify", help="Aggregate receipts using externally trusted job results and artifact ID")
    qualify.add_argument("--catalog", required=True, type=pathlib.Path)
    qualify.add_argument("--candidate", required=True, type=pathlib.Path)
    qualify.add_argument("--receipts", required=True, type=pathlib.Path, help="JSON array of qualification receipts")
    qualify.add_argument("--job-results", required=True, type=pathlib.Path, help="JSON object mapping profile to authoritative conclusion")
    qualify.add_argument("--candidate-artifact-id", required=True, type=int)
    qualify.add_argument("--policy", required=True, type=pathlib.Path)
    offline = commands.add_parser("offline-check", help="Validate production lock schema, not cryptographic provenance")
    offline.add_argument("--catalog", required=True, type=pathlib.Path)
    offline.add_argument("--lock", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "identity":
            result = {"schema_version": 1,
                      "recipe_digest": recipe_identity(load_json(args.catalog), args.family, args.repo_root),
                      "input_digest": input_identity(load_json(args.inputs)), "policy_digest": policy_identity(load_json(args.policy))}
        elif args.command == "inspect-oci":
            result = inspect_oci_archive(args.archive)
        elif args.command == "compare-loaded":
            compare_loaded_image(load_json(args.image), load_json(args.docker_inspect))
            result = {"schema_version": 1, "loaded_image_matches": True}
        elif args.command == "qualify":
            result = aggregate_qualification(load_json(args.catalog), load_json(args.candidate), load_json(args.receipts),
                                             load_json(args.job_results), candidate_artifact_id=args.candidate_artifact_id,
                                             policy=load_json(args.policy))
        else:
            validate_production_lock(load_json(args.lock), load_json(args.catalog))
            result = {"schema_version": 1, "offline_schema_valid": True,
                      "notice": "Schema consistency only; not cryptographic provenance verification."}
        print(json.dumps(result, sort_keys=True, indent=2))
        return 0
    except (ContractError, OSError) as error:
        print("ci_environment: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
