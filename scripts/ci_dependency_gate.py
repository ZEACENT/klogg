#!/usr/bin/env python3
"""Read-only Actions transport for seven signed full builds and candidate cores.

The caller must pass the real GitHub context and GitHub-controlled toJSON(needs).
The REST jobs endpoint cannot supply job outputs. A token with actions:read does
not necessarily have attestations:read: absence of full-tar bundles is a blocker.
Neither a candidate nor an artifact name is independent runner/toolchain evidence.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile

from ci_dependency_catalog import validate_catalog
from ci_dependency_full_evidence import (MAX_FULL_TAR_BYTES, _unpack_full,
                                         _validate_jobs, reviewed_runners,
                                         verify_full_evidence)
from ci_dependency_pipeline import BUILDER_JOBS, qualify_from_repo
from ci_dependency_lock import _source
from ci_environment_registry import REPOSITORY, _json
from verify_adb_helper_artifact import (validate_overlay_receipt, validate_source_set_receipt,
                                        read_json, verify_hash_sidecar)

MAX_CANDIDATE_BYTES = 1024 * 1024
MAX_ZIP_BYTES = MAX_FULL_TAR_BYTES + 1024 * 1024
MAX_SUPPORT_BYTES = 2 * 1024**3
MAX_API_BYTES = 10 * 1024 * 1024
MAX_BUNDLE_BYTES = 10 * 1024 * 1024
MAX_GATE_ARCHIVE_BYTES = 1024 * 1024
LEGAL_JOB = "BuildAdbHelperLegalAssets"


class GateError(ValueError):
    """GitHub context, native evidence or transport failed closed."""


def require(condition, reason):
    if not condition:
        raise GateError(reason)


class ActionsAPI:
    """Bounded authenticated GitHub REST reads; never forward credentials off host."""

    def __init__(self, token: str):
        require(bool(token), "missing GH_TOKEN with actions:read")
        self.token = token

    def get_bytes(self, endpoint: str, limit: int) -> bytes:
        require(endpoint.startswith("/") and not endpoint.startswith("//"), "invalid API endpoint")
        url = "https://api.github.com/repos/" + REPOSITORY + endpoint
        for redirect in range(4):
            parsed = urllib.parse.urlsplit(url)
            require(parsed.scheme == "https" and bool(parsed.hostname), "unsafe artifact redirect")
            headers = {"Accept": "application/vnd.github+json",
                       "X-GitHub-Api-Version": "2022-11-28"}
            if parsed.hostname == "api.github.com":
                headers["Authorization"] = "Bearer " + self.token
            request = urllib.request.Request(url, headers=headers)
            try:
                with urllib.request.build_opener(_NoRedirect()).open(request, timeout=90) as response:
                    require(response.status == 200, "Actions API did not return success")
                    raw = response.read(limit + 1)
                    require(len(raw) <= limit, "oversized Actions API response")
                    return raw
            except urllib.error.HTTPError as error:
                if error.code not in (301, 302, 303, 307, 308) or redirect == 3:
                    raise GateError(f"Actions API unavailable (HTTP {error.code}): {endpoint}") from error
                location = error.headers.get("Location")
                require(bool(location), "artifact redirect missing location")
                url = urllib.parse.urljoin(url, location)
            except (OSError, urllib.error.URLError) as error:
                raise GateError(f"Actions API transport failed: {endpoint}") from error
        raise GateError("too many Actions API redirects")

    def get_json(self, endpoint: str) -> dict:
        try:
            result = _json(self.get_bytes(endpoint, MAX_API_BYTES))
        except (ValueError, UnicodeError) as error:
            raise GateError("invalid Actions API JSON") from error
        require(isinstance(result, dict), "Actions API did not return an object")
        return result


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, new_url):
        return None


def checkout_head(repo_root: pathlib.Path) -> str:
    try:
        result = subprocess.run(["git", "-C", str(repo_root), "rev-parse", "HEAD"],
                                check=True, capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError) as error:
        raise GateError("cannot verify checked-out HEAD") from error
    return result.stdout.strip()


def _list(api, endpoint: str, field: str) -> list:
    collected = []
    for page in range(1, 22):
        response = api.get_json(f"{endpoint}?per_page=100&page={page}")
        items, total = response.get(field), response.get("total_count")
        require(isinstance(items, list) and type(total) is int and 0 <= total <= 2000
                and len(items) <= 100 and len(collected) + len(items) <= total,
                "malformed or oversized paginated Actions API results")
        collected.extend(items)
        if len(collected) == total:
            return collected
        require(len(items) == 100, "missing paginated Actions API results")
    raise GateError("Actions API pagination exceeded bound")


def _zip_files(raw: bytes, expected: dict[str, int]) -> dict[str, bytes]:
    require(isinstance(raw, bytes) and 0 < len(raw) <= MAX_ZIP_BYTES, "missing or oversized artifact ZIP")
    try:
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            members = archive.infolist()
            require(len(members) == len(expected), "artifact ZIP has missing or unexpected members")
            result = {}
            for info in members:
                name = info.filename
                require(name in expected and name not in result and not info.is_dir()
                        and name not in (".", "..") and "\\" not in name
                        and all(part not in ("", ".", "..") for part in name.split("/"))
                        and not name.startswith("/")
                        and not (info.external_attr >> 16) & 0o170000 == 0o120000,
                        "unsafe, duplicate or unexpected artifact ZIP member")
                limit = expected[name]
                require(0 < info.file_size <= limit, "oversized artifact ZIP member")
                with archive.open(info) as source:
                    data = source.read(limit + 1)
                require(len(data) == info.file_size and len(data) <= limit,
                        "truncated or oversized artifact ZIP member")
                result[name] = data
            require(set(result) == set(expected), "artifact ZIP lacks expected files")
            return result
    except (zipfile.BadZipFile, OSError, EOFError, RuntimeError) as error:
        raise GateError("invalid artifact ZIP") from error


def _artifact(api, identifier: str, name: str, source: dict) -> dict:
    require(isinstance(identifier, str) and identifier.isascii() and identifier.isdecimal()
            and identifier[0] != "0", "missing immutable artifact ID")
    metadata = api.get_json(f"/actions/artifacts/{identifier}")
    ancestor = metadata.get("workflow_run")
    require(type(metadata.get("id")) is int and metadata["id"] == int(identifier)
            and metadata.get("name") == name and metadata.get("expired") is False
            and isinstance(ancestor, dict) and type(ancestor.get("id")) is int
            and ancestor["id"] == source["run_id"] and ancestor.get("head_sha") == source["sha"]
            and ancestor.get("head_branch") == source["ref"][len("refs/heads/"):],
            "artifact ID, name or source ancestry differs")
    return metadata


def fetch_attestation_bundle(api, full_tar: bytes, source: dict) -> bytes:
    """Fetch an actual repository Sigstore bundle for the exact full tar digest."""
    digest = hashlib.sha256(full_tar).hexdigest()
    endpoint = "/attestations/sha256:" + digest + "?predicate_type=provenance&per_page=100"
    try:
        response = api.get_json(endpoint)
    except (OSError, ValueError) as error:
        raise GateError("full tar attestation API unavailable (attestations:read may be required)") from error
    entries = response.get("attestations")
    require(isinstance(entries, list) and bool(entries) and len(entries) <= 100,
            "full tar attestation API unavailable or missing (attestations:read may be required)")
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        bundle = entry.get("bundle")
        if bundle is None and isinstance(entry.get("bundle_url"), str):
            url = urllib.parse.urlsplit(entry["bundle_url"])
            prefix = "/repos/" + REPOSITORY + "/attestations/"
            require(url.scheme == "https" and url.hostname == "api.github.com"
                    and url.path.startswith(prefix) and not url.query and not url.fragment
                    and url.path[len(prefix):].isascii() and url.path[len(prefix):].isdecimal(),
                    "untrusted attestation bundle URL")
            payload = api.get_json(url.path[len("/repos/" + REPOSITORY):])
            bundle = payload.get("bundle", payload)
        if isinstance(bundle, dict):
            raw = (json.dumps(bundle, sort_keys=True, separators=(",", ":")) + "\n").encode()
            require(len(raw) <= MAX_BUNDLE_BYTES, "oversized full tar Sigstore bundle")
            return raw
    raise GateError("full tar attestation bundle not delivered by API (attestations:read may be required)")


def _support_evidence(api, needs: dict, source: dict, jobs: list, workflow_text: str) -> dict:
    legal = needs.get(LEGAL_JOB)
    require(isinstance(legal, dict) and legal.get("result") == "success"
            and isinstance(legal.get("outputs"), dict),
            "missing successful independently trusted legal assets job")
    matches = [job for job in jobs if isinstance(job, dict)
               and job.get("name") == "Build ADB helper legal assets"]
    block = re.search(r"(?ms)^  BuildAdbHelperLegalAssets:\s*\n(.*?)(?=^  [A-Za-z][A-Za-z0-9_]*:|\Z)",
                      workflow_text)
    require(block is not None and re.findall(r"(?m)^    runs-on: (.*)$", block.group(1))
            == ["ubuntu-24.04"], "untrusted checked-out legal-assets runner policy")
    require(len(matches) == 1 and type(matches[0].get("run_id")) is int
            and matches[0]["run_id"] == source["run_id"]
            and matches[0].get("run_attempt") == source["run_attempt"]
            and matches[0].get("head_sha") == source["sha"]
            and matches[0].get("status") == "completed"
            and matches[0].get("conclusion") == "success"
            and isinstance(matches[0].get("labels"), list)
            and "ubuntu-24.04" in matches[0]["labels"]
            and "self-hosted" not in matches[0]["labels"]
            and isinstance(matches[0].get("runner_name"), str)
            and bool(matches[0]["runner_name"]), "legal assets job is untrusted")
    outputs = legal["outputs"]
    ids = (outputs.get("support_artifact_id"), outputs.get("full_release_artifact_id"))
    require(all(isinstance(value, str) and value.isascii() and value.isdecimal()
                and value[0] != "0" for value in ids) and ids[0] != ids[1],
            "missing immutable support artifact ID or full release artifact ID")
    for identifier, name in zip(ids, ("adb-helper-package-support", "adb-helper-legal-assets")):
        _artifact(api, identifier, name, source)
    return {"support": ids[0], "release": ids[1]}


def _legal_closure(api, ids: dict, repo_root: pathlib.Path,
                   full_tars: dict, candidate_downloads: dict) -> dict:
    """Recheck support projection against full legal archive and ADB build receipts."""
    from verify_adb_helper_artifact import sha256 as file_sha
    # Asset inventory is owned by the checked-out source lock. Never infer
    # legal artifact IDs from a name search or candidate-reported metadata.
    lock = read_json(repo_root / "packaging/adb/adb-helper.lock.json", "ADB helper lock")
    assets = lock.get("release_assets")
    require(isinstance(assets, list) and bool(assets), "missing locked ADB legal inventory")
    release_names = set()
    support_names = set()
    for asset in assets:
        if isinstance(asset, dict) and isinstance(asset.get("file_name"), str):
            release_names.add(asset["file_name"])
            if isinstance(asset.get("sha256_file"), str):
                release_names.add(asset["sha256_file"])
            if isinstance(asset.get("distribution"), dict) and asset["distribution"].get("package_required") is True:
                support_names.add(asset["file_name"])
                if isinstance(asset.get("sha256_file"), str):
                    support_names.add(asset["sha256_file"])
    require(bool(support_names) and support_names <= release_names,
            "invalid locked ADB legal package projection")
    release_names.add("adb-helper-release-assets.json")
    # Directories (e.g. licenses) are variable; their exact inventory is
    # authenticated through legal receipts and the existing artifact verifier.
    with tempfile.TemporaryDirectory(prefix="native-adb-legal-") as temporary:
        base = pathlib.Path(temporary)
        release_zip = api.get_bytes(f"/actions/artifacts/{ids['release']}/zip", MAX_ZIP_BYTES)
        support_zip = api.get_bytes(f"/actions/artifacts/{ids['support']}/zip", MAX_ZIP_BYTES)
        release = _safe_variable_zip(release_zip, base / "release", MAX_SUPPORT_BYTES)
        support = _safe_variable_zip(support_zip, base / "support", MAX_SUPPORT_BYTES)
        require(set(release) == release_names and set(support) == support_names
                and all(support[name] == release[name] for name in support),
                "ADB package support differs from full legal source closure")
        records = []
        for asset in assets:
            name, sidecar = asset["file_name"], asset["sha256_file"]
            digest = file_sha(base / "release" / name)
            verify_hash_sidecar(base / "release" / sidecar, digest, pathlib.Path(name).name)
            records.append({"kind": asset["kind"], "path": pathlib.Path(name).name,
                            "sha256": digest})
        require(_json(release["adb-helper-release-assets.json"]) == records,
                "ADB release inventory receipt differs from checked-out lock and bytes")
        for target_id, full_raw in full_tars.items():
            if not target_id.startswith("adb-"):
                continue
            work = base / target_id
            work.mkdir()
            _unpack_full(full_raw, work)
            receipt = read_json(work / "receipt.json", "ADB full build receipt")
            lock_path = repo_root / "packaging/adb/adb-helper.lock.json"
            source_hash = validate_source_set_receipt(lock, lock_path, receipt, base / "release", "release")
            validate_overlay_receipt(lock, base / "release", "release", source_hash)
            package_hash = validate_source_set_receipt(lock, lock_path, receipt, base / "support", "package")
            validate_overlay_receipt(lock, base / "support", "package", package_hash)
            require(source_hash == package_hash == receipt.get("source_set_receipt_sha256"),
                    "ADB legal source receipt differs from signed full build")
            candidate = _json(candidate_downloads[target_id]["candidate"])
            qualification = candidate.get("qualification")
            require(isinstance(qualification, dict) and qualification.get("legacy_package_receipt_sha256")
                    == file_sha(work / "package-verification.json"),
                    "ADB candidate package qualification differs from signed full build")
            verified = work / "rechecked-package-verification.json"
            helper = "adb.exe" if target_id == "adb-windows-x86_64" else "adb"
            command = [sys.executable, str(repo_root / "scripts/verify_adb_helper_artifact.py"),
                       "--lock", str(lock_path), "--receipt", str(work / "receipt.json"),
                       "--binary-smoke-receipt", str(work / "package-smoke.json"),
                       "--package-root", str(work), "--asset-scope", "package",
                       "--source-assets-root", str(base / "support"),
                       "--helper-path", "helpers/" + helper,
                       "--expected-target", target_id[len("adb-"):],
                       "--package-verification-receipt", str(verified), "--require-lock-binding"]
            try:
                result = subprocess.run(command, capture_output=True, text=True, timeout=300)
            except (OSError, subprocess.SubprocessError) as error:
                raise GateError("ADB offline package requalification unavailable") from error
            require(result.returncode == 0 and verified.is_file()
                    and file_sha(verified) == qualification.get("rechecked_package_receipt_sha256"),
                    "ADB offline package requalification differs from candidate")
        by_kind = {asset["kind"]: asset for asset in assets}
        require(all(kind in by_kind for kind in
                    ("source-set-receipt", "overlay-receipt", "source-archive")),
                "missing ADB source, overlay or corresponding archive")
        source_asset = by_kind["source-set-receipt"]
        overlay_asset = by_kind["overlay-receipt"]
        archive_asset = by_kind["source-archive"]
        return {"support_artifact_id": int(ids["support"]),
                "support_zip_sha256": hashlib.sha256(support_zip).hexdigest(),
                "full_release_artifact_id": int(ids["release"]),
                "full_release_zip_sha256": hashlib.sha256(release_zip).hexdigest(),
                "source_set_receipt_sha256": file_sha(base / "release" / source_asset["file_name"]),
                "overlay_receipt_sha256": file_sha(base / "release" / overlay_asset["file_name"]),
                "source_archive_sha256": file_sha(base / "release" / archive_asset["file_name"])}


def _safe_variable_zip(raw: bytes, root: pathlib.Path, limit: int) -> dict[str, bytes]:
    require(0 < len(raw) <= MAX_ZIP_BYTES, "missing legal artifact ZIP")
    try:
        with zipfile.ZipFile(io.BytesIO(raw)) as archive:
            members = archive.infolist()
            require(0 < len(members) <= 8192, "invalid legal ZIP member count")
            files, total = {}, 0
            for member in members:
                name = member.filename
                require(not name.startswith("/") and "\\" not in name
                        and all(part not in ("", ".", "..") for part in name.rstrip("/").split("/"))
                        and not (member.external_attr >> 16) & 0o170000 == 0o120000,
                        "unsafe legal ZIP member")
                if member.is_dir():
                    continue
                require(name not in files and member.file_size > 0, "duplicate or empty legal ZIP member")
                total += member.file_size
                require(total <= limit, "oversized legal ZIP contents")
                with archive.open(member) as input_file:
                    data = input_file.read(member.file_size + 1)
                require(len(data) == member.file_size, "invalid legal ZIP size")
                files[name] = data
            for name, data in files.items():
                path = root.joinpath(*name.split("/"))
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            return files
    except (zipfile.BadZipFile, OSError, EOFError, RuntimeError) as error:
        raise GateError("invalid legal artifact ZIP") from error


def _compact(document: dict) -> bytes:
    return (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def signed_material_for_target(*, target_id: str, source: dict, full_id: int,
                               full_tar: bytes, bundle: bytes, candidate_id: int,
                               core: bytes, environment: dict,
                               legal: dict | None = None) -> bytes:
    """Canonical compact evidence bytes; publisher must independently reconstruct."""
    require(isinstance(bundle, bytes) and bool(bundle) and isinstance(full_tar, bytes)
            and bool(full_tar) and isinstance(core, bytes) and bool(core)
            and isinstance(environment, dict) and bool(environment),
            "missing authenticated material for signed target")
    document = {"schema_version": 1, "kind": "native-dependency-signed-material",
                "target": target_id, "source": source,
                "full_artifact_id": full_id,
                "full_tar_sha256": hashlib.sha256(full_tar).hexdigest(),
                "full_tar_bundle_sha256": hashlib.sha256(bundle).hexdigest(),
                "candidate_artifact_id": candidate_id,
                "core_sha256": hashlib.sha256(core).hexdigest(),
                "trusted_environment": environment}
    if target_id.startswith("adb-"):
        require(isinstance(legal, dict) and set(legal) == {
            "support_artifact_id", "support_zip_sha256", "full_release_artifact_id",
            "full_release_zip_sha256", "source_set_receipt_sha256",
            "overlay_receipt_sha256", "source_archive_sha256"},
            "missing authenticated ADB legal material")
        document["legal"] = legal
    else:
        require(legal is None, "unexpected iOS legal material")
    return _compact(document)


def require_reviewed_ios_host_tools(targets: dict) -> None:
    """Refuse qualification until the active Autotools/Perl/m4 closure is pinned."""
    # Catalog schema 1 currently admits only five iOS tools. A future reviewed
    # schema must bind the additional inputs and compare independent runner
    # observations before this gate can issue a successful qualification.
    for target in ("ios-x86_64", "ios-arm64"):
        toolchain = targets[target]["toolchain"]
        require(isinstance(toolchain.get("host_tools"), dict) and bool(toolchain["host_tools"]),
                "iOS host tool closure is not independently reviewed and pinned")


def run_gate(*, repo_root: pathlib.Path, output_dir: pathlib.Path, mode: str,
             source: dict, needs: dict, api: ActionsAPI) -> dict:
    """Authenticate all evidence before writing any candidate-only receipts."""
    require(mode in ("qualify", "publish"), "unsupported dependency gate mode")
    try:
        _source({key: value for key, value in source.items() if key != "event_name"},
                ".github/workflows/ci-build.yml")
    except (TypeError, ValueError) as error:
        raise GateError("invalid canonical Actions source") from error
    require(source.get("event_name") == "workflow_dispatch", "expected workflow_dispatch source")
    root = pathlib.Path(repo_root)
    require(checkout_head(root) == source["sha"], "checked-out git HEAD differs from trusted source SHA")
    catalog = _json((root / "ci/dependencies/catalog.json").read_bytes())
    targets = validate_catalog(catalog)
    require_reviewed_ios_host_tools(targets)
    require(isinstance(needs, dict) and set(needs) == set(BUILDER_JOBS.values()) | {LEGAL_JOB},
            "expected seven builders and independently trusted legal job in toJSON(needs)")
    prefix = f"/actions/runs/{source['run_id']}"
    run = api.get_json(prefix)
    require(run.get("head_sha") == source["sha"] and run.get("run_attempt") == source["run_attempt"]
            and run.get("head_branch") == source["ref"][len("refs/heads/"):]
            and run.get("repository", {}).get("full_name") == source["repository"]
            and run.get("event") == "workflow_dispatch"
            and run.get("path") == source["workflow"], "wrong Actions run or attempt")
    jobs = _list(api, prefix + f"/attempts/{source['run_attempt']}/jobs", "jobs")
    artifact_list = _list(api, prefix + "/artifacts", "artifacts")
    workflow_text = (root / ".github/workflows/ci-build.yml").read_text(encoding="utf-8")
    try:
        _validate_jobs(jobs, {key: needs[key] for key in BUILDER_JOBS.values()}, source,
                       targets, reviewed_runners(workflow_text, targets))
    except ValueError as error:
        raise GateError("untrusted builder attempt job or checked-out runner policy") from error
    support_ids = _support_evidence(api, needs, source, jobs, workflow_text)
    require(len({item.get("id") for item in artifact_list if isinstance(item, dict)}) == len(artifact_list),
            "duplicate Actions artifact list IDs")
    metadata_by_id = {item["id"]: item for item in artifact_list}
    candidate_metadata, full_metadata, downloads, full_downloads, core_archives = [], [], {}, {}, {}
    builders = []
    for target_id, row in sorted(targets.items()):
        result = needs[BUILDER_JOBS[target_id]]
        require(isinstance(result, dict) and result.get("result") == "success"
                and isinstance(result.get("outputs"), dict), "missing successful builder needs result")
        outputs = result["outputs"]
        candidate_id, full_id = outputs.get("artifact_id"), outputs.get("full_artifact_id")
        candidate_name = f"native-core-candidate-{target_id}-{source['run_id']}-{source['run_attempt']}"
        full_name = f"adb-helper-{row['target']}" if target_id.startswith("adb-") else f"ios-native-{row['target']}"
        candidate_meta = _artifact(api, candidate_id, candidate_name, source)
        full_meta = _artifact(api, full_id, full_name, source)
        require(metadata_by_id.get(int(candidate_id)) == candidate_meta
                and metadata_by_id.get(int(full_id)) == full_meta,
                "artifact ID not independently listed in parent run")
        candidate_metadata.append(candidate_meta)
        full_metadata.append(full_meta)
        core = _zip_files(api.get_bytes(f"/actions/artifacts/{candidate_id}/zip", MAX_ZIP_BYTES),
                          {"candidate.json": MAX_CANDIDATE_BYTES, row["archive_name"]: MAX_FULL_TAR_BYTES})
        full = _zip_files(api.get_bytes(f"/actions/artifacts/{full_id}/zip", MAX_ZIP_BYTES),
                          {full_name + ".tar.gz": MAX_FULL_TAR_BYTES})
        downloads[int(candidate_id)] = {"candidate": core["candidate.json"],
                                        "archive": core[row["archive_name"]]}
        core_archives[target_id] = core[row["archive_name"]]
        full_downloads[int(full_id)] = full[full_name + ".tar.gz"]
        builders.append({"job_id": BUILDER_JOBS[target_id], "conclusion": result["result"],
                         "outputs": {field: outputs.get(field) for field in
                                     ("artifact_id", "candidate_sha256", "archive_sha256")}})
    require(len(set(full_downloads) | set(downloads) | {int(value) for value in support_ids.values()}) == 16,
            "reused artifact ID across independent builds or legal assets")
    for field, identifier in support_ids.items():
        require(metadata_by_id.get(int(identifier)) == api.get_json(f"/actions/artifacts/{identifier}"),
                f"{field} artifact not listed in parent run")
    bundles = {target_id: fetch_attestation_bundle(api, full_downloads[int(needs[BUILDER_JOBS[target_id]]["outputs"]["full_artifact_id"])], source)
               for target_id in sorted(targets)}
    trusted = verify_full_evidence(repo_root=root, source=source, run=run, attempt_jobs=jobs,
                                   needs={key: needs[key] for key in BUILDER_JOBS.values()},
                                   artifacts=full_metadata, full_downloads=full_downloads,
                                   candidate_archives=core_archives, bundles=bundles,
                                   catalog=catalog, workflow_text=workflow_text)
    legal = _legal_closure(api, support_ids, root, {
        target_id: full_downloads[int(needs[BUILDER_JOBS[target_id]]["outputs"]["full_artifact_id"])]
        for target_id in targets if target_id.startswith("adb-")},
        {target_id: downloads[int(needs[BUILDER_JOBS[target_id]]["outputs"]["artifact_id"])]
         for target_id in targets if target_id.startswith("adb-")})
    receipts = qualify_from_repo(root, source, builders, candidate_metadata, downloads,
                                 trusted_environment=trusted)
    require(set(receipts) == set(targets), "qualification omitted reviewed native target")
    material = {}
    for target_id in sorted(targets):
        outputs = needs[BUILDER_JOBS[target_id]]["outputs"]
        full_id, candidate_id = int(outputs["full_artifact_id"]), int(outputs["artifact_id"])
        material[target_id] = signed_material_for_target(
            target_id=target_id, source=source, full_id=full_id,
            full_tar=full_downloads[full_id], bundle=bundles[target_id],
            candidate_id=candidate_id, core=core_archives[target_id],
            environment=trusted[target_id],
            legal=legal if target_id.startswith("adb-") else None)
    qualified = {"schema_version": 1, "kind": "native-dependency-qualified-run",
                 "result": "success", "source": dict(source),
                 "targets": {target_id: {
                     "receipt": receipts[target_id],
                     "signed_material_sha256": hashlib.sha256(material[target_id]).hexdigest()}
                     for target_id in sorted(targets)}}
    destination = pathlib.Path(output_dir)
    require(not destination.exists(), "qualification output directory already exists")
    destination.mkdir(parents=True)
    try:
        material_root = destination / "materials"
        material_root.mkdir()
        for target_id, receipt in sorted(receipts.items()):
            require(receipt.get("publication_status") == "candidate-only", "unexpected production receipt")
            (destination / f"{target_id}.json").write_bytes(_compact(receipt))
            (material_root / f"{target_id}.json").write_bytes(material[target_id])
        (destination / "qualified.json").write_bytes(_compact(qualified))
    except BaseException:
        shutil.rmtree(destination)
        raise
    return receipts


def write_gate_archive(output_dir: pathlib.Path, tar_path: pathlib.Path) -> dict:
    """Select exactly the publisher's eight files, never tar the whole output root.

    Produces reproducible gzip and tar headers; the returned SHA/size describe
    the actual upload bytes, not an artifact ID or a prospective registry lock.
    """
    source = pathlib.Path(output_dir)
    destination = pathlib.Path(tar_path)
    require(not destination.exists() and not destination.is_symlink(),
            "gate archive output already exists")
    materials = source / "materials"
    require(source.is_dir() and not source.is_symlink()
            and materials.is_dir() and not materials.is_symlink(),
            "missing regular gate output directory")
    targets = set(BUILDER_JOBS)
    require({path.name for path in materials.iterdir()} == {target + ".json" for target in targets},
            "missing or extra signed material file")
    names = ["qualified.json", *(f"materials/{target}.json" for target in sorted(targets))]
    files = {}
    for name in names:
        path = source.joinpath(*name.split("/"))
        require(path.is_file() and not path.is_symlink()
                and 0 < path.stat().st_size <= MAX_GATE_ARCHIVE_BYTES,
                "missing or unsafe Gate material")
        files[name] = path.read_bytes()
    require(sum(len(raw) for raw in files.values()) + 10240 + 512 * len(files)
            <= MAX_GATE_ARCHIVE_BYTES, "oversized Gate material archive")
    try:
        qualified = _json(files["qualified.json"])
        require(_compact(qualified) == files["qualified.json"]
                and isinstance(qualified, dict)
                and set(qualified) == {"schema_version", "kind", "result", "source", "targets"}
                and qualified["kind"] == "native-dependency-qualified-run"
                and qualified["result"] == "success" and type(qualified["schema_version"]) is int
                and qualified["schema_version"] == 1 and isinstance(qualified["targets"], dict)
                and set(qualified["targets"]) == targets,
                "invalid Gate qualification document")
        for target in sorted(targets):
            entry = qualified["targets"][target]
            raw = files[f"materials/{target}.json"]
            document = _json(raw)
            require(isinstance(entry, dict) and set(entry) == {"receipt", "signed_material_sha256"}
                    and isinstance(document, dict) and document.get("target") == target
                    and document.get("source") == qualified["source"]
                    and raw == _compact(document)
                    and entry["signed_material_sha256"] == hashlib.sha256(raw).hexdigest(),
                    "modified or mismatched Gate signed material")
            receipt = source / f"{target}.json"
            require(receipt.is_file() and not receipt.is_symlink()
                    and receipt.read_bytes() == _compact(entry["receipt"]),
                    "missing or changed candidate qualification receipt")
        tar_stream = io.BytesIO()
        with tarfile.open(fileobj=tar_stream, mode="w", format=tarfile.USTAR_FORMAT) as archive:
            for name in names:
                data = files[name]
                member = tarfile.TarInfo(name)
                member.size, member.mode, member.mtime = len(data), 0o644, 0
                archive.addfile(member, io.BytesIO(data))
        expanded = tar_stream.getvalue()
        require(len(expanded) <= MAX_GATE_ARCHIVE_BYTES, "oversized Gate tar contents")
        stream = io.BytesIO()
        with gzip.GzipFile(fileobj=stream, mode="wb", filename="", mtime=0) as compressor:
            compressor.write(expanded)
        raw = stream.getvalue()
        require(0 < len(raw) <= MAX_GATE_ARCHIVE_BYTES, "oversized Gate archive")
        with destination.open("xb") as output:
            output.write(raw)
        return {"sha256": hashlib.sha256(raw).hexdigest(), "size": len(raw)}
    except (OSError, TypeError, KeyError, AttributeError, tarfile.TarError) as error:
        raise GateError("cannot construct exact Gate archive") from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("qualify", "publish"), required=True)
    parser.add_argument("--repo-root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--needs-json", type=pathlib.Path, required=True,
                        help="file containing GitHub-controlled toJSON(needs), including legal job")
    args = parser.parse_args(argv)
    source = {"repository": os.environ.get("GITHUB_REPOSITORY"),
              "workflow": ".github/workflows/ci-build.yml", "event_name": os.environ.get("GITHUB_EVENT_NAME"),
              "sha": os.environ.get("GITHUB_SHA"), "ref": os.environ.get("GITHUB_REF"),
              "run_id": int(os.environ.get("GITHUB_RUN_ID", "0")),
              "run_attempt": int(os.environ.get("GITHUB_RUN_ATTEMPT", "0"))}
    try:
        needs = _json(args.needs_json.read_bytes())
        run_gate(repo_root=args.repo_root, output_dir=args.output_dir, mode=args.mode,
                 source=source, needs=needs, api=ActionsAPI(os.environ.get("GH_TOKEN", "")))
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"native dependency gate blocked: {error}\n")


if __name__ == "__main__":
    main()
