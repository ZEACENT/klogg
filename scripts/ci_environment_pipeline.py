#!/usr/bin/env python3
"""Bind environment candidates and qualification receipts to one trusted CI run.

This is orchestration, not a signature verifier. Job conclusions and artifact IDs
come from explicit workflow needs; candidate bytes are inspected before loading.
Only a later protected publisher may turn complete qualification into a release.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import tarfile
import tempfile

import ci_environment as core
import ci_environment_profiles as profiles
from consume_ci_environment import LOCAL_TAGS

REPOSITORY = "ZEACENT/klogg"
WORKFLOW = ".github/workflows/ci-environments.yml"
BUILD_JOBS = {
    "focal-qt5-gcc13": "BuildFocal", "jammy-qt5": "BuildJammy",
    "noble-qt6": "BuildNoble", "resolute-qt6": "BuildResolute",
    "jammy-qt5-tsan": "BuildTsan", "noble-qt693-analysis": "BuildAnalysis",
}
PROFILE_JOBS = {
    "focal-qt5-gcc13": {"appimage": "QualifyAppImage"},
    "jammy-qt5": {"deb": "QualifyJammyDeb", "asan-lsan": "QualifyAsan", "ubsan": "QualifyUbsan"},
    "noble-qt6": {"deb": "QualifyNobleDeb"},
    "resolute-qt6": {"deb": "QualifyResoluteDeb"},
    "jammy-qt5-tsan": {"tsan": "QualifyTsan"},
    "noble-qt693-analysis": {"static": "QualifyStatic", "coverage": "QualifyCoverage", "codeql": "QualifyCodeql"},
}


class PipelineError(core.ContractError):
    """Missing or contradictory evidence prevents further execution."""


def require(condition, message):
    if not condition:
        raise PipelineError(message)


def sha256(path):
    digest = hashlib.sha256()
    with pathlib.Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def regular(root, relative):
    root = pathlib.Path(root)
    require(not root.is_symlink() and root.is_dir(), "artifact root must be a real directory")
    relative = core._relative_path(relative, "artifact path")
    path = root / relative
    require(not any(item.is_symlink() for item in (path,) + tuple(path.parents)[:len(pathlib.PurePosixPath(relative).parts)]),
            "artifact path contains a symlink")
    require(path.is_file(), "missing artifact file: " + relative)
    return path


def read_document(root, relative):
    return core.load_json(regular(root, relative))


def write_document(path, document):
    path = pathlib.Path(path)
    require(not path.exists() and not path.is_symlink(), "refusing to overwrite evidence: " + str(path))
    path.parent.mkdir(parents=True, exist_ok=True)
    data = json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n"
    with path.open("x", encoding="ascii") as stream:
        stream.write(data)


def validate_source(source):
    core._validate_source(source)
    require(source["workflow"] == WORKFLOW, "unexpected producer workflow")
    require(source["ref"].startswith("refs/heads/") and not source["ref"].endswith(("/", ".")),
            "producer must use an explicit branch ref")


def run(command, runner=None, *, timeout=120, cwd=None):
    try:
        result = (runner or subprocess.run)(command, check=True, capture_output=True, text=True,
                                             timeout=timeout, cwd=cwd)
    except (OSError, subprocess.SubprocessError) as error:
        detail = ""
        for field in ("stdout", "stderr"):
            text = getattr(error, field, None)
            if isinstance(text, str) and text.strip():
                detail += "; " + field + "=" + text.strip()[-512:]
        raise PipelineError("pipeline command failed: " + " ".join(command[:3]) + detail) from error
    require(isinstance(result.stdout, str), "command did not return text output")
    return result.stdout


def source_context(expected_source_sha, analysis_base_sha, *, environment=None, runner=None, repo_root=None):
    environment = os.environ if environment is None else environment
    require(environment.get("GITHUB_EVENT_NAME") == "workflow_dispatch", "producer requires explicit dispatch")
    require(environment.get("GITHUB_REPOSITORY") == REPOSITORY, "producer requires the authoritative repository")
    require(isinstance(expected_source_sha, str) and re.fullmatch(r"[0-9a-f]{40}", expected_source_sha)
            and expected_source_sha != "0" * 40, "expected source must be a full commit SHA")
    require(environment.get("GITHUB_SHA") == expected_source_sha, "dispatch source SHA mismatch")
    require(isinstance(analysis_base_sha, str) and re.fullmatch(r"[0-9a-f]{40}", analysis_base_sha)
            and analysis_base_sha != "0" * 40 and analysis_base_sha != expected_source_sha,
            "analysis requires a distinct full base SHA")
    source = {"repository": REPOSITORY, "sha": expected_source_sha,
              "ref": environment.get("GITHUB_REF"), "workflow": WORKFLOW}
    for field, variable in (("run_id", "GITHUB_RUN_ID"), ("run_attempt", "GITHUB_RUN_ATTEMPT")):
        value = environment.get(variable, "")
        require(isinstance(value, str) and re.fullmatch(r"[1-9][0-9]*", value), "invalid " + variable)
        source[field] = int(value)
    validate_source(source)
    require(run(["git", "rev-parse", "HEAD"], runner, cwd=repo_root).strip() == expected_source_sha,
            "checked-out source differs from dispatch")
    run(["git", "merge-base", "--is-ancestor", analysis_base_sha, expected_source_sha], runner, cwd=repo_root)
    return source


def verify_artifact(artifact_id, source, runner=None):
    validate_source(source)
    core._positive_int(artifact_id, "artifact ID")
    output = run(["gh", "api", "repos/" + REPOSITORY + "/actions/artifacts/" + str(artifact_id)], runner)
    require(len(output.encode("utf-8")) <= core.MAX_METADATA_BYTES, "artifact metadata is oversized")
    artifact = core._parse_json(output.encode("utf-8"), "artifact metadata")
    require(isinstance(artifact, dict) and type(artifact.get("id")) is int
            and artifact["id"] == artifact_id and artifact.get("expired") is False,
            "artifact identity is missing, expired or substituted")
    workflow_run = artifact.get("workflow_run")
    require(isinstance(workflow_run, dict) and type(workflow_run.get("id")) is int
            and workflow_run["id"] == source["run_id"] and workflow_run.get("head_sha") == source["sha"]
            and workflow_run.get("head_branch") == source["ref"][len("refs/heads/"):],
            "artifact was not produced by the exact expected workflow run and branch")


def retained_material_manifest(candidate_root, inputs):
    records = inputs.get("files")
    require(isinstance(records, list) and all(isinstance(record, dict) for record in records),
            "resolved input file inventory is malformed")
    selected = [record for record in records if record.get("path") == "inputs/material-manifest.json"]
    require(len(selected) <= 1, "duplicate material provenance record")
    if not selected:
        require(not (pathlib.Path(candidate_root) / "material-manifest.json").exists(), "undeclared material provenance")
        return None
    record = selected[0]
    core._object(record, {"path", "sha256", "size"}, "material provenance record")
    core._digest(record["sha256"], "material provenance SHA-256", prefix=False)
    require(type(record["size"]) is int and 0 < record["size"] <= core.MAX_METADATA_BYTES,
            "material provenance size is invalid")
    path = regular(candidate_root, "material-manifest.json")
    require(path.stat().st_size == record["size"], "material provenance size mismatch")
    data = path.read_bytes()
    require(len(data) == record["size"] and hashlib.sha256(data).hexdigest() == record["sha256"],
            "material provenance hash mismatch")
    manifest = core._parse_json(data, "material provenance")
    require(isinstance(manifest, dict) and type(manifest.get("schema_version")) is int
            and manifest["schema_version"] == 1 and manifest.get("kind") == "ci-material-acquisition"
            and manifest.get("family") == inputs["family"], "material provenance family or schema mismatch")
    return data


def validate_candidate_files(repo_root, family, candidate_root, source, archive_sha256):
    """Inspect exact artifact bytes without executing the candidate or its files."""
    try:
        validate_source(source)
        core._digest(archive_sha256, "external OCI archive SHA-256", prefix=False)
        root = pathlib.Path(repo_root)
        catalog = read_document(root, "ci/environments/recipes.json")
        candidate = read_document(candidate_root, "candidate.json")
        core.validate_candidate(candidate, catalog)
        require(candidate["family"] == family and core.canonical_digest(candidate["source"]) == core.canonical_digest(source),
                "candidate family or producer source/run/attempt mismatch")
        require(candidate["recipe_digest"] == core.recipe_identity(catalog, family, root), "candidate recipe is stale")
        require(candidate["image"]["archive_sha256"] == archive_sha256, "external candidate archive hash mismatch")
        transport = read_document(candidate_root, "transport.json")
        core._object(transport, {"schema_version", "kind", "candidate_digest", "oci_archive", "docker_archive",
                                 "inputs_manifest", "config_digest", "diff_ids", "loaded_image_matches"}, "candidate transport")
        require(type(transport["schema_version"]) is int and transport["schema_version"] == 1
                and transport["kind"] == "docker-load-transport" and transport["loaded_image_matches"] is True,
                "unsupported candidate transport")
        require(transport["candidate_digest"] == core.canonical_digest(candidate)
                and transport["config_digest"] == candidate["image"]["config_digest"]
                and transport["diff_ids"] == candidate["image"]["diff_ids"], "candidate transport identity mismatch")
        for field, filename in (("oci_archive", "candidate.oci.tar"), ("docker_archive", "candidate.docker.tar"),
                                ("inputs_manifest", "inputs.json")):
            record = transport[field]
            core._object(record, {"path", "sha256"}, "transport file")
            core._digest(record["sha256"], "transport hash", prefix=False)
            require(record["path"] == filename, "unexpected transport filename")
            file = regular(candidate_root, filename)
            require(sha256(file) == record["sha256"], "transport file hash mismatch: " + filename)
        require(transport["oci_archive"]["sha256"] == archive_sha256, "OCI transport hash substitution")
        inputs = read_document(candidate_root, "inputs.json")
        require(inputs.get("family") == family and inputs.get("platform") == "linux/amd64"
                and core.input_identity(inputs) == candidate["input_digest"], "candidate input identity mismatch")
        retained_material_manifest(candidate_root, inputs)
        observed = core.inspect_oci_archive(regular(candidate_root, "candidate.oci.tar"))
        require(core.canonical_digest(observed) == core.canonical_digest(candidate["image"]), "OCI archive identity mismatch")
        return candidate, inputs, transport
    except core.ContractError as error:
        raise PipelineError(str(error)) from error


def prepare_candidate(repo_root, family, candidate_root, candidate_artifact_id, archive_sha256, source, output, *, runner=None):
    output = pathlib.Path(output)
    require(not output.exists() and not output.is_symlink(), "prepared output already exists")
    candidate, _, transport = validate_candidate_files(repo_root, family, candidate_root, source, archive_sha256)
    verify_artifact(candidate_artifact_id, source, runner)
    companion = regular(candidate_root, "candidate.docker.tar")
    loaded = run(["docker", "load", "--input", str(companion)], runner, timeout=900)
    require([line for line in loaded.splitlines() if line.startswith("Loaded image")]
            == ["Loaded image ID: " + candidate["image"]["config_digest"]], "Docker loaded a different image")
    inspected = run(["docker", "image", "inspect", candidate["image"]["config_digest"]], runner)
    require(len(inspected.encode("utf-8")) <= core.MAX_METADATA_BYTES, "Docker inspect output is oversized")
    try:
        core.compare_loaded_image(candidate["image"], core._parse_json(inspected.encode("utf-8"), "Docker inspect"))
    except core.ContractError as error:
        raise PipelineError(str(error)) from error
    require(sha256(companion) == transport["docker_archive"]["sha256"], "companion changed while loading")
    if family in LOCAL_TAGS:
        run(["docker", "image", "tag", candidate["image"]["config_digest"], LOCAL_TAGS[family]], runner)
    preparation = {"schema_version": 1, "kind": "prepared-candidate", "candidate_artifact_id": candidate_artifact_id,
                   "candidate_digest": core.canonical_digest(candidate), "loaded_image_matches": True}
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="klogg-prepared-", dir=str(output.parent)) as temporary:
        staged = pathlib.Path(temporary) / "prepared"
        write_document(staged / "preparation.json", preparation)
        require(not output.exists() and not output.is_symlink(), "prepared output appeared concurrently")
        staged.rename(output)
    return preparation


def family_policy(repo_root, family, role_materials=None):
    root = pathlib.Path(repo_root)
    catalog = read_document(root, "ci/environments/recipes.json")
    document = read_document(root, "ci/environments/profiles.json")
    require(family in document["families"], "unknown policy family")
    required = {name for name, profile in document["families"][family].items() if profile["role_materials"]}
    materials = {}
    if required:
        locked = core.load_json(role_materials) if role_materials else read_document(root, "ci/environments/role-materials.json")
        materials = {name: locked[name] for name in required if name in locked}
        require(set(materials) == required, "missing required role material lock")
    return profiles.build_policy(catalog, document, family, root, role_materials=materials)


def profile_receipt(repo_root, family, profile, candidate_root, candidate_artifact_id, source, output, *, role_materials=None):
    core._positive_int(candidate_artifact_id, "candidate artifact ID")
    candidate = read_document(candidate_root, "candidate.json")
    candidate, _, _ = validate_candidate_files(repo_root, family, candidate_root, source, candidate["image"]["archive_sha256"])
    policy = family_policy(repo_root, family, role_materials)
    require(profile in policy["profiles"], "unexpected qualification profile")
    # The caller's preceding qualification step and authoritative job result are
    # the success authority. This metadata is never sufficient for publication.
    receipt = {"schema_version": 1, "kind": "qualification-receipt", "profile": profile, "result": "passed",
               "candidate": candidate, "candidate_artifact_id": candidate_artifact_id,
               "policy_digest": core.policy_identity(policy)}
    write_document(output, receipt)
    return receipt


def validate_artifact_identities(catalog, identities, job_results):
    require(set(catalog["families"]) == set(BUILD_JOBS), "pipeline family inventory differs from catalog")
    for family, jobs in PROFILE_JOBS.items():
        require(set(catalog["families"][family]["profiles"]) == set(jobs), "pipeline profile inventory differs from catalog")
    core._object(identities, {"schema_version", "candidates", "profiles"}, "artifact identities")
    core._version(identities, "artifact identities")
    for field in ("candidates", "profiles"):
        require(isinstance(identities[field], dict) and set(identities[field]) == set(BUILD_JOBS),
                "exact artifact family set is required")
    expected_jobs = set(BUILD_JOBS.values()) | {job for mapping in PROFILE_JOBS.values() for job in mapping.values()}
    require(isinstance(job_results, dict) and set(job_results) == expected_jobs
            and all(result == "success" for result in job_results.values()),
            "every exact builder and qualification job must succeed")
    ids = set()
    for family, builder in BUILD_JOBS.items():
        candidate = identities["candidates"][family]
        core._object(candidate, {"job", "artifact_id", "archive_sha256"}, "candidate artifact identity")
        require(candidate["job"] == builder, "candidate artifact producer job substitution")
        mapping = identities["profiles"][family]
        require(isinstance(mapping, dict) and set(mapping) == set(PROFILE_JOBS[family]), "exact profile artifact set required")
        for profile, receipt in mapping.items():
            core._object(receipt, {"job", "artifact_id", "archive_sha256", "receipt_sha256"}, "receipt artifact identity")
            require(receipt["job"] == PROFILE_JOBS[family][profile], "receipt artifact producer job substitution")
            core._digest(receipt["receipt_sha256"], "receipt SHA-256", prefix=False)
        for entry in [candidate] + list(mapping.values()):
            core._positive_int(entry["artifact_id"], "artifact ID")
            require(entry["artifact_id"] not in ids, "duplicate artifact ID")
            ids.add(entry["artifact_id"])
            core._digest(entry["archive_sha256"], "artifact SHA-256", prefix=False)


def read_profile_archive(path, identity):
    require(path.stat().st_size <= core.MAX_METADATA_BYTES, "qualification archive exceeds size limit")
    require(sha256(path) == identity["archive_sha256"], "qualification archive SHA mismatch")
    try:
        with tarfile.open(path, "r|gz") as archive:
            receipt = None
            for member in archive:
                require(member.name == "receipt.json" and member.isfile() and not member.pax_headers
                        and 0 < member.size <= core.MAX_METADATA_BYTES and receipt is None,
                        "qualification archive must contain only one regular receipt.json")
                stream = archive.extractfile(member)
                require(stream is not None, "missing qualification receipt data")
                receipt = stream.read(core.MAX_METADATA_BYTES + 1)
                require(len(receipt) == member.size, "truncated qualification receipt")
    except (tarfile.TarError, EOFError) as error:
        raise PipelineError("invalid qualification archive") from error
    require(receipt is not None and hashlib.sha256(receipt).hexdigest() == identity["receipt_sha256"],
            "raw qualification receipt hash mismatch")
    return core._parse_json(receipt, "qualification receipt")


def aggregate(repo_root, source, evidence_root, job_results, artifact_identities, operation, sarif_result, output, *, runner=None):
    """Qualify all families together; no image execution or registry writes."""
    try:
        validate_source(source)
        require(operation in ("qualify", "publish"), "unknown qualification operation")
        require(sarif_result == ("success" if operation == "publish" else "skipped"),
                "SARIF uploader did not reach the required mode-specific result")
        catalog = read_document(repo_root, "ci/environments/recipes.json")
        core.validate_catalog(catalog)
        validate_artifact_identities(catalog, artifact_identities, job_results)
        output = pathlib.Path(output)
        require(not output.exists() and not output.is_symlink(), "qualified output already exists")
        output.parent.mkdir(parents=True, exist_ok=True)
        result = {"schema_version": 1, "kind": "qualified-run", "operation": operation,
                  "source": source, "sarif_result": sarif_result, "job_results": job_results,
                  "artifact_identities": artifact_identities, "families": {}}
        with tempfile.TemporaryDirectory(prefix="klogg-qualification-", dir=str(output.parent)) as temporary:
            staged = pathlib.Path(temporary) / "qualified"
            for family in sorted(BUILD_JOBS):
                identity = artifact_identities["candidates"][family]
                candidate_root = pathlib.Path(evidence_root) / "candidates" / family
                candidate, inputs, _ = validate_candidate_files(repo_root, family, candidate_root, source, identity["archive_sha256"])
                verify_artifact(identity["artifact_id"], source, runner)
                receipts = []
                for profile in sorted(PROFILE_JOBS[family]):
                    receipt_identity = artifact_identities["profiles"][family][profile]
                    archive = regular(pathlib.Path(evidence_root) / "receipts" / family / profile, "qualification.tar.gz")
                    receipt = read_profile_archive(archive, receipt_identity)
                    require(receipt.get("profile") == profile, "receipt profile substitution")
                    verify_artifact(receipt_identity["artifact_id"], source, runner)
                    receipts.append(receipt)
                policy = family_policy(repo_root, family)
                qualification = core.aggregate_qualification(catalog, candidate, receipts,
                    {profile: job_results[job] for profile, job in PROFILE_JOBS[family].items()},
                    candidate_artifact_id=identity["artifact_id"], policy=policy)
                directory = staged / "families" / family
                write_document(directory / "candidate.json", candidate)
                write_document(directory / "inputs.json", inputs)
                write_document(directory / "profile-receipts.json", receipts)
                write_document(directory / "verification.json", qualification)
                material_bytes = retained_material_manifest(candidate_root, inputs)
                if material_bytes is not None:
                    with (directory / "material-manifest.json").open("xb") as stream:
                        stream.write(material_bytes)
                result["families"][family] = {"candidate_digest": core.canonical_digest(candidate),
                    "input_digest": core.input_identity(inputs), "receipt_sha256": sha256(directory / "verification.json")}
            write_document(staged / "qualified.json", result)
            require(not output.exists() and not output.is_symlink(), "qualified output appeared concurrently")
            staged.rename(output)
        return result
    except core.ContractError as error:
        raise PipelineError(str(error)) from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    source = commands.add_parser("source-context")
    source.add_argument("--expected-source-sha", required=True)
    source.add_argument("--analysis-base-sha", required=True)
    source.add_argument("--output", required=True, type=pathlib.Path)
    prepare = commands.add_parser("prepare-candidate")
    for field in ("repo-root", "candidate-root", "source", "output"):
        prepare.add_argument("--" + field, required=True, type=pathlib.Path)
    prepare.add_argument("--family", required=True)
    prepare.add_argument("--candidate-artifact-id", required=True, type=int)
    prepare.add_argument("--archive-sha256", required=True)
    receipt = commands.add_parser("profile-receipt")
    for field in ("repo-root", "candidate-root", "source", "output"):
        receipt.add_argument("--" + field, required=True, type=pathlib.Path)
    receipt.add_argument("--family", required=True)
    receipt.add_argument("--profile", required=True)
    receipt.add_argument("--candidate-artifact-id", required=True, type=int)
    receipt.add_argument("--role-materials", type=pathlib.Path)
    gate = commands.add_parser("aggregate")
    for field in ("repo-root", "source", "evidence-root", "job-results", "artifact-identities", "output"):
        gate.add_argument("--" + field, required=True, type=pathlib.Path)
    gate.add_argument("--operation", required=True, choices=("qualify", "publish"))
    gate.add_argument("--sarif-result", required=True, choices=("skipped", "success"))
    material = commands.add_parser("materialize")
    for field in ("repo-root", "source", "output"):
        material.add_argument("--" + field, required=True, type=pathlib.Path)
    material.add_argument("--family", required=True)
    material.add_argument("--timeout", type=int, default=1800)
    fixture = commands.add_parser("prepare-fixture")
    for field in ("repo-root", "source", "output"):
        fixture.add_argument("--" + field, required=True, type=pathlib.Path)
    fixture.add_argument("--version", required=True)
    fixture_consumer = commands.add_parser("consume-fixture")
    for field in ("repo-root", "source", "fixture-root", "output"):
        fixture_consumer.add_argument("--" + field, required=True, type=pathlib.Path)
    fixture_consumer.add_argument("--fixture-artifact-id", required=True, type=int)
    fixture_consumer.add_argument("--archive-sha256", required=True)
    publisher = commands.add_parser("publish")
    for field in ("repo-root", "source", "qualified-root", "evidence-root", "output"):
        publisher.add_argument("--" + field, required=True, type=pathlib.Path)
    publisher.add_argument("--qualified-artifact-id", required=True, type=int)
    publisher.add_argument("--qualified-archive-sha256", required=True)
    finalize = commands.add_parser("finalize-publication")
    for field in ("repo-root", "source", "qualified-root", "publication-root", "bundle-map", "output"):
        finalize.add_argument("--" + field, required=True, type=pathlib.Path)
    verify = commands.add_parser("verify-publication")
    for field in ("repo-root", "source", "publication-root", "output"):
        verify.add_argument("--" + field, required=True, type=pathlib.Path)
    verify.add_argument("--publication-artifact-id", required=True, type=int)
    verify.add_argument("--archive-sha256", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "source-context":
            write_document(args.output, source_context(args.expected_source_sha, args.analysis_base_sha))
        elif args.command == "prepare-candidate":
            prepare_candidate(args.repo_root, args.family, args.candidate_root, args.candidate_artifact_id,
                              args.archive_sha256, core.load_json(args.source), args.output)
        elif args.command == "profile-receipt":
            profile_receipt(args.repo_root, args.family, args.profile, args.candidate_root,
                            args.candidate_artifact_id, core.load_json(args.source), args.output,
                            role_materials=args.role_materials)
        elif args.command == "aggregate":
            aggregate(args.repo_root, core.load_json(args.source), args.evidence_root,
                      core.load_json(args.job_results), core.load_json(args.artifact_identities),
                      args.operation, args.sarif_result, args.output)
        elif args.command == "materialize":
            from materialize_ci_environment import materialize
            validate_source(core.load_json(args.source))
            materialize(args.repo_root, args.family, args.output, timeout=args.timeout)
        elif args.command == "prepare-fixture":
            from ci_environment_fixture import prepare_fixture
            prepare_fixture(args.repo_root, core.load_json(args.source), args.version, args.output)
        elif args.command == "consume-fixture":
            from ci_environment_fixture import consume_fixture
            consume_fixture(args.repo_root, core.load_json(args.source), args.fixture_root,
                            args.fixture_artifact_id, args.archive_sha256, args.output)
        elif args.command == "publish":
            from publish_ci_environment import publish
            publish(args.repo_root, core.load_json(args.source), args.qualified_root,
                    args.qualified_artifact_id, args.qualified_archive_sha256, args.evidence_root, args.output)
        elif args.command == "finalize-publication":
            from publish_ci_environment import finalize_publication
            finalize_publication(args.repo_root, core.load_json(args.source), args.qualified_root,
                                 args.publication_root, core.load_json(args.bundle_map), args.output)
        elif args.command == "verify-publication":
            from publish_ci_environment import verify_publication
            verify_publication(args.repo_root, core.load_json(args.source), args.publication_root,
                               args.publication_artifact_id, args.archive_sha256, args.output)
        return 0
    except (core.ContractError, OSError) as error:
        print("ci_environment_pipeline: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
