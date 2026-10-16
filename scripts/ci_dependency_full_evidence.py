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

import argparse
import contextlib
import hashlib
import io
import os
import pathlib
import posixpath
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile

import build_ios_native_legal_assets as ios_legal
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
    """Extract bounded files/aliases; directories must permit owned inspection and cleanup."""
    _require(isinstance(raw, bytes) and 0 < len(raw) <= MAX_FULL_TAR_BYTES,
             "missing or oversized full archive bytes")
    try:
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as archive:
            members = archive.getmembers()
            _require(0 < len(members) <= MAX_MEMBERS, "invalid full tar member count")
            files, dirs, links = {}, {}, {}
            size, root_mode = 0, None
            for member in members:
                name = member.name[2:] if member.name.startswith("./") else member.name
                if name in ("", ".") and member.isdir():
                    _require(root_mode is None, "duplicate full tar root metadata")
                    _require(not member.mode & 0o7000 and member.mode & 0o700 == 0o700,
                             "unsupported full tar directory permissions")
                    root_mode = member.mode & 0o777
                    continue
                parts = name.split("/")
                _require(name and not name.startswith("/") and "\\" not in name
                         and all(part not in ("", ".", "..") for part in parts)
                         and not any(ord(character) < 32 for character in name)
                         and name not in files and name not in dirs and name not in links,
                         "unsafe or duplicate full tar member")
                if member.isdir():
                    _require(not member.mode & 0o7000 and member.mode & 0o700 == 0o700,
                             "unsupported full tar directory permissions")
                    dirs[name] = member.mode & 0o777
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
                         for name in set(files) | set(dirs) | set(links)
                         for index in range(1, len(name.split("/")))),
                     "full tar parent is a file or symlink")
            for name in dirs:
                (root / name).mkdir(parents=True, exist_ok=True)
            with contextlib.ExitStack() as handles:
                directory_modes = dict(dirs)
                if root_mode is not None:
                    directory_modes[""] = root_mode
                directory_handles = {}
                if os.name == "posix":
                    for name in directory_modes:
                        handle = os.open(root / name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                        handles.callback(os.close, handle)
                        directory_handles[name] = handle
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
                        if os.name == "posix":
                            os.fchmod(output.fileno(), member.mode & 0o777)
                        else:
                            destination.chmod(member.mode & 0o777)
                for name, member in links.items():
                    target = posixpath.join(posixpath.dirname(name), member.linkname)
                    _require(target in files and target.startswith("lib/")
                             and name != target, "dangling or chained full tar symlink")
                    (root / name).symlink_to(member.linkname)
                for name in sorted(directory_handles, key=len, reverse=True):
                    handle = directory_handles[name]
                    owned, current = os.fstat(handle), (root / name).lstat()
                    _require((owned.st_dev, owned.st_ino) == (current.st_dev, current.st_ino),
                             "full tar directory identity changed")
                    os.fchmod(handle, directory_modes[name])
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


@contextlib.contextmanager
def project_authenticated_full_artifacts(*, repo_root: pathlib.Path, source: dict, run: dict,
                                         attempt_jobs: list, needs: dict, artifacts: list,
                                         full_downloads: dict, candidate_archives: dict, bundles: dict,
                                         catalog: dict, workflow_text: str,
                                         run_verifier=subprocess.run,
                                         attestation_runner=subprocess.run):
    """Yield scheduler evidence and original full roots after all seven pass.

    The fresh private roots and original tar snapshots are owned by
    this context and removed on exit while base ownership remains intact; a
    foreign replacement is left untouched. Headers and support material come only
    from the exact signature-verified full tar, never the binary-only core.
    Caller must establish checkout SHA and child `needs` provenance. This is
    not production-core qualification, signing, publication, or a production
    lock. Separate ADB legal assets remain unauthenticated here.
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
    checkout = pathlib.Path(repo_root).resolve()
    base = pathlib.Path(tempfile.mkdtemp(prefix="native-full-evidence-"))
    resolved_base = base.resolve()
    # Existing fixed recipe inputs were read above; archives are in memory.
    # A fresh checkout descendant cannot overlap them. Never take cleanup
    # authority over the checkout itself or an ancestor, even on rejection.
    _require(resolved_base != checkout and resolved_base not in checkout.parents,
             "full projection must not contain checkout")
    base_stat = base.lstat()

    def owns_base():
        try:
            current = base.lstat()
            return (stat.S_ISDIR(current.st_mode) and base.resolve() == resolved_base
                    and (current.st_dev, current.st_ino) == (base_stat.st_dev, base_stat.st_ino))
        except OSError:
            return False

    try:
        def require_owned_directories(*paths):
            _require(owns_base() and all(path.is_dir() and not path.is_symlink() for path in paths),
                     "full projection directory ownership changed")

        def full_links(root):
            return {path.relative_to(root).as_posix(): os.readlink(path)
                    for path in root.rglob("*") if path.is_symlink()}

        def full_modes(root):
            return {path.relative_to(root).as_posix(): stat.S_IMODE(path.lstat().st_mode)
                    for path in (root, *root.rglob("*")) if not path.is_symlink()}

        def require_subject(subject, raw):
            _require(subject.is_file() and not subject.is_symlink() and subject.read_bytes() == raw,
                     "signed full tar snapshot changed")

        roots, snapshots = {}, []
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
            require_owned_directories(work)
            require_subject(subject, full_tar)
            full_root = work / "full"
            full_root.mkdir()
            try:
                _unpack_full(full_tar, full_root)
                _checksum_full(full_root)
                checksum_bytes = (full_root / "SHA256SUMS").read_bytes()
                links = full_links(full_root)
                modes = full_modes(full_root)
                receipt = _verify_full_metadata(full_root, pathlib.Path(repo_root), row, run_verifier)
                require_owned_directories(work, full_root)
                require_subject(subject, full_tar)
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
            roots[target_id] = full_root
            snapshots.append((target_id, row, work, subject, full_tar, full_root, checksum_bytes, links, modes, core))
        _require(ids == set(by_id), "unexpected full artifacts")
        # External runners have now finished. Recheck the original signed
        # subject/envelope, authenticated links, and existing content validators
        # before exposing roots. Checksum envelopes intentionally omit aliases.
        for target_id, row, work, subject, raw, full_root, checksum_bytes, links, modes, core in snapshots:
            try:
                require_owned_directories(work, full_root)
                require_subject(subject, raw)
                envelope = full_root / "SHA256SUMS"
                _require(not envelope.is_symlink() and envelope.read_bytes() == checksum_bytes,
                         "original full checksum envelope changed")
                _require(full_links(full_root) == links, "original full archive symlinks changed")
                _require(full_modes(full_root) == modes, "original full archive metadata changed")
                _checksum_full(full_root)
                _compare_binary_closure(full_root, row, core)
            except (OSError, ValueError, RuntimeError, tarfile.TarError) as error:
                raise FullEvidenceError(f"unqualified full build or core: {target_id}") from error
        yield result, roots
    finally:
        # A runner or caller may have moved our base and installed a foreign
        # directory at its old path. Never clean up that replacement.
        if owns_base():
            shutil.rmtree(base)


def assemble_authenticated_ios_stacks(*, repo_root: pathlib.Path, source: dict, run: dict,
                                      attempt_jobs: list, needs: dict, artifacts: list,
                                      full_downloads: dict, candidate_archives: dict, bundles: dict,
                                      catalog: dict, workflow_text: str, outputs: dict,
                                      version: str, base_url: str, run_verifier=subprocess.run,
                                      attestation_runner=subprocess.run) -> dict:
    """Assemble new iOS overlays only after the existing seven full checks pass.

    This does not qualify production cores or authenticate separate ADB legal
    assets. Outputs must be fresh and caller-owned; failures retain partial
    outputs. No native payload runs or source acquisition occur during assembly.
    """
    _require(isinstance(outputs, dict) and set(outputs) == {"ios-arm64", "ios-x86_64"}
             and all(isinstance(path, (str, pathlib.Path)) for path in outputs.values()),
             "assembly requires exact two iOS outputs")
    destinations = {target: pathlib.Path(path) for target, path in outputs.items()}
    first, second = (path.resolve() for path in destinations.values())
    _require(first != second and first not in second.parents and second not in first.parents,
             "iOS assembly outputs must not overlap")
    version = ios_legal.validate_version(version)
    base_url = ios_legal.normalize_base_url(base_url)
    with project_authenticated_full_artifacts(
            repo_root=repo_root, source=source, run=run, attempt_jobs=attempt_jobs,
            needs=needs, artifacts=artifacts, full_downloads=full_downloads,
            candidate_archives=candidate_archives, bundles=bundles, catalog=catalog,
            workflow_text=workflow_text, run_verifier=run_verifier,
            attestation_runner=attestation_runner) as (result, roots):
        # Outputs may not live under the context-owned base that will be removed.
        bases = tuple({root.parent.parent for root in roots.values()})
        for output in destinations.values():
            ios_legal.check_output(output, bases)
            _require(not output.exists(), "iOS assembly output must not already exist")
        lock_path = pathlib.Path(repo_root) / "3rdparty/libimobiledevice/libimobiledevice.lock.json"
        lock = read_json(lock_path, "iOS native lock")
        for target, output in sorted(destinations.items()):
            root = roots[target]
            args = argparse.Namespace(
                lock=lock_path, stack_root=root, core_root=root, output=output,
                source_set_sha256=ios_legal.sha256(ios_legal.checked_input(root, ios_legal.SOURCE_SET_RECEIPT)),
                legal_receipt_sha256=ios_legal.sha256(ios_legal.checked_input(root, lock["receipts"]["legal"])),
                build_receipt_sha256=ios_legal.sha256(ios_legal.checked_input(root, lock["receipts"]["build"])))
            ios_legal.assemble_original_stack(args, lock, version, base_url, full_release=True)
        return result


def verify_full_evidence(*, repo_root: pathlib.Path, source: dict, run: dict,
                         attempt_jobs: list, needs: dict, artifacts: list,
                         full_downloads: dict, candidate_archives: dict, bundles: dict,
                         catalog: dict, workflow_text: str,
                         run_verifier=subprocess.run, attestation_runner=subprocess.run) -> dict:
    """Return existing scheduler/toolchain evidence without retaining full roots."""
    with project_authenticated_full_artifacts(
            repo_root=repo_root, source=source, run=run, attempt_jobs=attempt_jobs,
            needs=needs, artifacts=artifacts, full_downloads=full_downloads,
            candidate_archives=candidate_archives, bundles=bundles, catalog=catalog,
            workflow_text=workflow_text, run_verifier=run_verifier,
            attestation_runner=attestation_runner) as (result, _):
        return result
