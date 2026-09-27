#!/usr/bin/env python3
"""Read-only seven-target full-build provenance and byte comparison.

Inputs are independently fetched Actions run, attempt-jobs, artifacts (including
immutable IDs), downloads, and GitHub-controlled `toJSON(needs)` from a child
workflow. The Jobs REST API does NOT expose job outputs. This gate cannot
authenticate the caller's checkout or `needs` transport; the parent/child must
establish both before calling it. A GitHub-hosted OIDC signer plus checked-out
runs-on policy attests scheduler provenance, not physical runner hardware.

ADB package/legal requalification additionally needs same-run authenticated
BuildAdbHelperLegalAssets; this module verifies the signed full binary tar and
its ADB envelope but does not claim that separate qualification step.
"""

from __future__ import annotations

import hashlib
import io
import os
import pathlib
import posixpath
import re
import subprocess
import sys
import tarfile
import tempfile

from ci_dependency_catalog import validate_catalog
from ci_dependency_core import MANIFEST, canonical_adb_mode, verify_core
from ci_dependency_pipeline import BUILDER_JOBS, BUILDER_WORKFLOW, compute_identities
from ci_dependency_lock import _source
from ci_environment_registry import REPOSITORY, _json
from verify_adb_helper_envelope import verify_artifact_envelope
from verify_ios_native_stack import read_json, validate_build_receipt

MAX_FULL_TAR_BYTES = 2 * 1024**3
MAX_UNPACKED_BYTES = 4 * 1024**3
MAX_MEMBERS = 8192
MAX_BUNDLE_BYTES = 10 * 1024**2
JOB_HEADER = re.compile(r"^  ([A-Za-z][A-Za-z0-9_]*):\s*$", re.MULTILINE)
RUNNER = re.compile(r"^    runs-on:\s*(.*?)\s*$", re.MULTILINE)
DECIMAL = re.compile(r"[1-9][0-9]*\Z")
STEP_ATTESTATION = "Attest exact full native tar provenance"
STEP_IOS_PREFLIGHT = "Verify pinned iOS producer toolchain"
STEP_IOS_HOST_PROBE = "Observe unreviewed iOS host tool inputs"


class FullEvidenceError(ValueError):
    """Absent, untrusted or mismatched native full-build evidence."""


def _require(condition: bool, reason: str) -> None:
    if not condition:
        raise FullEvidenceError(reason)


def _job_names(catalog: dict) -> dict[str, str]:
    return {target_id: (f"Source-built ADB helper {row['target']}" if row["component"] == "adb-helper"
                        else f"iOS native stack {row['target']}")
            for target_id, row in catalog.items()}


def reviewed_runners(workflow_text: str, catalog: dict) -> dict[str, str]:
    """Only literal runs-on in the seven top-level job blocks is acceptable."""
    _require(isinstance(workflow_text, str), "missing checked-out workflow")
    blocks = list(JOB_HEADER.finditer(workflow_text))
    _require(len({item.group(1) for item in blocks}) == len(blocks), "duplicate workflow jobs")
    jobs = {match.group(1): workflow_text[match.end():blocks[index + 1].start()
                                         if index + 1 < len(blocks) else len(workflow_text)]
            for index, match in enumerate(blocks)}
    result = {}
    for target_id, row in catalog.items():
        block = jobs.get(BUILDER_JOBS[target_id], "")
        labels = RUNNER.findall(block)
        _require(labels == [row["runner"]] and "self-hosted" not in labels[0],
                 "checked-out runs-on differs from reviewed hosted runner")
        result[target_id] = labels[0]
    ios_block = jobs.get("BuildIosNativeX64", "")
    preflight = re.search(
        r"(?ms)^      - name: " + re.escape(STEP_IOS_PREFLIGHT)
        + r"$\n(.*?)(?=^      - (?:name|uses):|\Z)", ios_block)
    _require(preflight is not None
             and re.search(r"(?m)^          python3 scripts/ci_dependency_toolchain\.py \\\s*$",
                           preflight.group(1))
             and re.search(r'(?m)^            --target-id "ios-\$\{KLOGG_IOS_ARCHITECTURE\}"$',
                           preflight.group(1)),
             "checked-out workflow lacks pinned iOS toolchain preflight")
    probe = re.search(
        r"(?ms)^      - name: " + re.escape(STEP_IOS_HOST_PROBE)
        + r"$\n(.*?)(?=^      - (?:name|uses):|\Z)", ios_block)
    _require(probe is not None
             and "inputs.dependency-mode == 'qualify'" in probe.group(1)
             and "inputs.dependency-mode == 'publish'" in probe.group(1)
             and re.search(r"(?m)^          python3 scripts/ci_dependency_toolchain\.py \\\s*$",
                           probe.group(1))
             and re.search(r'(?m)^            --target-id "ios-\$\{KLOGG_IOS_ARCHITECTURE\}" \\$',
                           probe.group(1))
             and re.search(r"(?m)^            --probe-unreviewed-host-tools$", probe.group(1))
             and "continue-on-error" not in probe.group(1),
             "checked-out workflow lacks fail-closed iOS host tool observation")
    _require(re.search(r"(?m)^      - name: " + re.escape(STEP_ATTESTATION) + r"$",
                       jobs.get("BuildAdbLinuxX64", ""))
             and re.search(r"(?m)^      - name: " + re.escape(STEP_ATTESTATION) + r"$",
                           ios_block),
             "checked-out workflow lacks exact full tar attestation step")
    return result


def _validate_run(source: dict, run: dict) -> None:
    _require(isinstance(source, dict) and source.get("event_name") == "workflow_dispatch",
             "expected workflow_dispatch source")
    try:
        _source({key: value for key, value in source.items() if key != "event_name"},
                BUILDER_WORKFLOW)
    except ValueError as error:
        raise FullEvidenceError("invalid canonical source") from error
    _require(isinstance(run, dict)
             and type(run.get("id")) is int and run["id"] == source["run_id"]
             and type(run.get("run_attempt")) is int and run["run_attempt"] == source["run_attempt"]
             and run.get("head_sha") == source["sha"]
             and run.get("head_branch") == source["ref"][len("refs/heads/"):]
             and run.get("event") == "workflow_dispatch"
             and run.get("path") == BUILDER_WORKFLOW
             and isinstance(run.get("repository"), dict)
             and run["repository"].get("full_name") == REPOSITORY,
             "invalid parent Actions run")
    _require((run.get("status") == "in_progress" and run.get("conclusion") is None)
             or (run.get("status") == "completed" and run.get("conclusion") == "success"),
             "parent run has failed or has an unsupported status")


def _bound_steps(job: dict, *, ios: bool) -> None:
    steps = job.get("steps")
    _require(isinstance(steps, list), "missing Actions job step results")
    for name in ((STEP_ATTESTATION, STEP_IOS_PREFLIGHT, STEP_IOS_HOST_PROBE)
                 if ios else (STEP_ATTESTATION,)):
        selected = [step for step in steps if isinstance(step, dict) and step.get("name") == name]
        _require(len(selected) == 1 and selected[0].get("status") == "completed"
                 and selected[0].get("conclusion") == "success",
                 f"missing successful independent Actions step: {name}")


def _validate_jobs(jobs: list, needs: dict, source: dict, targets: dict, runners: dict) -> None:
    _require(isinstance(jobs, list) and isinstance(needs, dict)
             and set(needs) == set(BUILDER_JOBS.values()), "missing exact seven child needs")
    names = _job_names(targets)
    for target_id, display_name in names.items():
        matches = [job for job in jobs if isinstance(job, dict) and job.get("name") == display_name]
        _require(len(matches) == 1, f"missing/duplicated real Actions job: {display_name}")
        job = matches[0]
        _require(type(job.get("id")) is int and job["id"] > 0
                 and type(job.get("run_id")) is int and job["run_id"] == source["run_id"]
                 and type(job.get("run_attempt")) is int and job["run_attempt"] == source["run_attempt"]
                 and job.get("head_sha") == source["sha"]
                 and job.get("status") == "completed" and job.get("conclusion") == "success",
                 f"unqualified Actions job: {display_name}")
        labels = job.get("labels")
        _require(isinstance(labels, list) and runners[target_id] in labels
                 and "self-hosted" not in labels and isinstance(job.get("runner_name"), str)
                 and bool(job["runner_name"]), "job runner label differs from reviewed workflow")
        _bound_steps(job, ios=targets[target_id]["component"] == "ios-native")
        child = needs[BUILDER_JOBS[target_id]]
        _require(isinstance(child, dict) and child.get("result") == "success"
                 and isinstance(child.get("outputs"), dict), "missing successful child needs result")
    _require(len({job["id"] for job in jobs if isinstance(job, dict) and job.get("name") in names.values()}) == 7,
             "duplicate real Actions job IDs")


def _full_name(row: dict) -> str:
    return f"adb-helper-{row['target']}" if row["component"] == "adb-helper" else f"ios-native-{row['target']}"


def _verified_full_tar(subject: pathlib.Path, bundle: bytes, source: dict,
                       runner=subprocess.run) -> None:
    """gh verifies OIDC signature/signer; independently inspect the verified SLSA subject."""
    _require(isinstance(bundle, bytes) and 0 < len(bundle) <= MAX_BUNDLE_BYTES,
             "missing full tar signature bundle")
    bundle_path = subject.with_name("full-tar.sigstore.json")
    bundle_path.write_bytes(bundle)
    command = ["gh", "attestation", "verify", str(subject), "--bundle", str(bundle_path),
               "--repo", REPOSITORY, "--signer-workflow", REPOSITORY + "/" + BUILDER_WORKFLOW,
               "--signer-digest", source["sha"], "--source-digest", source["sha"],
               "--source-ref", source["ref"], "--deny-self-hosted-runners", "--format", "json"]
    try:
        result = runner(command, check=True, capture_output=True, text=True, timeout=90)
        _require(result.returncode == 0 and len(result.stdout) <= MAX_BUNDLE_BYTES,
                 "attestation verifier failed or exceeded output limit")
        records = _json(result.stdout)
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        raise FullEvidenceError("full tar signature verification failed") from error
    digest = hashlib.sha256(subject.read_bytes()).hexdigest()
    _require(isinstance(records, list) and any(
        isinstance(record, dict)
        and isinstance(record.get("verificationResult"), dict)
        and isinstance(record["verificationResult"].get("statement"), dict)
        and record["verificationResult"]["statement"].get("predicateType") == "https://slsa.dev/provenance/v1"
        and isinstance(record["verificationResult"]["statement"].get("subject"), list)
        and any(isinstance(item, dict) and item.get("name") == subject.name
                and item.get("digest") == {"sha256": digest}
                for item in record["verificationResult"]["statement"]["subject"])
        for record in records), "signed statement lacks exact full tar name and digest")


def _unpack_full(raw: bytes, root: pathlib.Path) -> None:
    """Extract only bounded regular files, directories, and direct iOS dylib aliases."""
    _require(isinstance(raw, bytes) and 0 < len(raw) <= MAX_FULL_TAR_BYTES,
             "missing or oversized full archive bytes")
    try:
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as archive:
            members = archive.getmembers()
            _require(0 < len(members) <= MAX_MEMBERS, "invalid full tar member count")
            files, dirs, links = {}, set(), {}
            size = 0
            for member in members:
                name = member.name[2:] if member.name.startswith("./") else member.name
                if name in ("", ".") and member.isdir():
                    continue
                parts = name.split("/")
                _require(name and not name.startswith("/") and "\\" not in name
                         and all(part not in ("", ".", "..") for part in parts)
                         and not any(ord(character) < 32 for character in name)
                         and name not in files and name not in dirs and name not in links,
                         "unsafe or duplicate full tar member")
                if member.isdir():
                    dirs.add(name)
                elif member.isfile():
                    size += member.size
                    _require(member.size >= 0 and size <= MAX_UNPACKED_BYTES,
                             "oversized full tar extraction")
                    files[name] = member
                elif member.issym():
                    _require(name.startswith("lib/") and name.endswith(".dylib")
                             and posixpath.basename(name) == name.split("/")[-1]
                             and member.linkname == posixpath.basename(member.linkname)
                             and member.linkname not in ("", ".", ".."),
                             "unsafe or unexpected full tar symlink")
                    links[name] = member
                else:
                    raise FullEvidenceError("unsupported full tar entry type")
            nondirectories = set(files) | set(links)
            _require(all("/".join(name.split("/")[:index]) not in nondirectories
                         for name in set(files) | dirs | set(links)
                         for index in range(1, len(name.split("/")))),
                     "full tar parent is a file or symlink")
            for name in dirs:
                (root / name).mkdir(parents=True, exist_ok=True)
            for name, member in files.items():
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                source = archive.extractfile(member)
                _require(source is not None, "unreadable full tar file")
                with source, destination.open("xb") as output:
                    remaining = member.size
                    while remaining:
                        chunk = source.read(min(1024 * 1024, remaining))
                        _require(bool(chunk), "truncated full tar member")
                        output.write(chunk)
                        remaining -= len(chunk)
                destination.chmod(member.mode & 0o777)
            for name, member in links.items():
                target = posixpath.join(posixpath.dirname(name), member.linkname)
                _require(target in files and target.startswith("lib/")
                         and name != target, "dangling or chained full tar symlink")
                (root / name).symlink_to(member.linkname)
    except (tarfile.TarError, EOFError, OSError, OverflowError) as error:
        raise FullEvidenceError("invalid full tar archive") from error


def _checksum_full(root: pathlib.Path) -> None:
    from verify_adb_helper_envelope import verify_checksum_file
    try:
        entries = verify_checksum_file(root / "SHA256SUMS")
    except (OSError, UnicodeError, ValueError, RuntimeError) as error:
        raise FullEvidenceError("full archive checksum envelope failed") from error
    files = {path.relative_to(root).as_posix() for path in root.rglob("*")
             if path.is_file() and not path.is_symlink() and path.name != "SHA256SUMS"}
    _require(set(entries) == files, "full tar has missing/unlisted checksums")


def _verify_full_metadata(root: pathlib.Path, repo_root: pathlib.Path, row: dict,
                          run_verifier) -> dict | None:
    if row["component"] == "adb-helper":
        verify_artifact_envelope(repo_root / "packaging/adb/adb-helper.lock.json", root, row["target"])
        return None
    lock_path = repo_root / "3rdparty/libimobiledevice/libimobiledevice.lock.json"
    lock = read_json(lock_path, "iOS native lock")
    receipt = read_json(root / "ios-native-build-receipt.json", "iOS native build receipt")
    validate_build_receipt(lock, lock_path, receipt, row["target"])
    command = [sys.executable, str(repo_root / "scripts/verify_ios_native_stack.py"),
               "--lock", str(lock_path), "--stack-root", str(root),
               "--architecture", row["target"], "--receipt", str(root / "ios-native-build-receipt.json"),
               "--asset-scope", "release", "--source-assets-root", str(root),
               "--source-receipt", str(root / "ios-native-source-set-receipt.json"),
               "--legal-receipt", str(root / "ios-native-legal-receipt.json"),
               "--sbom", str(root / "ios-native-sbom.spdx.json")]
    try:
        result = run_verifier(command, capture_output=True, text=True, timeout=300)
    except (OSError, subprocess.SubprocessError) as error:
        raise FullEvidenceError("iOS native full verifier failed") from error
    _require(result is not None and result.returncode == 0, "iOS native full verifier rejected archive")
    return receipt


def _core_entries(raw: bytes, path: pathlib.Path, row: dict, core_identity: str) -> dict:
    _require(isinstance(raw, bytes), "missing candidate core bytes")
    path.write_bytes(raw)
    verify_core(path, component=row["component"], target=row["target"],
                core_identity=core_identity, expected_sha256=hashlib.sha256(raw).hexdigest(),
                expected_size=len(raw))
    with tarfile.open(path, "r:gz") as archive:
        result = {}
        for member in archive:
            if member.name == MANIFEST:
                continue
            if member.isfile():
                source = archive.extractfile(member)
                _require(source is not None, "unreadable core binary")
                with source:
                    result[member.name] = ("file", member.mode, source.read())
            else:
                result[member.name] = ("symlink", member.mode, member.linkname)
        return result


def _compare_binary_closure(root: pathlib.Path, row: dict, entries: dict) -> None:
    closure = "helpers" if row["component"] == "adb-helper" else "lib"
    observed = {}
    for path in (root / closure).iterdir():
        name = closure + "/" + path.name
        if path.is_symlink():
            observed[name] = ("symlink", 0o777, os.readlink(path))
        elif path.is_file():
            mode = path.stat().st_mode & 0o777
            if row["component"] == "adb-helper":
                mode = canonical_adb_mode(name, mode, row["target"])
            observed[name] = ("file", mode, path.read_bytes())
        else:
            raise FullEvidenceError("untracked full artifact binary closure")
    _require(observed == entries, "candidate binary bytes, file modes or symlinks differ from full tar")


def verify_full_evidence(*, repo_root: pathlib.Path, source: dict, run: dict,
                         attempt_jobs: list, needs: dict, artifacts: list,
                         full_downloads: dict, candidate_archives: dict, bundles: dict,
                         catalog: dict, workflow_text: str,
                         run_verifier=subprocess.run, attestation_runner=subprocess.run) -> dict:
    """Verify all seven before emitting runner/toolchain data for validate_evidence.

    Caller must establish checkout SHA and child `needs` provenance. No signing,
    publication, or production lock is performed. ADB legal assets are not
    authenticated here and must be separately verified before publication.
    """
    try:
        targets = validate_catalog(catalog)
    except ValueError as error:
        raise FullEvidenceError("invalid reviewed catalog") from error
    _require(set(targets) == set(BUILDER_JOBS), "expected exactly seven native targets")
    _validate_run(source, run)
    runners = reviewed_runners(workflow_text, targets)
    _validate_jobs(attempt_jobs, needs, source, targets, runners)
    _require(isinstance(artifacts, list) and len(artifacts) == 7
             and isinstance(full_downloads, dict) and len(full_downloads) == 7
             and isinstance(candidate_archives, dict) and set(candidate_archives) == set(targets)
             and isinstance(bundles, dict) and set(bundles) == set(targets),
             "full evidence must cover seven distinct artifacts and signatures")
    by_id = {}
    for artifact in artifacts:
        _require(isinstance(artifact, dict) and type(artifact.get("id")) is int
                 and artifact["id"] > 0 and artifact["id"] not in by_id
                 and artifact.get("expired") is False
                 and isinstance(artifact.get("workflow_run"), dict)
                 and type(artifact["workflow_run"].get("id")) is int
                 and artifact["workflow_run"]["id"] == source["run_id"]
                 and artifact["workflow_run"].get("head_sha") == source["sha"]
                 and artifact["workflow_run"].get("head_branch") == source["ref"][len("refs/heads/"):],
                 "full artifact ID or run ancestry mismatch")
        by_id[artifact["id"]] = artifact
    _require(set(full_downloads) == set(by_id), "full archive downloads do not match artifact IDs")
    try:
        identities = compute_identities(pathlib.Path(repo_root), catalog)
    except (OSError, ValueError) as error:
        raise FullEvidenceError("cannot recompute checked-out core identities") from error
    result = {}
    ids = set()
    with tempfile.TemporaryDirectory(prefix="native-full-evidence-") as temporary:
        base = pathlib.Path(temporary)
        for target_id, row in sorted(targets.items()):
            output = needs[BUILDER_JOBS[target_id]]["outputs"].get("full_artifact_id")
            _require(isinstance(output, str) and DECIMAL.fullmatch(output) is not None,
                     "missing independent full_artifact_id output")
            identifier = int(output)
            _require(identifier not in ids and identifier in by_id
                     and by_id[identifier].get("name") == _full_name(row),
                     "full artifact substituted or named for another target")
            ids.add(identifier)
            full_tar = full_downloads[identifier]
            _require(isinstance(full_tar, bytes) and 0 < len(full_tar) <= MAX_FULL_TAR_BYTES,
                     "missing full tar bytes")
            tar_name = _full_name(row) + ".tar.gz"
            work = base / target_id
            work.mkdir()
            subject = work / tar_name
            subject.write_bytes(full_tar)
            _verified_full_tar(subject, bundles[target_id], source, attestation_runner)
            full_root = work / "full"
            full_root.mkdir()
            try:
                _unpack_full(full_tar, full_root)
                _checksum_full(full_root)
                receipt = _verify_full_metadata(full_root, pathlib.Path(repo_root), row, run_verifier)
                if row["component"] == "ios-native":
                    _require(isinstance(receipt, dict) and receipt.get("toolchain") == row["toolchain"],
                             "iOS observed build receipt differs from pinned preflight toolchain")
                core_identity = identities[target_id]["core_identity"]
                core = _core_entries(candidate_archives[target_id], work / "candidate.tar.gz", row,
                                     core_identity)
                _compare_binary_closure(full_root, row, core)
            except (OSError, ValueError, RuntimeError, tarfile.TarError) as error:
                raise FullEvidenceError(f"unqualified full build or core: {target_id}") from error
            result[target_id] = {"runner": runners[target_id]}
            if row["component"] == "ios-native":
                result[target_id]["toolchain"] = row["toolchain"]
    _require(ids == set(by_id), "unexpected full artifacts")
    return result
