#!/usr/bin/env python3
"""Read-only proof of the Qt source payload shipped inside a TSan image.

The trusted checkout supplies archive pins and exact recipe/patch/instruction
identities. Archives are hashed and their license members are read, never
extracted, patched, built, imported or executed. No network access is used.
This is a technical source-input proof, not a legal compliance determination or
an independent reauthentication of APT provenance. Qualification should pass
--require-locked; explicit legacy online builds can omit that flag.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import lzma
import pathlib
import re
import stat
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
RECIPE = pathlib.PurePosixPath("docker/ubuntu22.04-tsan")
MODULES = {"qtbase": "QTBASE_SHA256", "qtsvg": "QTSVG_SHA256",
           "qttools": "QTTOOLS_SHA256", "qttranslations": "QTTRANSLATIONS_SHA256"}
PATCH = "fix_qt5_qobject_tsan_publication.patch"
REPOSITORY_FILES = {
    "README.md": RECIPE / "README.md",
    "recipe/Dockerfile": RECIPE / "Dockerfile",
    "recipe/apt_snapshot_retry.sh": RECIPE / "apt_snapshot_retry.sh",
    "recipe/verify_elf_runtime_closure.sh": RECIPE / "verify_elf_runtime_closure.sh",
    "patches/" + PATCH: RECIPE / "patches" / PATCH,
}
OFFLINE_HELPER = "recipe/ci_install_locked_apt.sh"
APT_MANIFESTS = ("bootstrap-inputs.json", "source-inputs.json", "qt-builder-inputs.json", "runtime-inputs.json")
MAX_METADATA_BYTES = 4 * 1024 * 1024
MAX_LICENSE_BYTES = 2 * 1024 * 1024
MAX_ARCHIVE_BYTES = 2 * 1024**3


class SourceProofError(ValueError):
    """The retained source payload does not match the trusted build inputs."""


def require(condition, message):
    if not condition:
        raise SourceProofError(message)


def regular(path):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "source proof forbids links or special files: " + str(path))
    return info


def sha256(path, limit=MAX_METADATA_BYTES):
    before = regular(path)
    require(before.st_size <= limit, "source proof file exceeds size limit: " + str(path))
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        read = 0
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            read += len(block)
            require(read <= limit, "source proof file grew beyond size limit")
            digest.update(block)
    after = regular(path)
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
            == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns), "source proof file changed during hashing")
    return digest.hexdigest()


def repository_file(root, relative):
    path = root
    for part in relative.parts:
        path = path / part
        require(not path.is_symlink(), "trusted recipe path must not traverse a symlink")
    regular(path)
    return path


def inventory(root):
    require(root.is_dir() and not root.is_symlink(), "Qt sources must be a real directory")
    directories = {"archives", "licenses", "recipe", "patches", "apt"}
    directories.update("licenses/" + module for module in MODULES)
    found_directories = set()
    files = set()
    for path in root.rglob("*"):
        relative = path.relative_to(root).as_posix()
        if stat.S_ISDIR(path.lstat().st_mode):
            require(relative in directories, "unexpected expanded source directory: " + relative)
            found_directories.add(relative)
        else:
            regular(path)
            files.add(relative)
            require(len(files) <= 512, "unexpectedly large source payload file inventory")
    require(found_directories == directories, "source payload is missing a required directory")
    return files


def recipe_defaults(path):
    require(regular(path).st_size <= MAX_METADATA_BYTES, "Dockerfile exceeds metadata size limit")
    text = path.read_text(encoding="utf-8")
    defaults = {}
    for name in ("QT_VERSION", *MODULES.values()):
        values = re.findall(r"(?m)^\s*ARG\s+" + re.escape(name) + r"=([^\s]+)\s*$", text)
        require(values and len(set(values)) == 1, "missing or conflicting fixed Dockerfile argument: " + name)
        defaults[name] = values[0]
    require(re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", defaults["QT_VERSION"]) is not None, "invalid fixed Qt version")
    for name in MODULES.values():
        require(re.fullmatch(r"[0-9a-f]{64}", defaults[name]) is not None and defaults[name] != "0" * 64,
                "invalid fixed Qt archive SHA-256")
    return defaults


def license_members(archive_path, module, version):
    """Read only regular top-level LICENSE* members from an authenticated tar."""
    prefix = module + "-everywhere-src-" + version + "/"
    result = {}
    with tarfile.open(archive_path, "r|xz") as archive:
        count = 0
        for member in archive:
            count += 1
            require(count <= 500000, "Qt archive contains too many members")
            name = member.name[2:] if member.name.startswith("./") else member.name
            if not name.startswith(prefix) or not member.isfile():
                continue
            filename = name[len(prefix):]
            if not filename.startswith("LICENSE") or "/" in filename:
                continue
            require("\\" not in filename and not any(ord(char) < 32 or ord(char) == 127 for char in filename),
                    "unsafe license member name")
            relative = "licenses/" + module + "/" + filename
            require(relative not in result and 0 <= member.size <= MAX_LICENSE_BYTES, "duplicate or oversized license member")
            stream = archive.extractfile(member)
            require(stream is not None, "cannot read original license member")
            with stream:
                data = stream.read(MAX_LICENSE_BYTES + 1)
            require(len(data) == member.size, "truncated original license member")
            result[relative] = hashlib.sha256(data).hexdigest()
    require(bool(result), "Qt archive has no regular top-level LICENSE files: " + module)
    return result


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate provenance JSON field")
        result[key] = value
    return result


def invalid_constant(value):
    raise SourceProofError("invalid provenance JSON constant: " + value)


def verify_sources(sources, repo_root, *, require_locked=False):
    """Return a deterministic proof without writing to the payload or checkout."""
    root = pathlib.Path(sources)
    repo = pathlib.Path(repo_root).resolve()
    try:
        files = inventory(root)
        defaults = recipe_defaults(repository_file(repo, RECIPE / "Dockerfile"))
        version = defaults["QT_VERSION"]
        expected = set(REPOSITORY_FILES)
        require(expected <= files, "source payload is missing recipe, patch or instructions")
        repository_hashes = {}
        for relative, origin in REPOSITORY_FILES.items():
            reference = sha256(repository_file(repo, origin))
            require(sha256(root / relative) == reference, "source payload differs from trusted checkout: " + relative)
            repository_hashes[relative] = reference

        archives = {}
        licenses = {}
        compressed_bytes = 0
        for module, argument in MODULES.items():
            filename = module + "-everywhere-opensource-src-" + version + ".tar.xz"
            relative = "archives/" + filename
            expected.add(relative)
            require(relative in files, "missing original Qt archive: " + filename)
            actual = sha256(root / relative, MAX_ARCHIVE_BYTES)
            require(actual == defaults[argument], "Qt archive SHA-256 differs from fixed recipe: " + filename)
            archives[filename] = actual
            compressed_bytes += (root / relative).stat().st_size
            originals = license_members(root / relative, module, version)
            for license_path, license_hash in originals.items():
                require(license_path in files and sha256(root / license_path, MAX_LICENSE_BYTES) == license_hash,
                        "missing or altered original Qt license: " + license_path)
            licenses.update(originals)
        expected.update(licenses)

        provenance_paths = {"apt/" + name for name in APT_MANIFESTS}
        locked = bool((files & provenance_paths) or OFFLINE_HELPER in files)
        provenance = {}
        if require_locked or locked:
            require(provenance_paths <= files and OFFLINE_HELPER in files, "complete locked APT provenance and helper are required")
            helper_hash = sha256(repository_file(repo, pathlib.PurePosixPath("scripts/ci_install_locked_apt.sh")))
            require(sha256(root / OFFLINE_HELPER) == helper_hash, "retained offline helper differs from trusted checkout")
            repository_hashes[OFFLINE_HELPER] = helper_hash
            expected.add(OFFLINE_HELPER)
            bases = set()
            for relative in sorted(provenance_paths):
                provenance[relative] = sha256(root / relative)
                document = json.loads((root / relative).read_text(encoding="utf-8"),
                                      object_pairs_hook=unique_object, parse_constant=invalid_constant)
                require(isinstance(document, dict) and type(document.get("schema_version")) is int
                        and document.get("schema_version") == 1 and document.get("kind") == "apt-inputs"
                        and document.get("platform") == "linux/amd64" and isinstance(document.get("stage"), str)
                        and bool(document["stage"]) and isinstance(document.get("packages"), list) and bool(document["packages"]),
                        "retained file is not an APT input manifest: " + relative)
                base = document.get("base_image")
                require(isinstance(base, str) and re.fullmatch(r"[a-z0-9][a-z0-9./:_-]*@sha256:[0-9a-f]{64}", base) is not None
                        and not base.endswith("0" * 64), "retained APT manifest lacks an immutable original base")
                bases.add(base)
            require(len(bases) == 1, "retained APT manifests disagree on their original base")
            expected.update(provenance_paths)
            locked = True
        require(files == expected, "source payload contains undeclared files or invented license copies")
        return {"schema_version": 1, "kind": "tsan-qt-source-proof", "qt_version": version,
                "archives": archives, "compressed_archive_bytes": compressed_bytes,
                "repository_files": repository_hashes, "license_files": licenses,
                "locked_provenance": locked, "apt_manifests": provenance}
    except (OSError, UnicodeError, tarfile.TarError, lzma.LZMAError, EOFError, ValueError) as error:
        raise SourceProofError(str(error)) from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=pathlib.Path, default=pathlib.Path("/usr/share/klogg-ci/qt-sources"))
    parser.add_argument("--repo-root", type=pathlib.Path, default=ROOT, help="Trusted matching checkout, not the source payload itself")
    parser.add_argument("--require-locked", action="store_true", help="Require the offline helper and all four actual APT manifests")
    args = parser.parse_args(argv)
    try:
        print(json.dumps(verify_sources(args.sources, args.repo_root, require_locked=args.require_locked), sort_keys=True, indent=2))
        return 0
    except SourceProofError as error:
        print("verify_tsan_qt_sources: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
