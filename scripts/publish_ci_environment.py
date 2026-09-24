#!/usr/bin/env python3
"""Publish qualified original OCI bytes, then retain and publicly verify evidence.

No Docker commands or candidate executables run here. ORAS is downloaded only
from its checked-in official release pin, used with temporary authentication,
and never included in publication artifacts. Metadata tar files are read as
bounded regular-file snapshots; candidate layers are never extracted.

Publication may initially be private. Detached signatures are created by the
workflow's attestation actions, not this module. Finalization retains verified
bundles and a lock proposal before a separate anonymous visibility check. Only
output directories are written; the active repository lock is never changed.
"""
from __future__ import annotations

import gzip
import hashlib
import json
import os
import pathlib
import re
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import ci_environment as core
import ci_environment_pipeline as pipeline
import ci_environment_registry as registry
from consume_ci_environment import _compare_registry_image, _validate_receipt
from prefetch_adb_helper_sources import download, safe_extract

MAX_ARCHIVE_BYTES = 128 * 1024 * 1024
MAX_METADATA_FILES = 128
TSAN = "jammy-qt5-tsan"
SOURCE_VERIFIER = "scripts/verify_tsan_qt_sources.py"
LINUX_VALIDATOR = ".github/actions/linux-validate/action.yml"
FAMILY_FILES = ("candidate.json", "inputs.json", "profile-receipts.json", "verification.json", "material-manifest.json")


class PublicationError(pipeline.PipelineError):
    """Publication or proposal verification failed closed."""


def require(condition, message):
    if not condition:
        raise PublicationError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def same(left, right):
    return core.canonical_digest(left) == core.canonical_digest(right)


def plain_path(path):
    path = pathlib.Path(path).absolute()
    require(not any(item.is_symlink() for item in (path,) + tuple(path.parents)),
            "publication path must not traverse a symlink")
    return path


def file_bytes(path, limit=core.MAX_METADATA_BYTES):
    path = plain_path(path)
    before = path.lstat()
    require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and before.st_size <= limit,
            "publication input must be a bounded regular non-link file")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_BINARY", 0)
    with os.fdopen(os.open(path, flags), "rb") as stream:
        observed = os.fstat(stream.fileno())
        data = stream.read(limit + 1)
    after = path.stat()
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)
    require(identity(before) == identity(observed) == identity(after) and len(data) == before.st_size,
            "publication input changed while reading")
    return data


def document(files, name):
    require(name in files, "missing publication metadata: " + name)
    require(len(files[name]) <= core.MAX_METADATA_BYTES, "publication JSON exceeds size limit")
    return core._parse_json(files[name], name)


def metadata_archive(path):
    """Read GNU/USTAR gzip metadata without tar extraction or PAX expansion."""
    path = plain_path(path)
    require(path.is_file() and path.stat().st_size <= MAX_ARCHIVE_BYTES, "metadata archive exceeds size limit")
    files = {}
    names = set()
    total = 0
    try:
        with gzip.open(path, "rb") as stream:
            while True:
                header = stream.read(512)
                require(len(header) == 512, "truncated metadata archive")
                if header == b"\0" * 512:
                    require(stream.read(512) == b"\0" * 512, "metadata archive needs two end blocks")
                    trailing = stream.read(10241)
                    require(len(trailing) <= 10240 and not any(trailing), "unexpected data after metadata archive")
                    break
                require(header[257:263] in (b"ustar\0", b"ustar "), "metadata archive must use ordinary USTAR/GNU headers")
                member = tarfile.TarInfo.frombuf(header, "utf-8", "strict")
                require(member.type in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE),
                        "metadata archive links and extension records are forbidden")
                name = member.name
                if name.startswith("./"):
                    name = name[2:]
                if member.isdir():
                    name = name.rstrip("/")
                    require(member.size == 0, "metadata directory has a payload")
                    if name in ("", "."):
                        name = "."
                    else:
                        core._relative_path(name, "metadata directory")
                else:
                    core._relative_path(name, "metadata member")
                require(name not in names, "duplicate metadata archive member")
                names.add(name)
                require(len(names) <= MAX_METADATA_FILES, "too many metadata archive members")
                if member.isdir():
                    continue
                require(0 < member.size <= registry.BUNDLE_LIMIT, "oversized metadata archive member")
                total += member.size
                require(total <= MAX_ARCHIVE_BYTES, "metadata archive expands beyond size limit")
                payload = stream.read(member.size)
                require(len(payload) == member.size, "truncated metadata payload")
                padding = stream.read((-member.size) % 512)
                require(len(padding) == (-member.size) % 512 and not any(padding), "invalid metadata padding")
                files[name] = payload
    except (OSError, EOFError, tarfile.TarError, UnicodeError) as error:
        raise PublicationError("invalid publication metadata archive") from error
    return files


def transport(root, filename, artifact_id, archive_sha256, source, runner):
    pipeline.validate_source(source)
    core._positive_int(artifact_id, "external artifact ID")
    core._digest(archive_sha256, "external metadata archive SHA-256", prefix=False)
    path = plain_path(pipeline.regular(plain_path(root), filename))
    require(path.stat().st_size <= MAX_ARCHIVE_BYTES and pipeline.sha256(path) == archive_sha256,
            "external metadata archive SHA-256 mismatch")
    pipeline.verify_artifact(artifact_id, source, runner)
    files = metadata_archive(path)
    require(pipeline.sha256(path) == archive_sha256, "metadata archive changed while reading")
    return files


def output_path(path):
    path = plain_path(path)
    require(not path.exists(), "refusing to overwrite publication output")
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def write_snapshot(root, files):
    for relative, data in files.items():
        core._relative_path(relative, "publication output")
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("xb") as stream:
            stream.write(data)


def expose(output, files):
    output = output_path(output)
    with tempfile.TemporaryDirectory(prefix="klogg-publication-", dir=str(output.parent)) as temporary:
        staged = pathlib.Path(temporary) / "result"
        staged.mkdir()
        write_snapshot(staged, files)
        require(not output.exists() and not output.is_symlink(), "publication output appeared concurrently")
        staged.rename(output)


def json_bytes(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode("ascii")


def redistribution(repo_root, family, inputs, raw, policy):
    """Require retained origin mappings and policy-bound in-image Qt sources.

    QualifyTsan executes the source verifier in the image. This publisher binds
    that successful exact job and current verifier/wrapper bytes through policy;
    it does not invent a self-reported extra 'passed' receipt or extract layers.
    """
    records = inputs.get("files")
    require(isinstance(records, list) and all(isinstance(record, dict) for record in records), "invalid source material inventory")
    by_path = {}
    for record in records:
        core._object(record, {"path", "sha256", "size"}, "material file")
        core._relative_path(record["path"], "material file")
        core._digest(record["sha256"], "material file SHA-256", prefix=False)
        require(type(record["size"]) is int and record["size"] >= 0 and record["path"] not in by_path,
                "duplicate or invalid source material file")
        by_path[record["path"]] = record
    retained = by_path.get("inputs/material-manifest.json")
    require(retained is not None and retained["sha256"] == digest(raw) and retained["size"] == len(raw),
            "retained source/license material manifest is missing or not byte-bound")
    manifest = core._parse_json(raw, "source material manifest")
    core._object(manifest, {"schema_version", "kind", "family", "definition", "assets", "transforms"}, "source material manifest")
    core._version(manifest, "source material manifest")
    require(manifest["kind"] == "ci-material-acquisition" and manifest["family"] == family,
            "source material manifest family mismatch")
    declarations = pipeline.read_document(repo_root, "ci/environments/materials.json")
    core._object(declarations, {"schema_version", "assets", "families"}, "material declarations")
    core._version(declarations, "material declarations")
    require(isinstance(declarations["families"], dict) and set(declarations["families"]) == set(pipeline.BUILD_JOBS),
            "source material family inventory mismatch")
    definition = declarations["families"][family]
    require(same(manifest["definition"], definition), "retained source material definition is stale")
    selected = {item["asset"] for key in ("downloads", "tools") for item in definition[key]}
    require(isinstance(manifest["assets"], dict) and set(manifest["assets"]) == selected,
            "source/license asset mapping is incomplete")
    for name in selected:
        expected = declarations["assets"][name]
        require(same(manifest["assets"][name], expected), "source/license asset declaration mismatch")
        require(all(isinstance(expected.get(key), str) and expected[key] for key in ("license", "source_url", "checksum_source")),
                "applicable notices and authenticated source mappings are required")
        core._digest(expected.get("sha256"), "source asset SHA-256", prefix=False)
        require(urllib.parse.urlsplit(expected["source_url"]).scheme == "https", "source mapping requires HTTPS")
    require(inputs.get("build_args", {}).get("UBUNTU_IMAGE") == definition["base_image"], "upstream image origin mapping mismatch")
    if family != TSAN:
        return
    required_files = policy["profiles"]["tsan"]["verification_files"]
    for path in (SOURCE_VERIFIER, LINUX_VALIDATOR):
        require(required_files.get(path) == "sha256:" + pipeline.sha256(pipeline.regular(repo_root, path)),
                "modified Qt redistribution requires current source verifier and execution policy")
    downloads = [item for item in definition["downloads"] if item["path"].startswith("inputs/qt/")]
    version = definition["build_args"].get("QT_VERSION")
    expected_names = {module + "-everywhere-opensource-src-" + str(version) + ".tar.xz"
                      for module in ("qtbase", "qtsvg", "qttools", "qttranslations")}
    require(len(downloads) == 4 and {pathlib.PurePosixPath(item["path"]).name for item in downloads} == expected_names,
            "modified Qt requires all four original corresponding-source archives")
    for item in downloads:
        asset = manifest["assets"][item["asset"]]
        record = by_path.get(item["path"])
        require(record is not None and record["size"] > 0 and record["sha256"] == asset["sha256"],
                "original Qt source archive declaration is missing or substituted")
        origin = urllib.parse.urlsplit(asset["source_url"])
        acquired = urllib.parse.urlsplit(asset["url"])
        require(acquired.scheme == "https" and acquired.path == origin.path
                and origin.path.endswith("/" + pathlib.PurePosixPath(item["path"]).name),
                "Qt source archives must retain their original source mapping")


def qualified_records(repo_root, source, files):
    manifest = document(files, "qualified.json")
    core._object(manifest, {"schema_version", "kind", "operation", "source", "sarif_result", "job_results", "artifact_identities", "families"}, "qualified run")
    core._version(manifest, "qualified run")
    require(manifest["kind"] == "qualified-run" and manifest["operation"] == "publish" and manifest["sarif_result"] == "success",
            "qualify-only or incomplete SARIF evidence cannot authorize publication")
    pipeline.validate_source(manifest["source"])
    require(same(manifest["source"], source), "qualified source/ref/run/attempt mismatch")
    catalog = pipeline.read_document(repo_root, "ci/environments/recipes.json")
    core.validate_catalog(catalog)
    pipeline.validate_artifact_identities(catalog, manifest["artifact_identities"], manifest["job_results"])
    require(isinstance(manifest["families"], dict) and set(manifest["families"]) == set(pipeline.BUILD_JOBS), "qualified family set mismatch")
    expected_files = {"qualified.json"} | {"families/" + family + "/" + name for family in pipeline.BUILD_JOBS for name in FAMILY_FILES}
    require(set(files) == expected_files, "qualified archive must retain exact family evidence and material manifests")
    families = {}
    for family in sorted(pipeline.BUILD_JOBS):
        prefix = "families/" + family + "/"
        entry = manifest["families"][family]
        core._object(entry, {"candidate_digest", "input_digest", "receipt_sha256"}, "qualified family")
        candidate = document(files, prefix + "candidate.json")
        core.validate_candidate(candidate, catalog)
        require(candidate["family"] == family and same(candidate["source"], source), "qualified candidate family/source mismatch")
        require(candidate["recipe_digest"] == core.recipe_identity(catalog, family, repo_root), "qualified recipe is stale")
        require(core.canonical_digest(candidate) == entry["candidate_digest"], "qualified candidate digest mismatch")
        inputs = document(files, prefix + "inputs.json")
        require(inputs.get("family") == family and inputs.get("platform") == core.PLATFORM
                and core.input_identity(inputs) == entry["input_digest"] == candidate["input_digest"], "qualified input digest mismatch")
        raw = files[prefix + "verification.json"]
        require(len(raw) <= registry.METADATA_LIMIT and digest(raw) == entry["receipt_sha256"], "raw qualification receipt digest mismatch")
        policy = pipeline.family_policy(repo_root, family)
        receipts = document(files, prefix + "profile-receipts.json")
        identity = manifest["artifact_identities"]["candidates"][family]
        qualification = core.aggregate_qualification(catalog, candidate, receipts,
            {profile: manifest["job_results"][job] for profile, job in pipeline.PROFILE_JOBS[family].items()},
            candidate_artifact_id=identity["artifact_id"], policy=policy)
        require(same(qualification, core._parse_json(raw, "qualification receipt")), "current-policy reaggregation does not match qualification receipt")
        material = files[prefix + "material-manifest.json"]
        redistribution(repo_root, family, inputs, material, policy)
        families[family] = {"candidate": candidate, "inputs": inputs, "receipts": receipts,
                            "receipt": raw, "material": material}
    return manifest, families


def manifest_bytes(path, image):
    # The core scanner rejects links/extensions and checks all stored blob bytes.
    with plain_path(path).open("rb") as stream:
        members, archive_hash = core._scan_tar(stream)
        require(archive_hash == image["archive_sha256"], "original OCI archive changed before publication")
        record = members["blobs/sha256/" + image["manifest_digest"][7:]]
        require(record["size"] <= registry.METADATA_LIMIT, "OCI manifest exceeds attestation limit")
        stream.seek(record["offset"])
        data = stream.read(record["size"])
    require("sha256:" + digest(data) == image["manifest_digest"], "original OCI manifest digest mismatch")
    return data


def tool_pin(repo_root):
    document = pipeline.read_document(repo_root, "ci/environments/publisher-tools.json")
    core._object(document, {"schema_version", "oras"}, "publisher tool lock")
    core._version(document, "publisher tool lock")
    tool = document["oras"]
    core._object(tool, {"version", "url", "sha256", "executable"}, "ORAS tool lock")
    require(isinstance(tool["version"], str) and re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", tool["version"]), "invalid ORAS version")
    expected_url = "https://github.com/oras-project/oras/releases/download/v{0}/oras_{0}_linux_amd64.tar.gz".format(tool["version"])
    require(tool["url"] == expected_url and tool["executable"] == "oras", "publisher requires the pinned official ORAS archive")
    core._digest(tool["sha256"], "ORAS archive SHA-256", prefix=False)
    return tool


def run_oras(command, runner, *, stdin=None):
    try:
        result = (runner or subprocess.run)(command, check=True, capture_output=True, text=True, input=stdin, timeout=1800)
    except (OSError, subprocess.SubprocessError) as error:
        # Never include stdin, stderr or credentials in publication diagnostics.
        raise PublicationError("pinned ORAS command failed: " + command[1]) from error
    require(result.returncode == 0, "pinned ORAS command returned a failure status")


def publish(repo_root, source, qualified_root, qualified_artifact_id,
            qualified_archive_sha256, evidence_root, output, *, runner=None,
            downloader=None, environment=None):
    repo_root, evidence_root = plain_path(repo_root), plain_path(evidence_root)
    output = output_path(output)
    files = transport(qualified_root, "qualified.tar.gz", qualified_artifact_id, qualified_archive_sha256, source, runner)
    qualified, families = qualified_records(repo_root, source, files)
    # Complete every qualification/source check before obtaining any write tool
    # or authenticating. A bad last family must not partially publish earlier ones.
    manifests = {}
    for family, entry in families.items():
        identity = qualified["artifact_identities"]["candidates"][family]
        candidate_root = plain_path(evidence_root / "candidates" / family)
        candidate, inputs, _ = pipeline.validate_candidate_files(repo_root, family, candidate_root, source, identity["archive_sha256"])
        require(same(candidate, entry["candidate"]) and same(inputs, entry["inputs"]), "original candidate differs from qualified metadata")
        require(pipeline.retained_material_manifest(candidate_root, inputs) == entry["material"], "original source material bytes differ from qualification")
        pipeline.verify_artifact(identity["artifact_id"], source, runner)
        original_receipts = []
        for profile in sorted(pipeline.PROFILE_JOBS[family]):
            receipt_identity = qualified["artifact_identities"]["profiles"][family][profile]
            archive = pipeline.regular(evidence_root / "receipts" / family / profile, "qualification.tar.gz")
            original_receipts.append(pipeline.read_profile_archive(plain_path(archive), receipt_identity))
            pipeline.verify_artifact(receipt_identity["artifact_id"], source, runner)
        require(same(original_receipts, entry["receipts"]), "original profile receipts differ from qualified evidence")
        manifests[family] = manifest_bytes(candidate_root / "candidate.oci.tar", candidate["image"])
    tool = tool_pin(repo_root)
    environment = os.environ if environment is None else environment
    token, actor = environment.get("GH_TOKEN"), environment.get("GITHUB_ACTOR")
    require(isinstance(token, str) and bool(token) and not any(char.isspace() or ord(char) < 33 for char in token), "package-write credentials are required")
    require(isinstance(actor, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*(?:\[bot\])?", actor), "a valid registry actor is required")
    result = {"schema_version": 1, "kind": "environment-publication", "source": source,
              "qualified_artifact_id": qualified_artifact_id, "qualified_archive_sha256": qualified_archive_sha256,
              "tool": tool, "families": {}}
    snapshots = {}
    with tempfile.TemporaryDirectory(prefix="klogg-oras-") as temporary:
        temporary = pathlib.Path(temporary).resolve()
        archive = temporary / "oras.tar.gz"
        try:
            (downloader or download)(tool["url"], archive)
            require(pipeline.sha256(plain_path(archive)) == tool["sha256"], "ORAS tool archive hash mismatch")
            safe_extract(archive, temporary / "tool")
        except (OSError, RuntimeError, tarfile.TarError) as error:
            raise PublicationError("cannot acquire the pinned ORAS tool") from error
        executable = temporary / "tool" / "oras"
        file_bytes(executable, MAX_ARCHIVE_BYTES)
        require(os.access(executable, os.X_OK), "pinned ORAS executable is not executable")
        auth = temporary / "registry-config.json"
        with os.fdopen(os.open(auth, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "w") as stream:
            stream.write("{}")
        run_oras([str(executable), "login", "ghcr.io", "--username", actor, "--password-stdin", "--registry-config", str(auth)], runner, stdin=token + "\n")
        for family, entry in families.items():
            image = entry["candidate"]["image"]
            original = evidence_root / "candidates" / family / "candidate.oci.tar"
            reference = core.REGISTRY + "@" + image["manifest_digest"]
            tag = core.REGISTRY + ":" + family + "-" + image["manifest_digest"][7:]
            # ORAS v1.3 target.go parses the last '@' as a digest and opens
            # non-directory OCI sources with oci.NewFromTar. No layer extraction.
            run_oras([str(executable), "cp", "--from-oci-layout", str(original) + "@" + image["manifest_digest"],
                      tag, "--to-registry-config", str(auth)], runner)
            remote = temporary / (family + ".manifest.json")
            run_oras([str(executable), "manifest", "fetch", "--registry-config", str(auth), "--output", str(remote), reference], runner)
            require(file_bytes(remote, registry.METADATA_LIMIT) == manifests[family], "registry copy changed original OCI manifest bytes/digest")
            require(pipeline.sha256(original) == image["archive_sha256"], "original candidate archive changed during publication")
            prefix = "families/" + family + "/"
            snapshots[prefix + "manifest.json"] = manifests[family]
            snapshots[prefix + "verification.json"] = entry["receipt"]
            result["families"][family] = {"image_digest": image["manifest_digest"],
                "receipt_sha256": digest(entry["receipt"]), "receipt": prefix + "verification.json"}
    snapshots["publication.json"] = json_bytes(result)
    expose(output, snapshots)
    return result


def verify_bundles(manifest, receipt, image_bundle, receipt_bundle, source, runner):
    with tempfile.TemporaryDirectory(prefix="klogg-publication-signatures-") as temporary:
        root = pathlib.Path(temporary).resolve()
        write_snapshot(root, {"manifest.json": manifest, "verification.json": receipt,
                              "image.sigstore.json": image_bundle, "receipt.sigstore.json": receipt_bundle})
        require(registry.verify_attestation(root / "manifest.json", root / "image.sigstore.json", source,
                                            registry.REGISTRY, runner=runner) is True, "image attestation is not verified")
        require(registry.verify_attestation(root / "verification.json", root / "receipt.sigstore.json", source,
                                            "verification.json", runner=runner) is True, "receipt attestation is not verified")


def finalize_publication(repo_root, source, qualified_root, publication_root,
                         bundle_map, output, *, runner=None):
    output = output_path(output)
    repo_root, publication_root = plain_path(repo_root), plain_path(publication_root)
    publication = core._parse_json(file_bytes(publication_root / "publication.json"), "publication metadata")
    core._object(publication, {"schema_version", "kind", "source", "qualified_artifact_id", "qualified_archive_sha256", "tool", "families"}, "publication")
    core._version(publication, "publication")
    require(publication["kind"] == "environment-publication" and same(publication["source"], source), "publication source identity mismatch")
    require(same(publication["tool"], tool_pin(repo_root)), "publisher tool pin changed")
    files = transport(qualified_root, "qualified.tar.gz", publication["qualified_artifact_id"], publication["qualified_archive_sha256"], source, runner)
    _, families = qualified_records(repo_root, source, files)
    require(isinstance(publication["families"], dict) and set(publication["families"]) == set(families)
            and isinstance(bundle_map, dict) and set(bundle_map) == set(families), "publication requires every exact family and detached bundle pair")
    lock = {"schema_version": 1, "kind": "production-lock", "families": {}}
    snapshots = {}
    for family, entry in families.items():
        image = entry["candidate"]["image"]
        published = publication["families"][family]
        core._object(published, {"image_digest", "receipt_sha256", "receipt"}, "published family")
        require(published["image_digest"] == image["manifest_digest"] and published["receipt_sha256"] == digest(entry["receipt"])
                and published["receipt"] == "families/" + family + "/verification.json", "published image/receipt substitution")
        require(file_bytes(publication_root / published["receipt"], registry.METADATA_LIMIT) == entry["receipt"], "published raw receipt changed")
        manifest = file_bytes(publication_root / "families" / family / "manifest.json", registry.METADATA_LIMIT)
        require("sha256:" + digest(manifest) == image["manifest_digest"], "published manifest digest changed")
        bundles = core._object(bundle_map[family], {"image_bundle", "receipt_bundle"}, "detached bundle paths")
        image_bundle = file_bytes(bundles["image_bundle"], registry.BUNDLE_LIMIT)
        receipt_bundle = file_bytes(bundles["receipt_bundle"], registry.BUNDLE_LIMIT)
        verify_bundles(manifest, entry["receipt"], image_bundle, receipt_bundle, source, runner)
        prefix = "ci/environments/evidence/" + family + "/"
        snapshots[prefix + "verification.json"] = entry["receipt"]
        snapshots[prefix + "image.sigstore.json"] = image_bundle
        snapshots[prefix + "receipt.sigstore.json"] = receipt_bundle
        snapshots[prefix + "material-manifest.json"] = entry["material"]
        snapshots["ci/environments/inputs/" + family + ".json"] = files["families/" + family + "/inputs.json"]
        candidate = entry["candidate"]
        lock["families"][family] = {"image": core.REGISTRY + "@" + image["manifest_digest"], "platform": core.PLATFORM,
            "config_digest": image["config_digest"], "recipe_digest": candidate["recipe_digest"], "input_digest": candidate["input_digest"],
            "source": source, "qualification": {"digest": "sha256:" + digest(entry["receipt"]),
                "receipt": prefix + "verification.json", "image_bundle": prefix + "image.sigstore.json", "receipt_bundle": prefix + "receipt.sigstore.json"}}
    core.validate_production_lock(lock, pipeline.read_document(repo_root, "ci/environments/recipes.json"))
    snapshots["ci/environments/lock.json"] = json_bytes(lock)
    retained = {"schema_version": 1, "kind": "publication-evidence", "source": source,
                "files": {path: digest(data) for path, data in sorted(snapshots.items())}}
    snapshots["publication-evidence.json"] = json_bytes(retained)
    expose(output, snapshots)
    return retained


def verify_publication(repo_root, source, publication_root, publication_artifact_id,
                       archive_sha256, output, *, runner=None, client=None):
    output = output_path(output)
    repo_root = plain_path(repo_root)
    files = transport(publication_root, "publication-evidence.tar.gz", publication_artifact_id, archive_sha256, source, runner)
    evidence = document(files, "publication-evidence.json")
    core._object(evidence, {"schema_version", "kind", "source", "files"}, "publication evidence")
    core._version(evidence, "publication evidence")
    require(evidence["kind"] == "publication-evidence" and same(evidence["source"], source), "publication evidence source mismatch")
    require(isinstance(evidence["files"], dict) and set(files) == set(evidence["files"]) | {"publication-evidence.json"}, "publication evidence inventory mismatch")
    for path, expected in evidence["files"].items():
        core._digest(expected, "publication file SHA-256", prefix=False)
        require(digest(files[path]) == expected, "publication evidence raw file digest mismatch")
    catalog = pipeline.read_document(repo_root, "ci/environments/recipes.json")
    lock = document(files, "ci/environments/lock.json")
    core.validate_production_lock(lock, catalog)
    require(set(lock["families"]) == set(pipeline.BUILD_JOBS), "public verification requires all six families")
    expected_files = {"publication-evidence.json", "ci/environments/lock.json"}
    for family, locked in lock["families"].items():
        expected_files.add("ci/environments/inputs/" + family + ".json")
        expected_files.add("ci/environments/evidence/" + family + "/material-manifest.json")
        expected_files.update(locked["qualification"][key] for key in ("receipt", "image_bundle", "receipt_bundle"))
    require(set(files) == expected_files, "public proposal inventory contains missing or unexpected files")
    public = client if client is not None else registry.RegistryClient()
    checked = {}
    for family, locked in lock["families"].items():
        require(same(locked["source"], source) and locked["recipe_digest"] == core.recipe_identity(catalog, family, repo_root), "public proposal source/recipe mismatch")
        paths = locked["qualification"]
        receipt = files.get(paths["receipt"])
        require(receipt is not None and "sha256:" + digest(receipt) == paths["digest"], "public raw receipt digest mismatch")
        policy = pipeline.family_policy(repo_root, family)
        candidate = _validate_receipt(core._parse_json(receipt, "signed qualification"), catalog, family, locked, core.policy_identity(policy))
        inputs = document(files, "ci/environments/inputs/" + family + ".json")
        require(inputs.get("family") == family and inputs.get("platform") == core.PLATFORM
                and core.input_identity(inputs) == locked["input_digest"], "public resolved inputs mismatch")
        redistribution(repo_root, family, inputs, files.get("ci/environments/evidence/" + family + "/material-manifest.json", b""), policy)
        manifest = _compare_registry_image(public.read_image(candidate["image"]["manifest_digest"]), candidate["image"])
        require(paths["image_bundle"] in files and paths["receipt_bundle"] in files, "missing public detached signature bundle")
        verify_bundles(manifest, receipt, files[paths["image_bundle"]], files[paths["receipt_bundle"]], source, runner)
        checked[family] = locked["image"]
    result = {"schema_version": 1, "kind": "public-environment-verification", "source": source,
              "publication_artifact_id": publication_artifact_id, "publication_archive_sha256": archive_sha256, "families": checked}
    proposed = {path: data for path, data in files.items() if path.startswith("ci/environments/")}
    proposed["public-verification.json"] = json_bytes(result)
    expose(output, proposed)
    return result
