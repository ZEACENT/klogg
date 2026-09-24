#!/usr/bin/env python3
"""Prepare/consume the isolated same-run Linux environment package fixture.

This bootstrap is not a release attestation and does not relax normal CI helper
provenance. It builds only linux-x86_64 using the existing locked source/legal,
container toolchain, complete ADB, smoke, artifact and envelope validators. The
fixture retains the complete legal/source release closure; consumers install
only the package-required projection and the independently pinned packaging tool.
"""
from __future__ import annotations

import argparse
import copy
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

import ci_environment as core
import ci_environment_pipeline as pipeline
import build_adb_helper_legal_assets as legal
import verify_adb_helper_artifact as adb
import verify_adb_helper_envelope as envelope
from materialize_ci_environment import download, extract_archive, secure_url, _SecureRedirect
from source_publication_identity import published_source_name, validate_version

ROOT = pathlib.Path(__file__).resolve().parents[1]
TARGET = "linux-x86_64"
HELPER = "prefetch_artifacts/adb-helper"
FULL_SOURCES = "source-assets/adb-helper"
CONSUMPTION = "prefetch_artifacts/ci-environment-fixture.json"
TOOL_NAME = "linuxdeployqt-continuous-x86_64.AppImage"
MAX_FILES = 10000
MAX_BYTES = 2 * 1024**3

# Fixed container paths, never interpolated caller-controlled shell commands.
# All helper build and live smoke operations run on Linux, also for local macOS
# orchestration. Only the prefetched closure is mounted into the offline build.
BUILD_SCRIPT = r'''set -euo pipefail
python=/opt/python/cp311-cp311/bin/python3
test -x "$python"
"$python" /repo/scripts/verify_adb_helper_toolchain.py \
  --lock /repo/packaging/adb/adb-helper.lock.json --target linux-x86_64 \
  --containerized --container-image "$1"
"$python" /repo/scripts/build_adb_helper.py \
  --repository-root /repo --lock /repo/packaging/adb/adb-helper.lock.json \
  --source-root /work/sources --build-root /work/build --target linux-x86_64 \
  --artifact-root /work/fixture/prefetch_artifacts/adb-helper \
  --package-support-root /work/package-support
"$python" /repo/scripts/smoke_adb_helper.py \
  --adb /work/fixture/prefetch_artifacts/adb-helper/helpers/adb \
  --port 0 --timeout-seconds 15 \
  --json-output /work/fixture/prefetch_artifacts/adb-helper/package-smoke.json
"$python" /repo/scripts/verify_adb_helper_artifact.py \
  --lock /repo/packaging/adb/adb-helper.lock.json \
  --receipt /work/fixture/prefetch_artifacts/adb-helper/receipt.json \
  --binary-smoke-receipt /work/fixture/prefetch_artifacts/adb-helper/package-smoke.json \
  --package-root /work/fixture/prefetch_artifacts/adb-helper --asset-scope package \
  --source-assets-root /work/package-support --helper-path helpers/adb \
  --expected-target linux-x86_64 \
  --package-verification-receipt /work/fixture/prefetch_artifacts/adb-helper/package-verification.json \
  --require-lock-binding
'''


class FixtureError(core.ContractError):
    """Missing, changed or unauthenticated fixture evidence."""


def require(condition, message):
    if not condition:
        raise FixtureError(message)


def _version(value):
    require(isinstance(value, str), "fixture requires the shared KLOGG_VERSION")
    try:
        return validate_version(value)
    except ValueError as error:
        raise FixtureError(str(error)) from error


def _context(repo_root, source, runner):
    pipeline.validate_source(source)
    require(pipeline.run(["git", "rev-parse", "HEAD"], runner, cwd=repo_root).strip() == source["sha"],
            "fixture source differs from current checked-out Git commit")


def load_package_tool(repo_root):
    document = pipeline.read_document(repo_root, "ci/environments/package-tools.json")
    core._object(document, {"schema_version", "linuxdeployqt"}, "package tools")
    core._version(document, "package tools")
    pin = document["linuxdeployqt"]
    core._object(pin, {"repository", "release_id", "tag", "asset_id", "asset_name", "size", "sha256", "url"}, "linuxdeployqt pin")
    require(pin["repository"] == "probonopd/linuxdeployqt" and pin["asset_name"] == TOOL_NAME,
            "unexpected packaging tool repository or filename")
    require(isinstance(pin["tag"], str) and re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", pin["tag"]),
            "packaging tool must use a versioned release, not continuous")
    for field in ("release_id", "asset_id", "size"):
        core._positive_int(pin[field], "packaging tool " + field)
    core._digest(pin["sha256"], "packaging tool SHA-256", prefix=False)
    require(pin["size"] <= MAX_BYTES, "packaging tool exceeds fixture limit")
    require(pin["url"] == "https://github.com/" + pin["repository"] + "/releases/download/" + pin["tag"] + "/" + TOOL_NAME,
            "packaging tool URL differs from its declared versioned release")
    return pin


def _read_public_metadata(url, *, timeout):
    secure_url(url)
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json",
                                                  "User-Agent": "klogg-ci-fixture/1"})
    opener = urllib.request.build_opener(_SecureRedirect())
    with opener.open(request, timeout=timeout) as response:
        secure_url(response.geturl())
        data = response.read(core.MAX_METADATA_BYTES + 1)
    require(len(data) <= core.MAX_METADATA_BYTES, "packaging tool metadata is oversized")
    return core._parse_json(data, "official package tool asset")


def _acquire_tool(pin, destination, timeout, downloader, metadata_reader):
    url = "https://api.github.com/repos/" + pin["repository"] + "/releases/assets/" + str(pin["asset_id"])
    actual = metadata_reader(url, timeout=timeout)
    require(isinstance(actual, dict) and type(actual.get("id")) is int and actual["id"] == pin["asset_id"]
            and actual.get("name") == pin["asset_name"] and actual.get("state") == "uploaded"
            and type(actual.get("size")) is int and actual["size"] == pin["size"]
            and actual.get("digest") == "sha256:" + pin["sha256"]
            and actual.get("browser_download_url") == pin["url"],
            "official packaging asset no longer matches its immutable checked-in pin")
    destination.parent.mkdir(parents=True, exist_ok=True)
    downloader(pin["url"], destination, timeout=timeout)
    _regular(destination)
    require(destination.stat().st_size == pin["size"] and pipeline.sha256(destination) == pin["sha256"],
            "packaging tool download hash or size mismatch")
    destination.chmod(0o755)


def _regular(path):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "fixture links and special files are forbidden")
    return info


def _inventory(root):
    require(root.is_dir() and not root.is_symlink(), "fixture root must be a real directory")
    records = []
    directories = set()
    total = 0
    for path in sorted(root.rglob("*")):
        name = core._relative_path(path.relative_to(root).as_posix(), "fixture path")
        if path.is_dir() and not path.is_symlink():
            directories.add(name)
            continue
        info = _regular(path)
        if name == "fixture.json":
            continue
        total += info.st_size
        records.append({"path": name, "size": info.st_size, "sha256": pipeline.sha256(path), "mode": info.st_mode & 0o777})
        require(len(records) <= MAX_FILES and total <= MAX_BYTES, "fixture exceeds size/count limits")
    expected_directories = {parent.as_posix() for record in records for parent in pathlib.PurePosixPath(record["path"]).parents
                            if parent.as_posix() != "."}
    require(directories == expected_directories, "fixture contains extra empty directories")
    return records


def _lock(repo_root):
    path = pipeline.regular(repo_root, "packaging/adb/adb-helper.lock.json")
    _regular(path)
    document = core.load_json(path)
    require(type(document.get("schema_version")) is int and document["schema_version"] == 2, "unsupported ADB lock")
    require(isinstance(document.get("targets", {}).get(TARGET), dict), "ADB lock lacks Linux x86_64 target")
    return path, document


def _expected_paths(lock, pin):
    names = {HELPER + "/" + name for name in (*envelope.REQUIRED_RECEIPTS, "SHA256SUMS", "helpers/adb")}
    for runtime in lock["targets"][TARGET].get("usb", {}).get("runtime_files", []):
        require(isinstance(runtime, str) and pathlib.PurePosixPath(runtime).name == runtime,
                "unsafe locked Linux runtime name")
        names.add(HELPER + "/helpers/" + core._relative_path(runtime, "runtime filename"))
    names.add("tools/" + pin["asset_name"])
    for asset in legal.release_asset_plan(lock):
        for field in ("file_name", "sha256_file"):
            names.add(FULL_SOURCES + "/" + asset[field])
            if asset["distribution"]["package_required"]:
                names.add(HELPER + "/release/" + asset[field])
    names.add(FULL_SOURCES + "/adb-helper-release-assets.json")
    return names


def _verify_payload(repo_root, root, version, pin, runner):
    lock_path, lock = _lock(repo_root)
    artifact = root / HELPER
    envelope.verify_artifact_envelope(lock_path, artifact, TARGET)
    # Reuse the full existing verifier, not a parallel subset of its GLIBC,
    # private libusb, complete-client and production-server smoke contracts.
    pipeline.run([sys.executable, str(pipeline.regular(repo_root, "scripts/verify_adb_helper_artifact.py")),
                  "--lock", str(lock_path), "--receipt", str(artifact / "receipt.json"),
                  "--package-root", str(artifact), "--asset-scope", "package",
                  "--source-assets-root", str(artifact / "release"), "--helper-path", "helpers/adb",
                  "--expected-target", TARGET, "--binary-smoke-receipt", str(artifact / "package-smoke.json"),
                  "--checksum-envelope", str(artifact / "SHA256SUMS"), "--require-lock-binding"], runner, timeout=120)
    receipt = pipeline.read_document(artifact, "receipt.json")
    full = root / FULL_SOURCES
    adb.validate_source_set_receipt(lock, lock_path, receipt, full, "release")
    plan = legal.release_asset_plan(lock)
    for asset in plan:
        path = pipeline.regular(full, asset["file_name"])
        adb.verify_hash_sidecar(pipeline.regular(full, asset["sha256_file"]), pipeline.sha256(path), path.name)
    source_set_asset = next(asset for asset in plan if asset["kind"] == "source-set-receipt")
    source_set = pipeline.read_document(full, source_set_asset["file_name"])
    source_hash = source_set["archive"]["sha256"]
    source_offer = next(asset for asset in plan if asset["kind"] == "source-offer")
    lines = pipeline.regular(full, source_offer["file_name"]).read_text(encoding="utf-8").splitlines()
    require([line for line in lines if line.startswith("Published archive:")] ==
            ["Published archive: " + published_source_name(version, "adb-helper", source_hash)]
            and [line for line in lines if line.startswith("SHA-256:")] == ["SHA-256: " + source_hash],
            "fixture source offer is not bound to the shared version and corresponding source bytes")
    tool = pipeline.regular(root, "tools/" + pin["asset_name"])
    require(tool.stat().st_size == pin["size"] and pipeline.sha256(tool) == pin["sha256"]
            and tool.stat().st_mode & 0o777 == 0o755, "fixture packaging tool differs from current pin")


def _validate_fixture(repo_root, root, source, version, runner):
    document = pipeline.read_document(root, "fixture.json")
    core._object(document, {"schema_version", "kind", "source", "version", "target", "lock_sha256", "package_tool", "files"}, "Linux fixture")
    core._version(document, "Linux fixture")
    require(document["kind"] == "ci-linux-package-fixture" and document["target"] == TARGET, "unsupported fixture kind or target")
    require(core.canonical_digest(document["source"]) == core.canonical_digest(source), "fixture source/run/attempt differs")
    require(document["version"] == version, "fixture version differs from qualification version")
    lock_path, lock = _lock(repo_root)
    pin = load_package_tool(repo_root)
    require(document["lock_sha256"] == pipeline.sha256(lock_path), "fixture was built from a different ADB lock")
    require(core.canonical_digest(document["package_tool"]) == core.canonical_digest(pin), "fixture packaging tool pin is stale")
    records = document["files"]
    require(isinstance(records, list) and len(records) <= MAX_FILES, "invalid fixture file inventory")
    for record in records:
        core._object(record, {"path", "size", "sha256", "mode"}, "fixture file")
        core._relative_path(record["path"], "fixture file")
        core._digest(record["sha256"], "fixture file hash", prefix=False)
        require(type(record["size"]) is int and record["size"] >= 0 and type(record["mode"]) is int
                and record["mode"] in (0o644, 0o755), "invalid fixture file size or mode")
    require(len(records) == len({record["path"] for record in records})
            and {record["path"] for record in records} == _expected_paths(lock, pin), "fixture payload is incomplete or contains undeclared material")
    require(records == _inventory(root), "fixture payload bytes, modes or inventory changed")
    _verify_payload(repo_root, root, version, pin, runner)
    return document


def _run_build(command, name, runner, timeout):
    try:
        pipeline.run(command, runner, timeout=timeout)
    except (pipeline.PipelineError, KeyboardInterrupt) as error:
        if isinstance(error, KeyboardInterrupt) or isinstance(error.__cause__, subprocess.TimeoutExpired):
            # Killing a Docker client does not stop its container. Remove only
            # this operation's private name, never images, caches or builders.
            try:
                pipeline.run(["docker", "rm", "--force", name], runner, timeout=30)
            except pipeline.PipelineError as cleanup:
                raise FixtureError("fixture build cancellation cleanup failed: " + str(cleanup)) from error
        raise


def prepare_fixture(repo_root, source, version, output, *, runner=None, downloader=None, metadata_reader=None, timeout=5400):
    """Build one Linux-only fixture with existing production validators, atomically."""
    root, output = pathlib.Path(repo_root).resolve(), pathlib.Path(output).absolute()
    version = _version(version)
    require(type(timeout) is int and 0 < timeout <= 7200, "fixture timeout must be 1..7200 seconds")
    require(not output.exists() and not output.is_symlink(), "fixture output must be new")
    _context(root, source, runner)
    lock_path, lock = _lock(root)
    lock_sha = pipeline.sha256(lock_path)
    pin = load_package_tool(root)
    toolchain = lock.get("toolchains", {}).get(lock["targets"][TARGET].get("toolchain"))
    require(isinstance(toolchain, dict), "Linux helper toolchain is not locked")
    core._digest(toolchain.get("container_digest"), "helper toolchain container digest")
    image = str(toolchain.get("container_image")) + "@" + toolchain["container_digest"]
    require(re.fullmatch(r"[a-z0-9][a-z0-9./:_-]*@sha256:[0-9a-f]{64}", image), "invalid helper container image")
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix=".ci-linux-fixture-", dir=output.parent) as temporary:
            work = pathlib.Path(temporary).resolve()
            require(not any(char in str(path) for path in (root, work) for char in ",\r\n"), "unsupported Docker bind path")
            fixture = work / "fixture"
            fixture.mkdir()
            _acquire_tool(pin, fixture / "tools" / pin["asset_name"], min(timeout, 600),
                          downloader or download, metadata_reader or _read_public_metadata)
            prefetch = pipeline.regular(root, "scripts/prefetch_adb_helper_sources.py")
            cache = work / "source-cache"
            pipeline.run([sys.executable, str(prefetch), "--lock", str(lock_path), "--download-root", str(cache),
                          "--max-cache-bytes", "536870912"], runner, timeout=timeout, cwd=root)
            pipeline.run([sys.executable, str(prefetch), "--lock", str(lock_path), "--download-root", str(cache),
                          "--extract-root", str(work / "sources"), "--offline"], runner, timeout=timeout, cwd=root)
            pipeline.run([sys.executable, str(pipeline.regular(root, "scripts/build_adb_helper_legal_assets.py")),
                          "--lock", str(lock_path), "--archive-root", str(cache), "--repository-root", str(root),
                          "--version", version, "--base-url", "https://github.com/" + pipeline.REPOSITORY,
                          "--output", str(fixture / FULL_SOURCES), "--package-support-output", str(work / "package-support")],
                         runner, timeout=timeout, cwd=root)
            pipeline.run(["docker", "pull", "--platform=linux/amd64", image], runner, timeout=timeout)
            name = "klogg-fixture-" + work.name.lstrip(".")
            _run_build(["docker", "run", "--rm", "--name", name, "--pull=never", "--network=none", "--platform=linux/amd64",
                          "--user", "{}:{}".format(os.getuid(), os.getgid()), "--entrypoint=/bin/bash", "--workdir=/repo",
                          "--mount", "type=bind,source=" + str(root) + ",target=/repo,readonly",
                          "--mount", "type=bind,source=" + str(work) + ",target=/work",
                          "--env", "HOME=/work", "--env", "FETCHCONTENT_FULLY_DISCONNECTED=ON",
                          "--env", "FETCHCONTENT_UPDATES_DISCONNECTED=ON", image, "-lc", BUILD_SCRIPT, "fixture", image],
                         name, runner, timeout)
            artifact = fixture / HELPER
            shutil.copytree(work / "package-support", artifact / "release")
            # Exactly the existing build-adb-helper action's target envelope:
            # private runtime siblings plus the four existing verifier receipts.
            names = sorted("helpers/" + path.name for path in (artifact / "helpers").iterdir())
            names += list(envelope.REQUIRED_RECEIPTS)
            lines = []
            for name in names:
                path = pipeline.regular(artifact, name)
                _regular(path)
                lines.append(pipeline.sha256(path) + "  " + name)
            (artifact / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="ascii")
            document = {"schema_version": 1, "kind": "ci-linux-package-fixture", "source": copy.deepcopy(source),
                        "version": version, "target": TARGET, "lock_sha256": lock_sha,
                        "package_tool": pin, "files": _inventory(fixture)}
            pipeline.write_document(fixture / "fixture.json", document)
            _validate_fixture(root, fixture, source, version, runner)
            _context(root, source, runner)
            require(lock_sha == pipeline.sha256(lock_path), "ADB lock changed during fixture preparation")
            require(not output.exists() and not output.is_symlink(), "fixture output appeared concurrently")
            fixture.rename(output)
            return document
    except (OSError, ValueError, RuntimeError) as error:
        if isinstance(error, core.ContractError):
            raise
        raise FixtureError("Linux fixture preparation failed: " + str(error)) from error


def _destination(output, relative):
    core._relative_path(relative, "fixture destination")
    path = output
    for part in pathlib.PurePosixPath(relative).parts:
        path = path / part
        require(not path.is_symlink(), "fixture destination traverses a symlink")
    require(not path.exists(), "refusing to replace existing fixture destination: " + relative)
    return path


def _extract_fixture(archive, destination):
    # A fixture is much smaller and stricter than an SDK source archive. Bound
    # its expanded bytes/count before invoking the shared safe member extractor.
    try:
        with tarfile.open(archive, "r|gz") as stream:
            total = 0
            for count, member in enumerate(stream, 1):
                require(member.isdir() or member.isfile(), "fixture archive links/special files are forbidden")
                total += member.size
                require(member.size >= 0 and total <= MAX_BYTES and count <= MAX_FILES * 2,
                        "fixture expanded archive exceeds size/count limits")
        extract_archive(archive, destination, "tar", timeout=120)
    except (tarfile.TarError, EOFError) as error:
        raise FixtureError("invalid fixture tar transport: " + str(error)) from error


def consume_fixture(repo_root, source, fixture_root, fixture_artifact_id, archive_sha256, output, *, runner=None, version=None):
    """Authenticate same-run transport and install only its verified Linux inputs."""
    root, fixture_root, output = pathlib.Path(repo_root).resolve(), pathlib.Path(fixture_root), pathlib.Path(output).absolute()
    version = _version(version if version is not None else os.environ.get("KLOGG_VERSION"))
    _context(root, source, runner)
    require(output.is_dir() and not output.is_symlink(), "fixture consumer output must be an existing real workspace")
    core._digest(archive_sha256, "external fixture archive SHA-256", prefix=False)
    require(not fixture_root.is_symlink() and fixture_root.is_dir()
            and {path.name for path in fixture_root.iterdir()} == {"fixture.tar.gz"}, "fixture transport must contain exactly fixture.tar.gz")
    archive = pipeline.regular(fixture_root, "fixture.tar.gz")
    info = _regular(archive)
    require(0 < info.st_size <= MAX_BYTES and pipeline.sha256(archive) == archive_sha256, "fixture transport archive hash or size mismatch")
    pipeline.verify_artifact(fixture_artifact_id, source, runner)
    destinations = [_destination(output, name) for name in (HELPER, "tools/" + TOOL_NAME, CONSUMPTION)]
    moved = []
    created = []
    try:
        with tempfile.TemporaryDirectory(prefix=".consume-ci-fixture-", dir=output) as temporary:
            work = pathlib.Path(temporary)
            extracted = work / "extracted"
            _extract_fixture(archive, extracted)
            document = _validate_fixture(root, extracted, source, version, runner)
            _context(root, source, runner)
            require(pipeline.sha256(archive) == archive_sha256, "fixture transport changed while validating")
            receipt = {"schema_version": 1, "kind": "consumed-linux-package-fixture",
                       "fixture_artifact_id": fixture_artifact_id, "archive_sha256": archive_sha256,
                       "fixture": document}
            retained = work / "consumption.json"
            pipeline.write_document(retained, receipt)
            origins = [extracted / HELPER, extracted / "tools" / TOOL_NAME, retained]
            # Validate everything before publishing any consumer path. If a
            # later move fails, remove only this transaction's own new paths.
            for origin, destination in zip(origins, destinations):
                _destination(output, destination.relative_to(output).as_posix())
                if not destination.parent.exists():
                    destination.parent.mkdir()
                    created.append(destination.parent)
                origin.rename(destination)
                moved.append(destination)
            return receipt
    except (OSError, ValueError, RuntimeError, KeyboardInterrupt) as error:
        for path in reversed(moved):
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink()
        for path in reversed(created):
            if path.is_dir() and not any(path.iterdir()):
                path.rmdir()
        if isinstance(error, (core.ContractError, KeyboardInterrupt)):
            raise
        raise FixtureError("Linux fixture consumption failed: " + str(error)) from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("prepare-fixture", "consume-fixture"):
        command = commands.add_parser(name)
        command.add_argument("--repo-root", type=pathlib.Path, default=ROOT)
        command.add_argument("--source", type=pathlib.Path, required=True)
        command.add_argument("--output", type=pathlib.Path, required=True)
        if name == "prepare-fixture":
            command.add_argument("--version", required=True)
        else:
            command.add_argument("--fixture-root", type=pathlib.Path, required=True)
            command.add_argument("--fixture-artifact-id", type=int, required=True)
            command.add_argument("--archive-sha256", required=True)
    args = parser.parse_args(argv)
    try:
        source = core.load_json(args.source)
        if args.command == "prepare-fixture":
            result = prepare_fixture(args.repo_root, source, args.version, args.output)
        else:
            result = consume_fixture(args.repo_root, source, args.fixture_root, args.fixture_artifact_id,
                                     args.archive_sha256, args.output)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except (core.ContractError, OSError) as error:
        print("ci_environment_fixture: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
