#!/usr/bin/env python3
"""Resolve a production CI image only after current policy and provenance checks.

Only checked-in family locks are accepted. Missing/stale evidence fails closed:
there is no digest override, image build, package installation or bootstrap path.
Anonymous registry checks precede any optional Docker pull. Verification never
runs a container. Detached signatures are verified by the existing gh boundary,
not by trusting receipt JSON. Tests may inject a registry client and tool runner.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import ci_environment as core
import ci_environment_profiles as profiles_module
import ci_environment_registry as registry


LOCAL_TAGS = {
    "focal-qt5-gcc13": "zeacent/klogg_ubuntu20.04",
    "jammy-qt5": "zeacent/klogg_ubuntu22.04",
    "noble-qt6": "zeacent/klogg_ubuntu24.04",
    "resolute-qt6": "zeacent/klogg_ubuntu26.04",
    "jammy-qt5-tsan": "zeacent/klogg_ubuntu22.04-tsan",
}
ENVIRONMENT_DIR = "ci/environments/"


def _digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def _read_repository_file(root, relative, limit=core.MAX_METADATA_BYTES):
    """Reject unsafe paths and symlink ancestors, then snapshot bounded bytes."""
    core._relative_path(relative, "repository input")
    path = core._regular_recipe_file(root, relative)
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_BINARY", 0)
    with os.fdopen(os.open(path, flags), "rb") as stream:
        before = os.fstat(stream.fileno())
        core._require(stat.S_ISREG(before.st_mode), "repository input must be a regular file")
        data = stream.read(limit + 1)
        after = os.fstat(stream.fileno())
    core._require(len(data) <= limit, "repository input exceeds size limit: " + relative)
    core._regular_recipe_file(root, relative)
    current = path.stat()
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)
    core._require(identity(before) == identity(after) == identity(current),
                  "repository input changed while reading: " + relative)
    return data


def _load_document(root, relative):
    return core._parse_json(_read_repository_file(root, relative), relative)


def _validate_receipt(receipt, catalog, family, locked, policy_digest):
    core._object(receipt, {"schema_version", "kind", "candidate", "candidate_artifact_id",
                           "policy_digest", "profiles", "receipts_digest"}, "qualification")
    core._version(receipt, "qualification")
    core._require(receipt["kind"] == "qualification", "expected signed qualification kind")
    core._positive_int(receipt["candidate_artifact_id"], "qualified candidate artifact ID")
    core._digest(receipt["receipts_digest"], "profile receipts digest")
    core._require(receipt["policy_digest"] == policy_digest, "current qualification policy digest mismatch")
    core._require(receipt["profiles"] == sorted(catalog["families"][family]["profiles"]),
                  "qualification profile set mismatch")
    candidate = receipt["candidate"]
    core.validate_candidate(candidate, catalog)
    core._require(candidate["family"] == family, "qualified candidate family mismatch")
    for field in ("recipe_digest", "input_digest"):
        core._require(candidate[field] == locked[field], "qualified candidate " + field + " mismatch")
    core._require(core.canonical_digest(candidate["source"]) == core.canonical_digest(locked["source"]),
                  "qualified candidate source mismatch")
    image = candidate["image"]
    core._require(locked["image"] == core.REGISTRY + "@" + image["manifest_digest"],
                  "qualified candidate image manifest mismatch")
    core._require(image["config_digest"] == locked["config_digest"] and image["platform"] == locked["platform"],
                  "qualified candidate config/platform mismatch")
    return candidate


def _compare_registry_image(published, expected):
    core._require(isinstance(published, dict), "registry image metadata must be an object")
    for key in ("manifest_digest", "config_digest", "platform", "diff_ids", "layer_digests"):
        core._require(published.get(key) == expected[key], "registry image " + key + " mismatch")
    manifest = published.get("manifest_bytes")
    core._require(isinstance(manifest, bytes) and len(manifest) <= registry.METADATA_LIMIT
                  and _digest(manifest) == expected["manifest_digest"], "registry raw manifest digest mismatch")
    return manifest


def _run(command, runner):
    try:
        return runner(command, check=True, capture_output=True, text=True, timeout=600)
    except (OSError, subprocess.SubprocessError) as error:
        raise core.ContractError("verified image Docker operation failed: " + command[1]) from error


def consume(family, repo_root, *, pull=False, retag=False, client=None, runner=None):
    """Return the literal locked reference only after every required check.

    Optional pull/retag affect local image storage, not Docker daemon settings.
    The analysis family has no legacy local tag; it can only resolve or pull.
    """
    core._identifier(family, "family")
    core._require(not retag or pull, "retag requires an explicit pull")
    core._require(not retag or family in LOCAL_TAGS, "family has no existing local retag contract")
    root = pathlib.Path(repo_root).resolve()
    catalog = _load_document(root, ENVIRONMENT_DIR + "recipes.json")
    profiles = _load_document(root, ENVIRONMENT_DIR + "profiles.json")
    lock = _load_document(root, ENVIRONMENT_DIR + "lock.json")
    core.validate_production_lock(lock, catalog)
    core._require(family in catalog["families"], "unknown environment family")
    profiles_module.validate_profiles(catalog, profiles, root, require_recipe_files=False)
    locked = lock["families"][family]
    recipe_digest = core.recipe_identity(catalog, family, root)
    inputs = _load_document(root, ENVIRONMENT_DIR + "inputs/" + family + ".json")
    input_digest = core.input_identity(inputs)
    core._require(inputs.get("family") == family, "resolved input manifest family mismatch")
    core._require(inputs.get("platform") == core.PLATFORM, "resolved input manifest platform mismatch")
    core._require(locked["recipe_digest"] == recipe_digest, "current recipe digest mismatch")
    core._require(locked["input_digest"] == input_digest, "current resolved input digest mismatch")
    requires_materials = any(profile["role_materials"] for profile in profiles["families"][family].values())
    materials = _load_document(root, ENVIRONMENT_DIR + "role-materials.json") if requires_materials else None
    policy = profiles_module.build_policy(catalog, profiles, family, root, role_materials=materials)
    policy_digest = core.policy_identity(policy)

    evidence = locked["qualification"]
    receipt_bytes = _read_repository_file(root, evidence["receipt"], registry.METADATA_LIMIT)
    core._require(_digest(receipt_bytes) == evidence["digest"], "raw qualification receipt digest mismatch")
    receipt = core._parse_json(receipt_bytes, evidence["receipt"])
    candidate = _validate_receipt(receipt, catalog, family, locked, policy_digest)
    image_bundle = _read_repository_file(root, evidence["image_bundle"], registry.BUNDLE_LIMIT)
    receipt_bundle = _read_repository_file(root, evidence["receipt_bundle"], registry.BUNDLE_LIMIT)
    core._require(bool(image_bundle) and bool(receipt_bundle), "detached signature bundles must not be empty")
    run = runner if runner is not None else subprocess.run
    public_registry = client if client is not None else registry.RegistryClient()
    # Verify immutable snapshots, not paths a concurrent checkout can replace
    # between the raw digest comparison and the trusted verifier's file read.
    with tempfile.TemporaryDirectory(prefix="klogg-env-verify-") as temporary:
        temporary = pathlib.Path(temporary)
        subject = temporary / "verification.json"
        subject.write_bytes(receipt_bytes)
        receipt_signature = temporary / "receipt.sigstore.json"
        receipt_signature.write_bytes(receipt_bundle)
        core._require(registry.verify_attestation(subject, receipt_signature, locked["source"],
                                                 "verification.json", runner=run) is True,
                      "receipt attestation was not verified")
        published = public_registry.read_image(candidate["image"]["manifest_digest"])
        manifest = _compare_registry_image(published, candidate["image"])
        image_subject = temporary / "manifest.json"
        image_subject.write_bytes(manifest)
        image_signature = temporary / "image.sigstore.json"
        image_signature.write_bytes(image_bundle)
        core._require(registry.verify_attestation(image_subject, image_signature, locked["source"],
                                                 registry.REGISTRY, runner=run) is True,
                      "image attestation was not verified")

    image = locked["image"]
    local_tag = None
    if pull:
        _run(["docker", "pull", "--platform", core.PLATFORM, image], run)
        inspected = _run(["docker", "image", "inspect", image], run)
        core._require(len(inspected.stdout) <= core.MAX_METADATA_BYTES, "Docker image inspect output exceeds size limit")
        core.compare_loaded_image(candidate["image"], core._parse_json(inspected.stdout.encode("utf-8"), "Docker image inspect"))
        if retag:
            local_tag = LOCAL_TAGS[family]
            _run(["docker", "tag", image, local_tag], run)
    return {"schema_version": 1, "family": family, "image": image,
            "config_digest": locked["config_digest"], "recipe_digest": recipe_digest,
            "input_digest": input_digest, "policy_digest": policy_digest,
            "pulled": bool(pull), "local_tag": local_tag}


def _github_output(path, image):
    # The caller controls the output location, never its content or output key.
    core._require(image.startswith(core.REGISTRY + "@"), "invalid GitHub image output")
    core._digest(image[len(core.REGISTRY) + 1:], "GitHub image output")
    path = pathlib.Path(path).absolute()
    for ancestor in (path, *path.parents):
        core._require(not ancestor.is_symlink(), "GitHub output path must not traverse a symlink")
    flags = os.O_WRONLY | os.O_APPEND | os.O_CREAT | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_BINARY", 0)
    with os.fdopen(os.open(path, flags, 0o600), "ab") as stream:
        core._require(stat.S_ISREG(os.fstat(stream.fileno()).st_mode), "GitHub output must be a regular file")
        stream.write(("image=" + image + "\n").encode("ascii"))


def main(argv=None, *, client=None, runner=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--family", required=True)
    parser.add_argument("--repo-root", type=pathlib.Path, default=SCRIPT_DIR.parent)
    parser.add_argument("--pull", action="store_true", help="pull and inspect the fully verified exact digest")
    parser.add_argument("--retag", action="store_true", help="with --pull, assign the existing fixed local family tag")
    parser.add_argument("--github-output", type=pathlib.Path, help="append only image=<verified locked reference>")
    args = parser.parse_args(argv)
    try:
        result = consume(args.family, args.repo_root, pull=args.pull, retag=args.retag, client=client, runner=runner)
        if args.github_output is not None:
            _github_output(args.github_output, result["image"])
        print(json.dumps(result, sort_keys=True, indent=2))
        return 0
    except (core.ContractError, registry.RegistryError, OSError) as error:
        print("consume_ci_environment: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
