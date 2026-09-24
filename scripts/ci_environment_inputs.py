#!/usr/bin/env python3
"""Materialize one authenticated APT closure relative to an exact Linux base.

Schema 1 supports declared signed HTTP/HTTPS sources, ordered prerequisite
bundles, an explicit HTTPS Ubuntu snapshot/CA-bootstrap policy, and source-scoped
locally supplied pinned ASCII keys. Network key acquisition, global trust imports
and arbitrary source/expiry/TLS options remain unsupported.
Validation establishes byte consistency, not provenance: callers authenticate
the manifest/runtime-lock digests. Prepared images are local resolver state,
never environment candidates or replacement base pins, and are not deleted.
No host APT is invoked. Final offline proof replays the full prerequisite chain
from the original pinned linux/amd64 base with the container network disabled.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.parse

from ci_environment import ContractError, load_json

MAX_FILES = 10000
MAX_BYTES = 20 * 1024**3
STAGE_FIELDS = {"schema_version", "stage", "platform", "base_image", "sources",
                "requested_packages", "acquisition"}
OPTIONAL_STAGE_FIELDS = {"prerequisites", "source_keyrings"}
KEY_NAME = r"[A-Za-z0-9][A-Za-z0-9_.-]*\.asc"
NAME = r"[a-z0-9][a-z0-9+.-]+"
VERSION = r"[0-9][A-Za-z0-9.+:~_-]*"
BASENAME = r"[A-Za-z0-9][A-Za-z0-9_.+:%~=-]*"
INSTALLER = pathlib.Path(__file__).with_name("ci_install_locked_apt.sh")

# The only networked package operation. An empty private lists directory plus strict
# update errors prevents fallback to stale indexes after partial acquisition.
RESOLVE_SCRIPT = r'''set -eu
export DEBIAN_FRONTEND=noninteractive LC_ALL=C
# Restore bind-mount ownership even after failure so non-root producers can
# validate/publish or remove the private staging tree on Linux hosts.
cleanup() {
    status=$?
    trap - EXIT
    rm -f -- /inputs/.bootstrap-apt.conf || status=$?
    chown -hR -- "$KLOGG_OUTPUT_OWNER" /inputs || status=$?
    exit "$status"
}
trap cleanup EXIT
test "$(uname -s)" = Linux
test "$(dpkg --print-architecture)" = amd64
case "${KLOGG_TLS_BOOTSTRAP:-}" in
    '') ;;
    snapshot.ubuntu.com)
        test "$#" -eq 1 && test "$1" = ca-certificates
        printf '%s\n' 'Acquire::https::snapshot.ubuntu.com::Verify-Peer "false";' > /inputs/.bootstrap-apt.conf
        ;;
    *) exit 2 ;;
esac
apt_locked() {
    if [ -n "${KLOGG_TLS_BOOTSTRAP:-}" ]; then
        set -- -c /inputs/.bootstrap-apt.conf "$@"
    fi
    apt-get -o Dir::Etc::sourcelist=/inputs/sources.list \
        -o Dir::Etc::sourceparts=- -o Dir::State::lists=/inputs/lists \
        -o Dir::Cache::archives=/inputs/debs \
        -o APT::Update::Error-Mode=any \
        -o Acquire::AllowInsecureRepositories=false \
        -o Acquire::AllowDowngradeToInsecureRepositories=false \
        -o APT::Get::AllowUnauthenticated=false \
        -o Acquire::https::Verify-Peer=true -o Acquire::https::Verify-Host=true \
        -o "Acquire::Check-Valid-Until=${KLOGG_CHECK_VALID_UNTIL:-true}" \
        -o Acquire::Retries=3 -o Acquire::http::Timeout=30 \
        -o Acquire::https::Timeout=30 -o Acquire::Languages=none "$@"
}
mkdir -p /inputs/lists /inputs/debs
rm -rf -- /inputs/lists/* /inputs/debs/*
dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\t${db:Status-Status}\n' > /inputs/base-packages.unsorted
sort /inputs/base-packages.unsorted > /inputs/base-packages.tsv
rm /inputs/base-packages.unsorted
# Older APT releases can ignore Error-Mode and exit zero after partial index
# failures. Check their C-locale diagnostics as well as the process status.
update_status=0
apt_locked update > /inputs/update.log 2>&1 || update_status=$?
cat /inputs/update.log
if [ "$update_status" -eq 0 ] && grep -Eq '^(W:|E:|Err:)' /inputs/update.log; then
    update_status=100
fi
rm -f /inputs/update.log
[ "$update_status" -eq 0 ] || exit "$update_status"
apt_locked --download-only --reinstall --no-install-recommends -y install "$@"
# This is an APT format placeholder, not a shell command substitution.
# shellcheck disable=SC2016
apt_locked indextargets --format '$(FILENAME)' 'Identifier: Packages' > /inputs/targets.raw
: > /inputs/index-targets.txt
while IFS= read -r target; do
    test -n "$target" && test -f "$target" && test ! -L "$target"
    printf '%s\n' "${target##*/}" >> /inputs/index-targets.txt
done < /inputs/targets.raw
test -s /inputs/index-targets.txt
rm /inputs/targets.raw
: > /inputs/resolution.tsv
for archive in /inputs/debs/*.deb; do
    test -f "$archive" && test ! -L "$archive"
    package=$(dpkg-deb -f "$archive" Package)
    version=$(dpkg-deb -f "$archive" Version)
    architecture=$(dpkg-deb -f "$archive" Architecture)
    printf '%s\t%s\t%s\t%s\n' "$package" "$version" "$architecture" "${archive##*/}" >> /inputs/resolution.tsv
done
rm -rf /inputs/lists/partial /inputs/lists/auxfiles /inputs/debs/partial
rm -f /inputs/lists/lock /inputs/debs/lock
'''


class InputError(ContractError):
    """An APT input descriptor or materialized closure is invalid."""


def require(condition, message):
    if not condition:
        raise InputError(message)


def exact_object(value, fields, label, optional=()):
    require(isinstance(value, dict) and set(fields) <= set(value) <= set(fields) | set(optional),
            label + " has missing or unknown fields")


def stage_document(document):
    return {key: document[key] for key in STAGE_FIELDS | OPTIONAL_STAGE_FIELDS if key in document}


def matches(pattern, value):
    return isinstance(value, str) and re.fullmatch(pattern, value) is not None


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def encoded(document):
    return (json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                       allow_nan=False) + "\n").encode("ascii")


def keyring_policy(stage):
    """Validate source indexes and declared key pins, without parsing OpenPGP."""
    entries = stage.get("source_keyrings", [])
    require(isinstance(entries, list) and len(entries) <= len(stage["sources"]), "invalid source keyring scopes")
    scopes = {}
    keys = {}
    for scope in entries:
        exact_object(scope, {"source_index", "keys"}, "source keyring scope")
        index = scope["source_index"]
        require(type(index) is int and 0 <= index < len(stage["sources"]) and index not in scopes,
                "invalid or duplicate source keyring index")
        require(isinstance(scope["keys"], list) and 0 < len(scope["keys"]) <= 64, "invalid scoped key list")
        names = set()
        for key in scope["keys"]:
            exact_object(key, {"name", "sha256", "fingerprints"}, "scoped key")
            require(matches(KEY_NAME, key["name"]) and key["name"] not in names, "unsafe or duplicate key filename")
            names.add(key["name"])
            require(matches(r"[0-9a-f]{64}", key["sha256"]) and key["sha256"] != "0" * 64, "invalid key digest")
            fingerprints = key["fingerprints"]
            require(isinstance(fingerprints, list) and 0 < len(fingerprints) <= 64
                    and all(matches(r"[0-9A-F]{40}", value) and value != "0" * 40 for value in fingerprints)
                    and len(fingerprints) == len(set(fingerprints)), "invalid or duplicate key fingerprints")
            require(key["name"] not in keys or keys[key["name"]] == key,
                    "a reused key name must have the same bytes and fingerprint policy")
            keys[key["name"]] = key
        scopes[index] = scope["keys"]
    return scopes, keys


def validate_stage(stage):
    exact_object(stage, STAGE_FIELDS, "stage", OPTIONAL_STAGE_FIELDS)
    require(type(stage["schema_version"]) is int and stage["schema_version"] == 1, "schema_version must be 1")
    require(matches(r"[a-z0-9]+(?:[._-][a-z0-9]+)*", stage["stage"]), "invalid stage name")
    require(stage["platform"] == "linux/amd64", "only linux/amd64 is supported")
    require(matches(r"[a-z0-9][a-z0-9./:_-]*@sha256:[0-9a-f]{64}", stage["base_image"])
            and not stage["base_image"].endswith("0" * 64), "base_image must have an exact non-placeholder digest")
    references = stage.get("prerequisites", [])
    require(isinstance(references, list) and len(references) <= 64, "invalid prerequisite list")
    names = {stage["stage"]}
    for reference in references:
        exact_object(reference, {"stage", "manifest_sha256", "runtime_lock_sha256"}, "prerequisite")
        require(matches(r"[a-z0-9]+(?:[._-][a-z0-9]+)*", reference["stage"])
                and reference["stage"] not in names, "duplicate or cyclic prerequisite stage")
        names.add(reference["stage"])
        for field in ("manifest_sha256", "runtime_lock_sha256"):
            require(matches(r"[0-9a-f]{64}", reference[field]) and reference[field] != "0" * 64,
                    "invalid prerequisite digest")
    acquisition = stage["acquisition"]
    exact_object(acquisition, {"mode"}, "acquisition", {"snapshot", "tls_bootstrap"})
    require(acquisition["mode"] == "apt-signed", "unsupported APT acquisition mode")
    snapshot = acquisition.get("snapshot")
    if "snapshot" in acquisition:
        exact_object(snapshot, {"timestamp", "check_valid_until"}, "snapshot policy")
        require(matches(r"[0-9]{8}T[0-9]{6}Z", snapshot["timestamp"])
                and snapshot["check_valid_until"] is False, "invalid snapshot timestamp or expiry policy")
        try:
            datetime.datetime.strptime(snapshot["timestamp"], "%Y%m%dT%H%M%SZ")
        except ValueError as error:
            raise InputError("invalid snapshot timestamp") from error
    if "tls_bootstrap" in acquisition:
        require(snapshot is not None and acquisition["tls_bootstrap"] == {"host": "snapshot.ubuntu.com"}
                and stage["requested_packages"] == ["ca-certificates"] and not references,
                "TLS bootstrap is restricted to the first snapshot CA-only stage")
    elif snapshot is not None:
        require(bool(references), "snapshot acquisition requires its verified CA prerequisite")
    for key in ("sources", "requested_packages"):
        values = stage[key]
        require(isinstance(values, list) and 0 < len(values) <= MAX_FILES
                and all(isinstance(value, str) for value in values)
                and len(values) == len(set(values)), key + " must be a nonempty unique string list")
    for package in stage["requested_packages"]:
        require(matches(NAME + r"(?::amd64)?(?:=" + VERSION + r")?", package), "unsupported package request")
    scopes, _ = keyring_policy(stage)
    for source_index, source in enumerate(stage["sources"]):
        # Only typed scope declarations may generate Signed-By; no raw options.
        match = re.fullmatch(r"deb (https?://[A-Za-z0-9./:%_~-]+) ([a-z0-9][a-z0-9.-]*) ((?:[a-z0-9][a-z0-9-]*)(?: [a-z0-9][a-z0-9-]*)*)", source)
        require(match is not None, "unsupported or unsafe APT source line")
        try:
            url = urllib.parse.urlsplit(match.group(1))
            require(bool(url.hostname) and not url.username and not url.password
                    and url.port in (None, 80, 443) and not url.query and not url.fragment,
                    "unsupported APT source URL")
        except ValueError as error:
            raise InputError("invalid APT source URL") from error
        if url.hostname.rstrip(".") in ("ppa.launchpad.net", "ppa.launchpadcontent.net"):
            require(url.path.rstrip("/") == "/ubuntu-toolchain-r/test/ubuntu" and source_index in scopes,
                    "only the explicitly key-scoped Ubuntu toolchain PPA is supported")
        if snapshot is not None:
            require(match.group(1) == "https://snapshot.ubuntu.com/ubuntu/" + snapshot["timestamp"] + "/",
                    "snapshot policy must bind every source to its exact HTTPS snapshot root")
    return stage


def render_sources(stage):
    """Generate only APT's signed-by path/fingerprint grammar from typed scopes."""
    validate_stage(stage)
    scopes, _ = keyring_policy(stage)
    lines = []
    for index, source in enumerate(stage["sources"]):
        if index in scopes:
            selectors = ["/inputs/keys/" + key["name"] for key in scopes[index]]
            selectors.extend(fingerprint for key in scopes[index] for fingerprint in key["fingerprints"])
            source = "deb [signed-by=" + ",".join(selectors) + "] " + source[4:]
        lines.append(source)
    return "\n".join(lines) + "\n"


def validate_keyring_files(stage, keyring_files=None):
    """Verify supplied ASCII key bytes; APT enforces the fingerprint selectors."""
    _, definitions = keyring_policy(stage)
    supplied = {} if keyring_files is None else keyring_files
    require(isinstance(supplied, dict) and set(supplied) == set(definitions),
            "supplied key files must exactly match declared key names")
    verified = {}
    for name, definition in definitions.items():
        try:
            path = pathlib.Path(supplied[name])
            info = path.lstat()
            require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and 0 < info.st_size <= 4 * 1024 * 1024,
                    "key must be a bounded regular file, not a link")
            require(sha256(path) == definition["sha256"], "key SHA-256 mismatch: " + name)
            require(path.read_bytes().isascii(), "armored key bytes must be ASCII")
            verified[name] = path.resolve()
        except (OSError, TypeError) as error:
            raise InputError("cannot read declared key: " + str(error)) from error
    return verified


def inventory(root):
    require(not root.is_symlink() and root.is_dir(), "bundle must be a real directory")
    files = set()
    total = 0
    for path in root.rglob("*"):
        relative = path.relative_to(root).as_posix()
        info = path.lstat()
        if stat.S_ISDIR(info.st_mode):
            require(relative in ("lists", "debs", "keys"), "unexpected bundle directory: " + relative)
            continue
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
                "bundle links and special files are forbidden: " + relative)
        total += info.st_size
        files.add(relative)
        require(len(files) <= MAX_FILES and total <= MAX_BYTES, "bundle exceeds size or count limits")
    return files


def file_record(root, path):
    return {"path": path, "size": (root / path).stat().st_size, "sha256": sha256(root / path)}


def runtime_bytes(manifest):
    records = sorted(manifest["files"] + manifest["packages"], key=lambda record: record["path"])
    lines = ["schema_version\t1", "stage\t" + manifest["stage"],
             "platform\tlinux/amd64", "base_image\t" + manifest["base_image"],
             "file_count\t" + str(len(records))]
    lines.extend("requested\t" + request for request in manifest["requested_packages"])
    for reference in manifest.get("prerequisites", []):
        lines.append("prerequisite\t{stage}\t{manifest_sha256}\t{runtime_lock_sha256}".format(**reference))
    for record in records:
        lines.append("file\t{sha256}\t{size}\t{path}".format(**record))
    for package in sorted(manifest["packages"], key=lambda record: record["package"]):
        lines.append("package\t{package}\t{version}\t{architecture}\t{path}".format(**package))
    return ("\n".join(lines) + "\n").encode("ascii")


def check_records(root, manifest):
    records = manifest["files"] + manifest["packages"]
    paths = set()
    identities = set()
    for record in records:
        is_package = "package" in record
        exact_object(record, {"path", "size", "sha256"} | ({"package", "version", "architecture"} if is_package else set()), "file record")
        path = record["path"]
        allowed = (matches(r"debs/" + BASENAME + r"\.deb", path) if is_package else
                   path in ("sources.list", "base-packages.tsv", "index-targets.txt")
                   or matches(r"lists/" + BASENAME, path) or matches(r"keys/" + KEY_NAME, path))
        require(allowed and path not in paths, "unsafe, duplicate or unexpected file path")
        paths.add(path)
        require(type(record["size"]) is int and 0 < record["size"] <= MAX_BYTES
                and matches(r"[0-9a-f]{64}", record["sha256"]), "invalid file size or SHA-256")
        require((root / path).is_file() and file_record(root, path) == {key: record[key] for key in ("path", "size", "sha256")},
                "missing or tampered input: " + path)
        if is_package:
            require(matches(NAME, record["package"]) and matches(VERSION, record["version"])
                    and record["architecture"] in ("all", "amd64"), "invalid package identity")
            require(record["package"] not in identities, "duplicate package identity")
            identities.add(record["package"])
    require({"sources.list", "base-packages.tsv", "index-targets.txt"} <= paths, "missing APT context")
    require(manifest["packages"], "empty downloaded package closure")
    releases = [path[:-len("_InRelease")] for path in paths if path.endswith("_InRelease")]
    repositories = set()
    for source in manifest["sources"]:
        _, uri, suite, *_ = source.split(" ")
        url = urllib.parse.urlsplit(uri)
        repositories.add((url.netloc, url.path.rstrip("/"), suite))
    require(len(releases) == len(repositories), "missing authenticated InRelease metadata for a declared repository")
    for name in ("sources.list", "base-packages.tsv", "index-targets.txt"):
        require((root / name).stat().st_size <= 4 * 1024 * 1024, "APT context exceeds metadata size limit")
    stage = stage_document(manifest)
    _, keys = keyring_policy(stage)
    require({path for path in paths if path.startswith("keys/")} == {"keys/" + name for name in keys},
            "retained key set differs from the source-scoped declarations")
    require((root / "keys").is_dir() == bool(keys), "unexpected or missing key directory")
    validate_keyring_files(stage, {name: root / "keys" / name for name in keys})
    require((root / "sources.list").read_text() == render_sources(stage), "source descriptor mismatch")
    targets = (root / "index-targets.txt").read_text().splitlines()
    require(targets and len(targets) == len(set(targets)), "missing or duplicate APT index targets")
    for target in targets:
        require(matches(BASENAME, target) and "lists/" + target in paths and "_Packages" in target
                and any(("lists/" + target).startswith(release + "_") for release in releases),
                "missing authenticated Packages index or its release")
    for release in releases:
        require(any(("lists/" + target).startswith(release + "_") for target in targets),
                "declared repository has no retained Packages index")
    for request in manifest["requested_packages"]:
        name, _, version = request.partition("=")
        if name.endswith(":amd64"):
            name = name[:-len(":amd64")]
        require(any(package["package"] == name and (not version or package["version"] == version)
                    for package in manifest["packages"]), "requested package is absent from downloaded closure")
    return paths


def validate_materialized_inputs(directory, *, expected_stage=None):
    """Validate this bundle's bytes/inventory, not provenance or external edges.

    Call validate_prerequisites with the separately supplied bundle paths to
    verify the full ordered prefix; both resolve and the validate CLI do so.
    """
    root = pathlib.Path(directory)
    try:
        actual = inventory(root)
        require("manifest.json" in actual and "runtime.lock" in actual, "missing bundle manifests")
        require((root / "runtime.lock").stat().st_size <= 4 * 1024 * 1024, "runtime lock exceeds metadata size limit")
        manifest = load_json(root / "manifest.json")
        exact_object(manifest, STAGE_FIELDS | {"kind", "files", "packages", "runtime_lock"},
                     "input manifest", OPTIONAL_STAGE_FIELDS)
        require(manifest["kind"] == "apt-inputs", "unsupported input manifest kind")
        stage = validate_stage(stage_document(manifest))
        if expected_stage is not None:
            require(stage == validate_stage(expected_stage), "stage descriptor identity mismatch")
        require(all(isinstance(manifest[key], list) for key in ("files", "packages")), "invalid manifest record lists")
        require((root / "manifest.json").read_bytes() == encoded(manifest), "manifest JSON must be canonical")
        expected = check_records(root, manifest)
        require(actual == expected | {"manifest.json", "runtime.lock"}, "unexpected or missing bundle files")
        require(manifest["runtime_lock"] == file_record(root, "runtime.lock"), "runtime lock digest or size mismatch")
        require((root / "runtime.lock").read_bytes() == runtime_bytes(manifest), "runtime lock does not match manifest")
        return manifest
    except (ContractError, OSError, UnicodeError, TypeError, KeyError) as error:
        raise InputError(str(error)) from error


def validate_prerequisites(stage, prerequisite_bundles=None):
    """Verify external bundle bytes and an exact, ordered same-base prefix."""
    validate_stage(stage)
    references = stage.get("prerequisites", [])
    bundles = {} if prerequisite_bundles is None else prerequisite_bundles
    require(isinstance(bundles, dict) and set(bundles) == {item["stage"] for item in references},
            "prerequisite bundle paths must match the declared stage set exactly")
    verified = []
    for index, reference in enumerate(references):
        try:
            path = pathlib.Path(bundles[reference["stage"]])
            manifest = validate_materialized_inputs(path)
            require(sha256(path / "manifest.json") == reference["manifest_sha256"]
                    and manifest["runtime_lock"]["sha256"] == reference["runtime_lock_sha256"],
                    "prerequisite digest substitution")
            require(manifest["stage"] == reference["stage"]
                    and manifest["base_image"] == stage["base_image"]
                    and manifest["platform"] == stage["platform"], "prerequisite base/platform/stage mismatch")
            require(manifest.get("prerequisites", []) == references[:index],
                    "prerequisite chain is not an exact ordered prefix")
            verified.append((reference, path.resolve(), manifest))
        except (OSError, TypeError) as error:
            raise InputError("invalid prerequisite bundle: " + str(error)) from error
    acquisition = stage["acquisition"]
    if "snapshot" in acquisition and "tls_bootstrap" not in acquisition:
        require(verified and verified[0][2]["acquisition"].get("tls_bootstrap") == {"host": "snapshot.ubuntu.com"}
                and verified[0][2]["acquisition"].get("snapshot") == acquisition["snapshot"],
                "snapshot stage requires the matching verified snapshot CA bootstrap prefix")
    return verified


def finalize(root, stage):
    actual = inventory(root)
    require("resolution.tsv" in actual, "resolver did not produce package metadata")
    require((root / "resolution.tsv").stat().st_size <= 4 * 1024 * 1024, "resolution metadata is too large")
    packages = []
    paths = set()
    for line in (root / "resolution.tsv").read_text().splitlines():
        values = line.split("\t")
        require(len(values) == 4, "invalid resolved package record")
        package, version, architecture, filename = values
        require(matches(BASENAME + r"\.deb", filename), "unsafe archive filename")
        path = "debs/" + filename
        require(path in actual and path not in paths, "missing or duplicate downloaded archive")
        paths.add(path)
        packages.append(dict(file_record(root, path), package=package, version=version, architecture=architecture))
    manifest = dict(stage, kind="apt-inputs", packages=sorted(packages, key=lambda value: value["package"]),
                    files=[file_record(root, path) for path in sorted(actual - paths - {"resolution.tsv"})])
    # Validate classification before trusting any resolver-supplied filename.
    check_records(root, manifest)
    (root / "resolution.tsv").unlink()
    (root / "runtime.lock").write_bytes(runtime_bytes(manifest))
    manifest["runtime_lock"] = file_record(root, "runtime.lock")
    (root / "manifest.json").write_bytes(encoded(manifest))
    return validate_materialized_inputs(root, expected_stage=stage)


def bounded_diagnostics(value):
    """Retain bounded progress/error tails without URL credentials or queries."""
    if isinstance(value, bytes):
        value = value.decode("utf-8", errors="replace")
    text = str(value or "")

    def redact(match):
        try:
            url = urllib.parse.urlsplit(match.group(0))
            host = url.netloc.rsplit("@", 1)[-1]
            if "@" in url.netloc:
                host = "[redacted]@" + host
            return urllib.parse.urlunsplit((url.scheme, host, url.path,
                                            "[redacted]" if url.query else "",
                                            "[redacted]" if url.fragment else ""))
        except ValueError:
            return "[redacted-url]"

    text = re.sub(r"https?://[^\s<>\"']+", redact, text, flags=re.IGNORECASE)
    return ("[truncated]\n" if len(text) > 4096 else "") + text[-4096:]


def command_failure(phase, error):
    if isinstance(error, subprocess.TimeoutExpired):
        status = "timeout after {} seconds".format(error.timeout)
    elif isinstance(error, subprocess.CalledProcessError):
        status = "exit status {}".format(error.returncode)
    else:
        status = bounded_diagnostics(error)
    parts = [phase + ": " + status]
    for stream in ("stdout", "stderr"):
        value = getattr(error, stream, None)
        if value:
            parts.append(stream + ":\n" + bounded_diagnostics(value))
    return "\n".join(parts)


def resolve_stage(stage, output_directory, *, docker="docker", runner=None, timeout=1800,
                  prerequisite_bundles=None, keyring_files=None):
    """Publish only after strict acquisition AND real no-network installation.

    Timeout is one shared Docker-operation budget, plus up to 30 seconds for
    removal of this operation's named container. Buildx cancellation belongs
    to its client/session; no shared builders, caches or image IDs are pruned.
    Abrupt external SIGKILL or an unreachable daemon can still require cleanup;
    use --timeout 480 beneath a 600-second outer runner rather than killing it.
    """
    validate_stage(stage)
    require(type(timeout) is int and 0 < timeout <= 1800, "timeout must be 1..1800 seconds")
    deadline = time.monotonic() + timeout
    output = pathlib.Path(output_directory)
    require(not output.exists() and not output.is_symlink(), "output directory already exists")
    require(output.parent.is_dir(), "output parent must already exist")
    require(INSTALLER.is_file() and not INSTALLER.is_symlink(), "offline installer is missing")
    verified = validate_prerequisites(stage, prerequisite_bundles)
    key_paths = validate_keyring_files(stage, keyring_files)
    run = runner or subprocess.run
    try:
        with tempfile.TemporaryDirectory(prefix=".apt-inputs-", dir=output.parent) as directory:
            workspace = pathlib.Path(directory).resolve()
            root = workspace / "bundle"
            root.mkdir()
            root.chmod(0o755)  # The mounted public inputs must be traversable by _apt.
            require(not any(char in str(workspace) for char in ",\r\n"), "unsupported Docker bind path")
            if key_paths:
                key_directory = root / "keys"
                key_directory.mkdir()
                key_directory.chmod(0o755)
                for key_name, path in key_paths.items():
                    copied_key = key_directory / key_name
                    shutil.copyfile(path, copied_key)
                    copied_key.chmod(0o644)  # APT's sandbox user must read public keys.
                validate_keyring_files(stage, {name: key_directory / name for name in key_paths})
            (root / "sources.list").write_text(render_sources(stage))
            (root / "sources.list").chmod(0o644)
            name = "klogg-" + workspace.name.lstrip(".")
            common = [docker, "run", "--rm", "--name", name, "--platform=linux/amd64", "--user=0:0"]

            def run_command(command, phase, *, container=False):
                remaining = deadline - time.monotonic()
                require(remaining > 0, phase + ": timeout budget exhausted")
                try:
                    return run(command, check=True, capture_output=True, text=True, timeout=remaining)
                except (OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
                    cleanup = ""
                    if container and isinstance(error, (subprocess.TimeoutExpired, KeyboardInterrupt)):
                        # Killing a Docker client does not stop its container.
                        try:
                            result = run([docker, "rm", "--force", name], check=False,
                                         capture_output=True, text=True, timeout=30)
                            if result.returncode:
                                failure = subprocess.CalledProcessError(result.returncode, result.args,
                                                                        output=result.stdout, stderr=result.stderr)
                                cleanup = "\n" + command_failure("container cleanup", failure)
                        except (OSError, subprocess.SubprocessError) as cleanup_error:
                            cleanup = "\n" + command_failure("container cleanup", cleanup_error)
                    if isinstance(error, KeyboardInterrupt):
                        raise
                    raise InputError(command_failure(phase, error) + cleanup) from error

            installer = str(INSTALLER.resolve())
            require(not any(char in installer for char in ",\r\n"), "unsupported installer bind path")
            resolver_image = stage["base_image"]
            replay = []
            replay_mounts = []
            if verified:
                context = workspace / "prepared"
                context.mkdir()
                shutil.copyfile(INSTALLER, context / "installer.sh")
                instructions = ["FROM " + stage["base_image"], "COPY installer.sh /installer"]
                for index, (reference, path, prerequisite) in enumerate(verified):
                    slot = str(index).zfill(4)
                    copied = context / "prerequisites" / slot
                    shutil.copytree(path, copied)
                    validate_materialized_inputs(copied, expected_stage=stage_document(prerequisite))
                    require(sha256(copied / "manifest.json") == reference["manifest_sha256"],
                            "prerequisite changed while preparing build context")
                    location = "/prerequisites/" + slot
                    install = "/bin/sh /installer " + location + " " + reference["runtime_lock_sha256"]
                    instructions.extend(["COPY prerequisites/" + slot + "/ " + location + "/",
                                         "RUN --network=none " + install + " && rm -rf " + location])
                    replay.append(install)
                dockerfile = context / "Dockerfile"
                dockerfile.write_text("\n".join(instructions) + "\n")
                iidfile = workspace / "prepared.iid"
                # The original base must also be local for the independent
                # offline replay: buildx's container driver has its own cache.
                run_command([docker, "pull", "--platform=linux/amd64", stage["base_image"]], "pinned base acquisition")
                run_command([docker, "buildx", "build", "--platform=linux/amd64", "--network=none",
                             "--load", "--iidfile", str(iidfile), "--file", str(dockerfile), str(context)],
                            "offline prerequisite preparation")
                require(iidfile.is_file() and not iidfile.is_symlink() and iidfile.stat().st_size <= 256,
                        "prepared build did not produce a regular image ID file")
                built_id = iidfile.read_text().strip()
                require(matches(r"sha256:[0-9a-f]{64}", built_id) and not built_id.endswith("0" * 64),
                        "prepared build did not report an immutable image ID")
                inspected = run_command([docker, "image", "inspect", "--format", "{{.Id}}", built_id],
                                        "loaded prepared image inspection")
                resolver_image = inspected.stdout.strip()
                require(matches(r"sha256:[0-9a-f]{64}", resolver_image) and not resolver_image.endswith("0" * 64),
                        "prepared image was not loaded with an immutable local ID")
                replay_mounts = ["--mount", "type=bind,source=" + str(context / "prerequisites")
                                 + ",target=/prerequisites,readonly"]

            acquisition = stage["acquisition"]
            owner = "{}:{}".format(root.stat().st_uid, root.stat().st_gid)
            run_command(common + ["--pull=never" if verified else "--pull=always", "--env", "KLOGG_OUTPUT_OWNER=" + owner,
                        "--env", "KLOGG_TLS_BOOTSTRAP=" + acquisition.get("tls_bootstrap", {}).get("host", ""),
                        "--env", "KLOGG_CHECK_VALID_UNTIL=" + ("false" if "snapshot" in acquisition else "true"),
                        "--mount", "type=bind,source=" + str(root) + ",target=/inputs",
                        "--entrypoint=/bin/sh", resolver_image, "-ec", RESOLVE_SCRIPT, "resolver", *stage["requested_packages"]],
                        "acquisition", container=True)
            manifest = finalize(root, stage)
            verification = ["/installer", "/inputs", manifest["runtime_lock"]["sha256"]]
            if verified:
                proof = "set -eu\n" + "\n".join(replay) + "\nexec /bin/sh " + " ".join(verification) + "\n"
                verification = ["-ec", proof]
            run_command(common + ["--pull=never", "--network=none", "--mount",
                        "type=bind,source=" + str(root) + ",target=/inputs,readonly", "--mount",
                        "type=bind,source=" + installer + ",target=/installer,readonly"] + replay_mounts
                        + ["--entrypoint=/bin/sh", stage["base_image"]] + verification,
                        "offline verification", container=True)
            validate_materialized_inputs(root, expected_stage=stage)
            require(not output.exists() and not output.is_symlink(), "output appeared during resolution")
            root.rename(output)
            return manifest
    except (ContractError, OSError, UnicodeError, subprocess.SubprocessError) as error:
        raise InputError("APT closure materialization failed: " + str(error)) from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    resolve = commands.add_parser("resolve")
    resolve.add_argument("--stage", required=True, type=pathlib.Path)
    resolve.add_argument("--output", required=True, type=pathlib.Path)
    resolve.add_argument("--timeout", type=int, default=1800, help="Shared producer timeout in seconds (1..1800; use 480 under a 600-second outer runner)")
    resolve.add_argument("--keyring", action="append", default=[], metavar="NAME=PATH",
                         help="Locally supplied pinned public ASCII key; repeat for every declared name")
    validate = commands.add_parser("validate")
    validate.add_argument("--directory", required=True, type=pathlib.Path)
    validate.add_argument("--stage", type=pathlib.Path)
    for command in (resolve, validate):
        command.add_argument("--prerequisite", action="append", default=[], metavar="STAGE=PATH",
                             help="Verified prerequisite bundle path; repeat in any order")
    args = parser.parse_args(argv)
    try:
        bundles = {}
        for value in args.prerequisite:
            name, separator, path = value.partition("=")
            require(separator and name and path and name not in bundles, "invalid or duplicate prerequisite path argument")
            bundles[name] = pathlib.Path(path)
        if args.command == "resolve":
            keyrings = {}
            for value in args.keyring:
                name, separator, path = value.partition("=")
                require(separator and name and path and name not in keyrings, "invalid or duplicate keyring path argument")
                keyrings[name] = pathlib.Path(path)
            manifest = resolve_stage(load_json(args.stage), args.output, timeout=args.timeout,
                                     prerequisite_bundles=bundles, keyring_files=keyrings)
        else:
            manifest = validate_materialized_inputs(args.directory,
                         expected_stage=load_json(args.stage) if args.stage else None)
            validate_prerequisites(stage_document(manifest), bundles)
        print(json.dumps(manifest, sort_keys=True, indent=2))
        return 0
    except (ContractError, OSError) as error:
        print("ci_environment_inputs: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
