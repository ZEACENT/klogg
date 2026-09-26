#!/usr/bin/env python3
"""Acquire declared CI inputs; never parse shell recipes or fall back to host SDKs.

The checked-in materials catalog owns source URLs, exact SHA-256/base pins, APT
requests and archive layout. APT runs only through the container resolver and its
independent offline replay. The final schema-1 envelope and material directory
appear together, only after validation. Network/process callables are test seams,
not consumer overrides. --timeout sets the download socket and extraction
timeouts; the APT resolver shares its budget across its own Docker operations.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
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
import urllib.parse
import urllib.request
import zipfile

import ci_environment as ci
import ci_environment_inputs as apt
from prefetch_adb_helper_sources import normalized_archive_parts

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROVENANCE = "inputs/material-manifest.json"
TOOLS = "inputs/tools.tar"
QT_PREFIX = "/opt/klogg-tools/qt/6.9.3/gcc_64"
MAX_MEMBERS = 300000
MAX_ARCHIVE_BYTES = 20 * 1024**3
MAX_LISTING_BYTES = 128 * 1024**2


class MaterializeError(ci.ContractError):
    """Declared producer inputs could not be acquired or validated."""


def require(condition, message):
    if not condition:
        raise MaterializeError(message)


def fields(value, required, label, optional=()):
    require(isinstance(value, dict) and set(required) <= set(value) <= set(required) | set(optional),
            label + " has missing or unknown fields")


def relative(value, *, allow_dot=False):
    return ci._relative_path(value, "material path", allow_dot=allow_dot)


def regular(path):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
            "material must be a regular non-link file: " + str(path))
    return info


def secure_url(value):
    require(isinstance(value, str) and not any(ord(char) < 33 for char in value), "invalid acquisition URL")
    try:
        url = urllib.parse.urlsplit(value)
        require(url.scheme == "https" and url.hostname and not url.username and not url.password
                and url.port in (None, 443) and not url.fragment, "acquisition requires public HTTPS: " + value)
    except ValueError as error:
        raise MaterializeError("invalid acquisition URL: " + str(error)) from error
    return value


def load_definition(repo_root, family):
    """Validate explicit declarations without interpreting Dockerfile commands."""
    root = pathlib.Path(repo_root)
    catalog = ci.load_json(ci._regular_recipe_file(root, "ci/environments/recipes.json"))
    ci.validate_catalog(catalog)
    document = ci.load_json(ci._regular_recipe_file(root, "ci/environments/materials.json"))
    fields(document, {"schema_version", "assets", "families"}, "materials catalog")
    require(type(document["schema_version"]) is int and document["schema_version"] == 1, "materials require schema 1")
    require(isinstance(document["families"], dict) and isinstance(document["assets"], dict), "invalid materials maps")
    require(set(document["families"]) == set(catalog["families"]), "materials must cover the exact recipe family set")
    require(isinstance(family, str) and family in document["families"], "unknown material family")
    definition = document["families"][family]
    fields(definition, {"base_image", "build_args", "apt_stages", "downloads", "tools"}, "family materials")
    base = definition["base_image"]
    require(isinstance(base, str) and re.fullmatch(r"[a-z0-9][a-z0-9./:_-]*@sha256:[0-9a-f]{64}", base)
            and not base.endswith("0" * 64), "base image must be an exact nonzero digest")
    require(isinstance(definition["build_args"], dict), "invalid material build arguments")
    arguments = dict(catalog["families"][family].get("build_args", {}))
    for name, value in dict(definition["build_args"], UBUNTU_IMAGE=base).items():
        require(isinstance(name, str) and re.fullmatch(r"[A-Z][A-Z0-9_]*", name)
                and isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_./:@+%=-]+", value), "unsafe material build argument")
        require(name not in arguments or arguments[name] == value, "material argument conflicts with fixed catalog: " + name)
        arguments[name] = value
    require("UBUNTU_IMAGE" not in definition["build_args"]
            and not any(name.startswith("APT_") or name == "TOOLS_ARCHIVE_SHA256" for name in definition["build_args"]),
            "base and lock arguments are derived, not caller supplied")
    stages = definition["apt_stages"]
    require(isinstance(stages, list) and bool(stages), "family requires explicit APT stages")
    paths = {PROVENANCE: "file"}
    if definition["tools"]:
        paths[TOOLS] = "file"
    previous = {}
    lock_args = set()

    def reserve(path, kind):
        relative(path)
        require(not any(path == other or path.startswith(other + "/") or other.startswith(path + "/")
                        for other in paths), "overlapping material paths: " + path)
        paths[path] = kind

    for stage in stages:
        fields(stage, {"stage", "path", "build_arg", "sources", "requested_packages", "acquisition", "prerequisites"},
               "APT stage declaration", {"source_keyrings"})
        name = stage["stage"]
        ci._identifier(name, "APT stage")
        require(name not in previous, "duplicate APT stage")
        refs = stage["prerequisites"]
        require(isinstance(refs, list) and all(isinstance(ref, str) and ref in previous for ref in refs)
                and len(refs) == len(set(refs)), "APT prerequisites must be unique earlier stages")
        for index, ref in enumerate(refs):
            require(previous[ref]["prerequisites"] == refs[:index], "APT prerequisites must form an exact ordered prefix")
        argument = stage["build_arg"]
        require(isinstance(argument, str) and re.fullmatch(r"APT_[A-Z0-9_]+_LOCK_SHA256", argument)
                and argument not in lock_args and argument not in arguments, "duplicate or invalid derived APT argument")
        lock_args.add(argument)
        reserve(stage["path"], "directory")
        declaration = _stage_descriptor(stage, base, [])
        _validate_declared_stage(declaration, refs, previous)
        _keyring_files(root, declaration)
        previous[name] = stage
    selected_assets = {}
    for key in ("downloads", "tools"):
        require(isinstance(definition[key], list), key + " must be a list")
        for entry in definition[key]:
            fields(entry, {"asset", "path"} if key == "downloads" else
                   {"asset", "format", "source_root", "destination"}, key + " declaration")
            asset = entry["asset"]
            require(isinstance(asset, str) and asset in document["assets"], "unknown source asset")
            ci._identifier(asset, "asset")
            record = document["assets"][asset]
            fields(record, {"url", "sha256", "checksum_source", "license", "source_url"}, "asset")
            secure_url(record["url"])
            secure_url(record["source_url"])
            ci._digest(record["sha256"], "source SHA-256", prefix=False)
            require(all(isinstance(record[field], str) and bool(record[field]) for field in ("license", "checksum_source")),
                    "source license and checksum provenance are required")
            selected_assets[asset] = record
            if key == "downloads":
                reserve(entry["path"], "file")
            else:
                require(entry["format"] in ("tar", "zip", "7z", "file"), "unsupported tool source format")
                relative(entry["source_root"], allow_dot=True)
                relative(entry["destination"])
                require(entry["format"] != "file" or entry["source_root"] == ".", "file source cannot strip a root")
    return catalog, definition, selected_assets


def _validate_declared_stage(stage, references, previous):
    """Preflight source policy without inventing not-yet-resolved bundle hashes."""
    fields(stage["acquisition"], {"mode"}, "APT acquisition policy", {"snapshot", "tls_bootstrap"})
    checked = copy.deepcopy(stage)
    if references:
        require("tls_bootstrap" not in stage["acquisition"], "only the first stage may bootstrap snapshot TLS")
        if "snapshot" in stage["acquisition"]:
            bootstrap = previous[references[0]]
            policy = bootstrap["acquisition"]
            require(policy.get("tls_bootstrap") == {"host": "snapshot.ubuntu.com"}
                    and stage["acquisition"] == {key: value for key, value in policy.items() if key != "tls_bootstrap"}
                    and stage["sources"] == bootstrap["sources"],
                    "snapshot dependents must use their CA bootstrap's exact snapshot sources and expiry policy")
            # The first stage already validated the exact snapshot URL/policy.
            # Validate the remaining grammar without pretending a manifest exists.
            checked["acquisition"] = {"mode": "apt-signed"}
    apt.validate_stage(checked)


def _stage_descriptor(declaration, base, references):
    stage = {key: copy.deepcopy(declaration[key]) for key in
             ("stage", "sources", "requested_packages", "acquisition", "source_keyrings") if key in declaration}
    stage.update(schema_version=1, platform="linux/amd64", base_image=base)
    if references:
        stage["prerequisites"] = references
    return stage


def _keyring_files(root, stage):
    _, keys = apt.keyring_policy(stage)
    paths = {name: ci._regular_recipe_file(root, "ci/environments/keys/" + name) for name in keys}
    return apt.validate_keyring_files(stage, paths)


class _SecureRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        secure_url(new_url)
        return super().redirect_request(request, fp, code, message, headers, new_url)


def download(url, destination, *, timeout):
    """Use normal TLS verification; never discover a replacement URL or hash."""
    secure_url(url)
    opener = urllib.request.build_opener(_SecureRedirect())
    request = urllib.request.Request(url, headers={"User-Agent": "klogg-ci-materializer/1"})
    with opener.open(request, timeout=timeout) as response, destination.open("xb") as output:
        secure_url(response.geturl())
        total = 0
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            require(total <= MAX_ARCHIVE_BYTES, "download exceeds material size limit")
            output.write(chunk)
    require(total > 0, "download is empty")


def _member_name(name):
    # A leading './' and trailing directory slash are conventional in tar files.
    while name.startswith("./"):
        name = name[2:]
    return relative(name.rstrip("/"))


def _preflight(members):
    """Reject any layout that could write through a file/link during extraction."""
    entries = {}
    total = 0
    for name, kind, size in members:
        name = _member_name(name)
        require(name not in entries, "duplicate archive member: " + name)
        require(kind in ("file", "directory", "symlink") and type(size) is int and size >= 0,
                "unsupported archive member type or size")
        entries[name] = (kind, size)
        total += size
        require(len(entries) <= MAX_MEMBERS and total <= MAX_ARCHIVE_BYTES, "archive exceeds extraction limits")
    require(entries, "archive contains no members")
    for name in entries:
        for parent in pathlib.PurePosixPath(name).parents:
            if parent.as_posix() in entries:
                require(entries[parent.as_posix()][0] == "directory", "archive member has a file/link ancestor")
    return entries


def _link_target(name, target):
    require(isinstance(target, str) and target and not any(ord(char) < 32 for char in target), "invalid archive link target")
    require(not pathlib.PurePosixPath(target).is_absolute() and "\\" not in target, "absolute or non-POSIX archive link")
    try:
        normalized_archive_parts((pathlib.PurePosixPath(name).parent / target).as_posix(), "link target")
    except RuntimeError as error:
        raise MaterializeError(str(error)) from error


def validate_tree(root):
    """Inspect without following links; then check complete link-chain containment."""
    require(root.is_dir() and not root.is_symlink(), "tool tree must be a real directory")
    result = {}
    total = 0
    for path in root.rglob("*"):
        name = relative(path.relative_to(root).as_posix())
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode):
            kind = "symlink"
            _link_target(name, os.readlink(path))
            try:
                path.resolve(strict=False).relative_to(root.resolve())
            except (OSError, RuntimeError, ValueError) as error:
                raise MaterializeError("archive link chain escapes or cycles: " + name) from error
        elif stat.S_ISDIR(info.st_mode):
            kind = "directory"
        else:
            regular(path)
            kind = "file"
            total += info.st_size
        result[name] = (kind, info.st_size if kind == "file" else 0)
        require(len(result) <= MAX_MEMBERS and total <= MAX_ARCHIVE_BYTES, "tool tree exceeds size/count limits")
    return result


def _check_extracted(destination, entries):
    actual = validate_tree(destination)
    expected = dict(entries)
    for name in entries:
        for parent in pathlib.PurePosixPath(name).parents:
            if parent.as_posix() != ".":
                expected.setdefault(parent.as_posix(), ("directory", 0))
    require(set(actual) == set(expected), "extracted archive inventory differs from preflight")
    for name, (kind, size) in expected.items():
        require(actual[name][0] == kind and (kind != "file" or actual[name][1] == size),
                "extracted member type or size mismatch: " + name)
        if kind != "symlink":
            path = destination / name
            path.chmod(0o755 if kind == "directory" or path.stat().st_mode & 0o111 else 0o644)


def _extract_7z(archive, destination, runner, timeout):
    result = runner(["7z", "l", "-slt", "-ba", str(archive)], check=True,
                    capture_output=True, text=True, timeout=timeout)
    require(result.returncode == 0 and isinstance(result.stdout, str)
            and len(result.stdout.encode("utf-8")) <= MAX_LISTING_BYTES, "invalid or oversized 7z inventory")
    members = []
    for block in result.stdout.strip("\r\n").split("\n\n"):
        record = {}
        for line in block.splitlines():
            key, separator, value = line.partition(" = ")
            require(separator and key not in record, "ambiguous 7z inventory record")
            record[key] = value
        require({"Path", "Size", "Attributes", "Encrypted"} <= set(record)
                and record["Encrypted"] == "-" and not record.get("Hard Link")
                and not record.get("Alternate Stream"), "unsupported 7z member metadata")
        attributes = record["Attributes"].split()[-1]
        require(re.fullmatch(r"[-dl][rwxstST-]{9}", attributes), "7z member lacks a supported Unix type")
        require(re.fullmatch(r"[0-9]+", record["Size"]), "invalid 7z member size")
        kind = {"-": "file", "d": "directory", "l": "symlink"}[attributes[0]]
        members.append((record["Path"], kind, int(record["Size"])))
    entries = _preflight(members)
    # The destination is empty and no member can have a symlink ancestor.
    # Therefore even a malicious link target cannot cause an external write.
    # Link targets (stored as 7z payloads, not listing fields) are checked before
    # any extracted tree is traversed for merging or copied into build inputs.
    runner(["7z", "x", "-y", "-bd", "-bb0", "-o" + str(destination), "--", str(archive)],
           check=True, capture_output=True, text=True, timeout=timeout)
    return entries


def extract_archive(archive, destination, format_name, *, timeout, runner=None):
    """Preflight every member, extract in isolation, and validate exact results."""
    archive, destination = pathlib.Path(archive), pathlib.Path(destination)
    require(not destination.exists() and not destination.is_symlink(), "extraction destination must be new")
    regular(archive)
    destination.mkdir(parents=True)
    try:
        if format_name == "7z":
            entries = _extract_7z(archive, destination, runner or subprocess.run, timeout)
        elif format_name == "tar":
            with tarfile.open(archive, "r:*") as source:
                members = []
                records = []
                for member in source:
                    if member.name.rstrip("/") == "." and member.isdir():
                        continue
                    kind = "directory" if member.isdir() else "symlink" if member.issym() else "file" if member.isfile() else "unsupported"
                    members.append((member.name, kind, member.size))
                    records.append(member)
                    require(len(records) <= MAX_MEMBERS, "too many tar members")
                entries = _preflight(members)
                for member in records:
                    name = _member_name(member.name)
                    target = destination / name
                    if member.issym():
                        _link_target(name, member.linkname)
                    elif member.isdir():
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with source.extractfile(member) as stream, target.open("xb") as output:
                            shutil.copyfileobj(stream, output)
                        target.chmod(0o755 if member.mode & 0o111 else 0o644)
                for member in records:
                    if member.issym():
                        target = destination / _member_name(member.name)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.symlink_to(member.linkname)
        elif format_name == "zip":
            with zipfile.ZipFile(archive) as source:
                members = source.infolist()
                rows = []
                for member in members:
                    mode = member.external_attr >> 16
                    require(not member.flag_bits & 1 and stat.S_IFMT(mode) in (0, stat.S_IFREG, stat.S_IFDIR),
                            "zip links, encryption and special files are unsupported")
                    rows.append((member.filename, "directory" if member.is_dir() else "file", member.file_size))
                entries = _preflight(rows)
                for member in members:
                    target = destination / _member_name(member.filename)
                    if member.is_dir():
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        with source.open(member) as stream, target.open("xb") as output:
                            shutil.copyfileobj(stream, output)
                        target.chmod(0o755 if (member.external_attr >> 16) & 0o111 else 0o644)
        else:
            raise MaterializeError("unsupported archive format")
        _check_extracted(destination, entries)
    except (OSError, RuntimeError, ValueError, tarfile.TarError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        raise MaterializeError("safe archive extraction failed: " + str(error)) from error


def merge_tree(source, destination):
    """Merge independent vendor modules, allowing only byte-identical overlap."""
    entries = validate_tree(source)
    require(not destination.is_symlink(), "tool destination cannot be a symlink")
    destination.mkdir(parents=True, exist_ok=True)
    for name, (kind, _) in sorted(entries.items(), key=lambda item: (item[1][0] == "symlink", item[0])):
        origin, target = source / name, destination / name
        for parent in target.relative_to(destination).parents:
            require(not (destination / parent).is_symlink(), "tool merge would traverse a link")
        exists = target.exists() or target.is_symlink()
        if kind == "directory":
            require(not exists or (target.is_dir() and not target.is_symlink()), "tool directory collision")
            target.mkdir(parents=True, exist_ok=True)
        elif exists:
            if kind == "symlink":
                require(target.is_symlink() and os.readlink(origin) == os.readlink(target), "tool symlink collision")
            else:
                regular(target)
                require(apt.sha256(origin) == apt.sha256(target)
                        and bool(origin.stat().st_mode & 0o111) == bool(target.stat().st_mode & 0o111), "tool content collision")
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            if kind == "symlink":
                target.symlink_to(os.readlink(origin))
            else:
                shutil.copyfile(origin, target)
                target.chmod(origin.stat().st_mode & 0o777)
    validate_tree(destination)


def relocate_qt(qt):
    """Qt6 uses qt.conf for runtime relocation; never execute host-incompatible ELF."""
    qt = pathlib.Path(qt)
    validate_tree(qt)
    for relative_path in ("bin/qmake", "mkspecs/qconfig.pri"):
        require((qt / relative_path).is_file(), "Qt kit lacks " + relative_path)
    changes = []

    def replace(path, data):
        regular(path)
        old = path.read_bytes()
        if old != data:
            path.write_bytes(data)
            changes.append({"path": path.relative_to(qt).as_posix(), "before_sha256": hashlib.sha256(old).hexdigest(),
                            "after_sha256": hashlib.sha256(data).hexdigest()})

    config = qt / "bin/qt.conf"
    data = b"[Paths]\nPrefix=..\n"
    if config.exists() or config.is_symlink():
        replace(config, data)
    else:
        config.write_bytes(data)
        changes.append({"path": "bin/qt.conf", "before_sha256": None, "after_sha256": hashlib.sha256(data).hexdigest()})
    qconfig = qt / "mkspecs/qconfig.pri"
    text = qconfig.read_text(encoding="utf-8")
    require(re.search(r"(?m)^QT_VERSION\s*=\s*6\.9\.3\s*$", text), "Qt configuration is not version 6.9.3")
    # The pinned Qt6 kit has no Qt5 edition/licheck fields. Relocation must not
    # invent licensing metadata or alter the vendor's feature configuration.
    # Qt6 CMake files compute their prefix relative to their own location. Only
    # vendor metadata containing the documented build prefix needs rewriting.
    for pattern in ("lib/pkgconfig/*.pc", "lib/*.prl", "lib/*.la"):
        for path in sorted(qt.glob(pattern)):
            regular(path)
            old = path.read_bytes()
            replace(path, old.replace(b"/home/qt/work/install", QT_PREFIX.encode("ascii")))
    return {"operation": "qt6-relocation-v1", "target_prefix": QT_PREFIX, "changes": changes}


def write_tools_tar(tree, destination):
    """Sorted POSIX tar, normalized ownership/time/modes, retained internal links."""
    entries = validate_tree(tree)
    with tarfile.open(destination, "w", format=tarfile.PAX_FORMAT) as archive:
        for name, (kind, size) in sorted(entries.items()):
            path = tree / name
            member = tarfile.TarInfo(name)
            member.uid = member.gid = member.mtime = 0
            member.uname = member.gname = "root"
            member.mode = 0o755 if kind == "directory" or path.lstat().st_mode & 0o111 else 0o644
            if kind == "directory":
                member.type = tarfile.DIRTYPE
                archive.addfile(member)
            elif kind == "symlink":
                member.type = tarfile.SYMTYPE
                member.linkname = os.readlink(path)
                member.mode = 0o777
                archive.addfile(member)
            else:
                member.size = size
                with path.open("rb") as stream:
                    archive.addfile(member, stream)


def materialize(repo_root, family, output, *, timeout=1800, downloader=None, resolver=None, runner=None):
    """Return the validated schema-1 envelope after atomically exposing output.

    Output must not already exist. Test seams have the same fail-closed byte and
    descriptor checks as production. No build argument/source override is exposed.
    """
    root, output = pathlib.Path(repo_root).resolve(), pathlib.Path(output).absolute()
    require(type(timeout) is int and 0 < timeout <= 1800, "timeout must be 1..1800 seconds")
    require(not output.exists() and not output.is_symlink(), "output must be new; refusing material reuse")
    catalog, definition, assets = load_definition(root, family)
    identity = ci.canonical_digest([catalog, definition, assets])
    fetch, resolve = downloader or download, resolver or apt.resolve_stage
    output.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix=".materialize-", dir=output.parent) as temporary:
            workspace = pathlib.Path(temporary)
            published = workspace / "published"
            materials = published / "materials"
            materials.mkdir(parents=True)
            acquired = workspace / "acquired"
            acquired.mkdir()
            for asset, record in sorted(assets.items()):
                path = acquired / asset
                fetch(record["url"], path, timeout=timeout)
                require(0 < regular(path).st_size <= MAX_ARCHIVE_BYTES and apt.sha256(path) == record["sha256"],
                        "source SHA-256 mismatch: " + asset)
                path.chmod(0o644)
            arguments = dict(catalog["families"][family].get("build_args", {}))
            arguments.update(definition["build_args"])
            arguments["UBUNTU_IMAGE"] = definition["base_image"]
            completed, bundles = {}, []
            for declaration in definition["apt_stages"]:
                names = declaration["prerequisites"]
                references = [completed[name][0] for name in names]
                prerequisites = {name: completed[name][1] for name in names}
                stage = _stage_descriptor(declaration, definition["base_image"], references)
                apt.validate_stage(stage)
                destination = materials / declaration["path"]
                destination.parent.mkdir(parents=True, exist_ok=True)
                kwargs = {"timeout": timeout, "prerequisite_bundles": prerequisites,
                          "keyring_files": _keyring_files(root, stage)}
                if runner is not None:
                    kwargs["runner"] = runner
                resolve(stage, destination, **kwargs)
                # Never trust a resolver return value in lieu of actual bytes.
                manifest = apt.validate_materialized_inputs(destination, expected_stage=stage)
                apt.validate_prerequisites(stage, prerequisites)
                digest = apt.sha256(destination / "manifest.json")
                runtime = manifest["runtime_lock"]["sha256"]
                arguments[declaration["build_arg"]] = runtime
                completed[stage["stage"]] = ({"stage": stage["stage"], "manifest_sha256": digest,
                                              "runtime_lock_sha256": runtime}, destination)
                bundles.append({"path": declaration["path"], "manifest_sha256": digest,
                                "build_arg": declaration["build_arg"], "manifest": manifest})
            transforms = []
            for declaration in definition["downloads"]:
                destination = materials / declaration["path"]
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(acquired / declaration["asset"], destination)
                require(apt.sha256(destination) == assets[declaration["asset"]]["sha256"], "source changed during staging")
            if definition["tools"]:
                tree = workspace / "tools"
                tree.mkdir()
                for index, declaration in enumerate(definition["tools"]):
                    source = acquired / declaration["asset"]
                    require(apt.sha256(source) == assets[declaration["asset"]]["sha256"], "source changed before extraction")
                    destination = tree / declaration["destination"]
                    if declaration["format"] == "file":
                        require(not destination.exists() and not destination.is_symlink(), "tool file destination collision")
                        for parent in destination.relative_to(tree).parents:
                            require(not (tree / parent).is_symlink(), "tool file destination has a link ancestor")
                        destination.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copyfile(source, destination)
                    else:
                        extracted = workspace / ("extract-" + str(index))
                        extract_archive(source, extracted, declaration["format"], timeout=timeout, runner=runner)
                        source_root = extracted / declaration["source_root"]
                        require(source_root.is_dir() and not source_root.is_symlink(), "declared archive root is missing")
                        if declaration["source_root"] != ".":
                            require(list(extracted.iterdir()) == [source_root], "archive has undeclared top-level content")
                        merge_tree(source_root, destination)
                        shutil.rmtree(extracted)
                    transforms.append(dict(declaration, operation="declared-tool-layout-v1"))
                transforms.append(relocate_qt(tree / "qt/6.9.3/gcc_64"))
                for path in ("cmake/bin/cmake", "bin/ninja", "boost/boost/version.hpp",
                             "qt/6.9.3/gcc_64/lib/cmake/Qt6/Qt6Config.cmake",
                             "qt/6.9.3/gcc_64/lib/cmake/Qt6Core5Compat/Qt6Core5CompatConfig.cmake"):
                    require((tree / path).is_file(), "tools tree lacks required file: " + path)
                destination = materials / TOOLS
                destination.parent.mkdir(parents=True, exist_ok=True)
                write_tools_tar(tree, destination)
                arguments["TOOLS_ARCHIVE_SHA256"] = apt.sha256(destination)
                transforms.append({"operation": "deterministic-posix-tar-v1", "path": TOOLS,
                                   "sha256": arguments["TOOLS_ARCHIVE_SHA256"]})
            provenance = {"schema_version": 1, "kind": "ci-material-acquisition", "family": family,
                          "definition": definition, "assets": assets, "transforms": transforms}
            manifest_path = materials / PROVENANCE
            manifest_path.parent.mkdir(parents=True, exist_ok=True)
            manifest_path.write_bytes(apt.encoded(provenance))
            records = []
            for path in sorted(materials.rglob("*")):
                if path.is_dir() and not path.is_symlink():
                    continue
                regular(path)
                records.append(apt.file_record(materials, path.relative_to(materials).as_posix()))
            envelope = {"schema_version": 1, "family": family, "platform": "linux/amd64",
                        "build_args": arguments, "files": records, "apt_bundles": bundles}
            # Share the consumer's exact byte/inventory/ARG contract, not a
            # weaker parallel implementation or a speculative Docker build.
            from build_ci_environment import _validate_inputs
            _validate_inputs(envelope, family, catalog["families"][family], materials,
                             root / catalog["families"][family]["dockerfile"])
            (published / "inputs.json").write_bytes(apt.encoded(envelope))
            require(identity == ci.canonical_digest(list(load_definition(root, family))), "material declarations changed during acquisition")
            require(not output.exists() and not output.is_symlink(), "output appeared during acquisition")
            published.rename(output)
            return envelope
    except (OSError, ValueError, UnicodeError, subprocess.SubprocessError) as error:
        raise MaterializeError("CI input materialization failed: " + str(error)) from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--family", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args(argv)
    try:
        envelope = materialize(args.repo_root, args.family, args.output, timeout=args.timeout)
        print(json.dumps(envelope, sort_keys=True, indent=2))
        return 0
    except (ci.ContractError, OSError) as error:
        print("materialize_ci_environment: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
