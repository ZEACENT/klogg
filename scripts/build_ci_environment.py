#!/usr/bin/env python3
"""Build one private CI image candidate, never publish or rebuild after testing.

One BuildKit invocation emits authoritative OCI bytes plus a Docker-load transport
companion. Both archives are retained unchanged. Canonical inputs.json retains the
verified envelope, including embedded APT package/source manifests, without copying
all package payloads into the candidate artifact. Its input identity and raw file
hash are bound to candidate/transport metadata without any output self-reference.
When declared, inputs/material-manifest.json is also retained verbatim beside
inputs.json; its existing input file record binds the acquisition/transform bytes.
Candidate metadata is made usable only after the load result and inspected
config/diffIDs match the OCI identity.

The resolved-input envelope and unsigned GitHub source identity are supplied by
the caller. Their consistency is checked; this driver does not authenticate their
provenance. APT acquisition and official tool-download verification are separate
materializer responsibilities. The build always uses --network=none and never
falls back to online package installation or changes Docker daemon configuration.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile

import ci_environment as ci
import ci_environment_inputs as apt

ROOT = pathlib.Path(__file__).resolve().parents[1]


class BuildError(ci.ContractError):
    """Candidate inputs, build output or loaded-image identity are invalid."""


def _require(condition, message):
    if not condition:
        raise BuildError(message)


def _fields(value, fields, label):
    _require(isinstance(value, dict) and set(value) == set(fields), label + " has missing or unknown fields")


def _path(value):
    return ci._relative_path(value, "input/context path")


def _regular(path):
    info = path.lstat()
    _require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, "links or special files are forbidden: " + str(path))
    return info


def _sha256(path):
    _regular(path)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _argument(name, value):
    _require(isinstance(name, str) and re.fullmatch(r"[A-Z][A-Z0-9_]*", name) is not None, "unsafe build argument name")
    _require(isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_./:@+%=-]+", value) is not None,
             "unsafe or empty build argument value: " + name)


def _check_materials(materials, records):
    _require(not materials.is_symlink() and materials.is_dir(), "materials must be a real directory")
    _require(isinstance(records, list) and len(records) <= apt.MAX_FILES, "invalid material file list")
    expected = {}
    directories = set()
    total = 0
    for record in records:
        _fields(record, {"path", "size", "sha256"}, "material record")
        relative = _path(record["path"])
        _require(relative not in expected, "duplicate material file: " + relative)
        _require(type(record["size"]) is int and record["size"] >= 0, "invalid material size")
        ci._digest(record["sha256"], "material SHA-256", prefix=False)
        total += record["size"]
        _require(total <= apt.MAX_BYTES, "material inventory exceeds size limit")
        expected[relative] = record
        directories.update(parent.as_posix() for parent in pathlib.PurePosixPath(relative).parents if parent.as_posix() != ".")
    actual = set()
    actual_directories = set()
    for path in materials.rglob("*"):
        relative = path.relative_to(materials).as_posix()
        info = path.lstat()
        if stat.S_ISDIR(info.st_mode):
            actual_directories.add(relative)
        else:
            _regular(path)
            actual.add(relative)
            _require(relative in expected, "undeclared material file: " + relative)
            record = expected[relative]
            _require(info.st_size == record["size"] and _sha256(path) == record["sha256"], "tampered material: " + relative)
    _require(actual == set(expected) and actual_directories == directories, "material inventory has missing or extra paths")
    return expected


def _recipe_paths(recipe, root):
    paths = {}
    context = pathlib.PurePosixPath(recipe["context"])
    for relative in recipe["recipe_files"]:
        source = ci._regular_recipe_file(root, relative)
        _regular(source)
        try:
            destination = pathlib.PurePosixPath(relative).relative_to(context).as_posix()
        except ValueError:
            destination = pathlib.PurePosixPath(relative).name
        _path(destination)
        _require(destination not in paths, "declared recipes collide in build context: " + destination)
        paths[destination] = source
    return paths


def _validate_inputs(inputs, family, recipe, materials, dockerfile):
    _fields(inputs, {"schema_version", "family", "platform", "build_args", "files", "apt_bundles"}, "resolved inputs")
    _require(type(inputs["schema_version"]) is int and inputs["schema_version"] == 1, "inputs require schema_version 1")
    _require(inputs["family"] == family and inputs["platform"] == recipe["platform"], "input family or platform mismatch")
    _require(isinstance(inputs["build_args"], dict), "build_args must be an object")
    arguments = dict(recipe.get("build_args", {}))
    for name, value in inputs["build_args"].items():
        _argument(name, value)
        _require(name not in arguments or arguments[name] == value, "input build argument conflicts with catalog: " + name)
        arguments[name] = value
    # The resolved manifest must identify the actual base itself; a catalog
    # default cannot silently fill an omitted acquisition identity.
    base = inputs["build_args"].get("UBUNTU_IMAGE")
    _require(isinstance(base, str) and re.fullmatch(r"[a-z0-9][a-z0-9./:_-]*@sha256:[0-9a-f]{64}", base) is not None
             and not base.endswith("0" * 64), "UBUNTU_IMAGE requires an exact nonzero base digest")
    records = _check_materials(materials, inputs["files"])
    bundles = inputs["apt_bundles"]
    _require(isinstance(bundles, list), "apt_bundles must be a list")
    bundle_paths = set()
    bundle_arguments = set()
    for bundle in bundles:
        _fields(bundle, {"path", "manifest_sha256", "build_arg", "manifest"}, "APT bundle reference")
        relative = _path(bundle["path"])
        ci._digest(bundle["manifest_sha256"], "APT manifest SHA-256", prefix=False)
        argument = bundle["build_arg"]
        _argument(argument, bundle["manifest_sha256"])
        _require(argument not in ("UBUNTU_IMAGE", "TOOLS_ARCHIVE_SHA256"), "APT bundle cannot replace base/tool arguments")
        _require(relative not in bundle_paths and argument not in bundle_arguments, "duplicate APT bundle path or runtime argument")
        bundle_paths.add(relative)
        bundle_arguments.add(argument)
        _require(relative + "/manifest.json" in records and relative + "/runtime.lock" in records,
                 "APT manifests must be declared material files")
        root = materials / relative
        _require(_sha256(root / "manifest.json") == bundle["manifest_sha256"], "APT raw manifest SHA-256 mismatch")
        manifest = apt.validate_materialized_inputs(root)
        _require(isinstance(bundle["manifest"], dict)
                 and ci.canonical_digest(bundle["manifest"]) == ci.canonical_digest(manifest),
                 "embedded APT manifest does not match the validated materialized manifest")
        _require(manifest["base_image"] == base and manifest["platform"] == inputs["platform"], "APT original base or platform mismatch")
        runtime_digest = manifest["runtime_lock"]["sha256"]
        _require(argument not in arguments or arguments[argument] == runtime_digest, "caller conflicts with derived APT runtime lock")
        arguments[argument] = runtime_digest
    if family == "noble-qt693-analysis" or "TOOLS_ARCHIVE_SHA256" in arguments:
        _require("inputs/tools.tar" in records and arguments.get("TOOLS_ARCHIVE_SHA256") == records["inputs/tools.tar"]["sha256"],
                 "TOOLS_ARCHIVE_SHA256 must match declared inputs/tools.tar bytes")
    declarations = re.findall(r"(?mi)^[ \t]*ARG[ \t]+([A-Za-z_][A-Za-z0-9_]*)([^\r\n]*)", dockerfile.read_text(encoding="utf-8"))
    declared_arguments = {name for name, _ in declarations}
    required_apt_arguments = {name for name, suffix in declarations
                              if name.startswith("APT_") and name.endswith("LOCK_SHA256") and not suffix.lstrip().startswith("=")}
    supplied_apt_arguments = {name for name in arguments if name.startswith("APT_") and name.endswith("LOCK_SHA256")}
    material_bundle_paths = {pathlib.PurePosixPath(path).parent.as_posix() for path in records
                             if pathlib.PurePosixPath(path).name == "runtime.lock"}
    _require(material_bundle_paths == bundle_paths, "every APT runtime.lock material requires a bundle reference")
    _require((required_apt_arguments | supplied_apt_arguments) <= bundle_arguments,
             "APT lock build arguments require validated bundle references")
    for name, value in arguments.items():
        _argument(name, value)
        _require(name in declared_arguments, "build argument is not declared by the recipe: " + name)
    return arguments, records


def _copy_checked(source, destination, digest):
    mode = _regular(source).st_mode & 0o777
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(mode)
    _regular(source)
    _require(_sha256(destination) == digest, "input changed while staging: " + str(source))


def _output_ready(path):
    _require(not path.is_symlink(), "output must not be a symlink")
    _require(not path.exists() or (path.is_dir() and not any(path.iterdir())), "output must be new or empty; refusing candidate reuse")


def _export_option(kind, destination):
    # Buildx output descriptors are CSV, not shell arguments. Quote destinations
    # containing commas rather than allowing them to add exporter options.
    output = io.StringIO()
    fields = ["type=" + kind, "dest=" + str(destination)]
    if kind == "oci":
        fields.append("oci-mediatypes=true")
    csv.writer(output, lineterminator="").writerow(fields)
    return output.getvalue()


def _run(runner, command):
    result = runner(command, check=True, capture_output=True, text=True, timeout=14400)
    _require(result.returncode == 0, "Docker operation failed: " + " ".join(command[:3]))
    return result


def _write_json(path, document, *, canonical=False):
    text = json.dumps(document, sort_keys=True, ensure_ascii=True, allow_nan=False,
                      indent=None if canonical else 2, separators=(",", ":") if canonical else None)
    path.write_text(text + "\n", encoding="ascii")


def build_environment(repo_root, family, inputs, materials, source, output, *, builder=None, runner=None):
    """Build once and atomically expose compared candidate bytes in a new directory.

    runner is the subprocess.run-compatible deterministic testing seam. Docker
    images are never removed, tagged or pushed, even after a failed comparison.
    """
    runner = subprocess.run if runner is None else runner
    root = pathlib.Path(repo_root).resolve()
    materials = pathlib.Path(materials).absolute()
    output = pathlib.Path(output).absolute()
    try:
        _output_ready(output)
        _require(not materials.is_symlink(), "materials must not be a symlink")
        materials = materials.resolve()
        output = output.parent.resolve() / output.name
        _require(not (output == materials or materials in output.parents), "output must not be inside input materials")
        if builder is not None:
            _require(isinstance(builder, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", builder) is not None, "unsafe builder name")
        catalog = ci.load_json(root / "ci/environments/recipes.json")
        ci.validate_catalog(catalog)
        _require(isinstance(family, str) and family in catalog["families"], "unknown build family")
        # Reuse the core source-only validator rather than fabricating image
        # digests just to invoke candidate validation before the image exists.
        ci._validate_source(source)
        recipe = catalog["families"][family]
        recipe_paths = _recipe_paths(recipe, root)
        dockerfile = root / recipe["dockerfile"]
        arguments, records = _validate_inputs(inputs, family, recipe, materials, dockerfile)
        _require(not set(recipe_paths) & set(records), "material collides with a declared recipe file")
        all_paths = set(recipe_paths) | set(records)
        for relative in all_paths:
            _require(not any(parent.as_posix() in all_paths for parent in pathlib.PurePosixPath(relative).parents),
                     "file/directory collision in staged context: " + relative)
        recipe_digest = ci.recipe_identity(catalog, family, root)
        input_digest = ci.input_identity(inputs)
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="klogg-ci-build-", dir=str(output.parent)) as temporary:
            private = pathlib.Path(temporary)
            context = private / "context"
            artifacts = private / "artifacts"
            context.mkdir()
            artifacts.mkdir()
            staged_dockerfile = None
            for relative, origin in recipe_paths.items():
                destination = context / relative
                _copy_checked(origin, destination, _sha256(origin))
                if origin == dockerfile:
                    staged_dockerfile = destination
            _require(staged_dockerfile is not None, "Dockerfile was not staged")
            for relative, record in records.items():
                _copy_checked(materials / relative, context / relative, record["sha256"])
            _require(ci.recipe_identity(catalog, family, root) == recipe_digest, "recipe changed while staging")
            oci_archive = artifacts / "candidate.oci.tar"
            docker_archive = artifacts / "candidate.docker.tar"
            command = ["docker", "buildx", "build", "--platform", "linux/amd64", "--network=none",
                       "--provenance=false", "--sbom=false", "--file", str(staged_dockerfile),
                       "--output", _export_option("oci", oci_archive), "--output", _export_option("docker", docker_archive)]
            if builder is not None:
                command += ["--builder", builder]
            for name, value in sorted(arguments.items()):
                command += ["--build-arg", name + "=" + value]
            command.append(str(context))
            _run(runner, command)
            _regular(oci_archive)
            image = ci.inspect_oci_archive(oci_archive)
            _require(_regular(docker_archive).st_size > 0, "Docker companion archive is empty")
            companion_sha = _sha256(docker_archive)
            loaded = _run(runner, ["docker", "load", "--input", str(docker_archive)])
            _require(isinstance(loaded.stdout, str), "Docker load did not report an image identity")
            loaded_lines = [line for line in loaded.stdout.splitlines() if line.startswith("Loaded image")]
            _require(loaded_lines == ["Loaded image ID: " + image["config_digest"]],
                     "Docker load identity differs from the authoritative OCI config")
            inspected = _run(runner, ["docker", "image", "inspect", image["config_digest"]])
            _require(isinstance(inspected.stdout, str) and len(inspected.stdout.encode("utf-8")) <= ci.MAX_METADATA_BYTES,
                     "Docker inspect output is missing or oversized")
            loaded_image = ci._parse_json(inspected.stdout.encode("utf-8"), "Docker image inspect")
            ci.compare_loaded_image(image, loaded_image)
            _require(_sha256(oci_archive) == image["archive_sha256"] and _sha256(docker_archive) == companion_sha,
                     "candidate archive changed during load/comparison")
            retained_inputs = artifacts / "inputs.json"
            _write_json(retained_inputs, inputs, canonical=True)
            _require(ci.input_identity(ci.load_json(retained_inputs)) == input_digest, "retained input identity mismatch")
            material_path = "inputs/material-manifest.json"
            if material_path in records:
                _copy_checked(context / material_path, artifacts / "material-manifest.json",
                              records[material_path]["sha256"])
            candidate = {"schema_version": 1, "kind": "candidate", "family": family, "recipe_digest": recipe_digest,
                         "input_digest": input_digest, "source": source, "image": image}
            ci.validate_candidate(candidate, catalog)
            transport = {"schema_version": 1, "kind": "docker-load-transport", "candidate_digest": ci.canonical_digest(candidate),
                         "oci_archive": {"path": oci_archive.name, "sha256": image["archive_sha256"]},
                         "docker_archive": {"path": docker_archive.name, "sha256": companion_sha},
                         "inputs_manifest": {"path": retained_inputs.name, "sha256": _sha256(retained_inputs)},
                         "config_digest": image["config_digest"], "diff_ids": image["diff_ids"], "loaded_image_matches": True}
            _write_json(artifacts / "candidate.json", candidate)
            _write_json(artifacts / "transport.json", transport)
            _output_ready(output)
            os.replace(artifacts, output)
            return candidate
    except (OSError, UnicodeError, subprocess.SubprocessError) as error:
        raise BuildError("candidate build failed: " + str(error)) from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--family", required=True)
    parser.add_argument("--inputs", type=pathlib.Path, required=True)
    parser.add_argument("--materials", type=pathlib.Path, required=True)
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--builder")
    args = parser.parse_args(argv)
    try:
        candidate = build_environment(args.repo_root, args.family, ci.load_json(args.inputs), args.materials,
                                      ci.load_json(args.source), args.output, builder=args.builder)
        print(json.dumps(candidate, sort_keys=True, indent=2))
        return 0
    except (ci.ContractError, OSError) as error:
        print("build_ci_environment: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
