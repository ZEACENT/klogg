"""Private candidate build contracts, with no Docker daemon or network access."""
from __future__ import annotations

import contextlib
import copy
import csv
import gzip
import hashlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import build_ci_environment as build
import ci_environment as ci
import ci_environment_inputs as apt

# Synthetic fixture identities are never proposed as production locks.
BASE = "example.invalid/ubuntu:jammy@sha256:" + "a" * 64
FAMILY = "jammy-qt5"
OCI = "application/vnd.oci.image."


def encoded(document):
    return json.dumps(document, sort_keys=True, separators=(",", ":")).encode("ascii")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def exports(command):
    return [dict(field.split("=", 1) for field in next(csv.reader([command[index + 1]])))
            for index, value in enumerate(command[:-1]) if value == "--output"]


def source():
    return {"repository": "ZEACENT/klogg", "sha": "b" * 40, "ref": "refs/heads/master",
            "workflow": ".github/workflows/ci-environment-producer.yml", "run_id": 123, "run_attempt": 2}


def oci_fixture():
    layer = gzip.compress(b"synthetic layer contents", mtime=0)
    diff_id = "sha256:" + sha(b"synthetic layer contents")
    config = encoded({"architecture": "amd64", "os": "linux", "rootfs": {"type": "layers", "diff_ids": [diff_id]}})
    def descriptor(data, media):
        return {"digest": "sha256:" + sha(data), "size": len(data), "mediaType": OCI + media}
    manifest = encoded({"schemaVersion": 2, "mediaType": OCI + "manifest.v1+json",
                        "config": descriptor(config, "config.v1+json"), "layers": [descriptor(layer, "layer.v1.tar+gzip")]})
    index = encoded({"schemaVersion": 2, "mediaType": OCI + "index.v1+json",
                     "manifests": [descriptor(manifest, "manifest.v1+json")]})
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for name, data in [("oci-layout", encoded({"imageLayoutVersion": "1.0.0"})), ("index.json", index)] + [
                ("blobs/sha256/" + sha(data), data) for data in (config, manifest, layer)]:
            info = tarfile.TarInfo(name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    image = {"Id": "sha256:" + sha(config), "Architecture": "amd64", "Os": "linux",
             "RootFS": {"Type": "layers", "Layers": [diff_id]}}
    return output.getvalue(), image


class BuildEnvironmentTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ci-build-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.materials = self.root / "materials"
        self.output = self.root / "candidate"
        self.materials.mkdir()
        self.context = self.repo / "docker/example"
        self.context.mkdir(parents=True)
        (self.context / "Dockerfile").write_text("ARG UBUNTU_IMAGE\nFROM ${UBUNTU_IMAGE}\nARG APT_RUNTIME_LOCK_SHA256=\nARG TOOLS_ARCHIVE_SHA256\nARG QT_BUILD_JOBS=2\nCOPY recipe.patch /tmp/recipe.patch\nCOPY shared_helper.sh /opt/helper.sh\nCOPY inputs /inputs\nRUN --network=none true\n", encoding="ascii")
        (self.context / "recipe.patch").write_bytes(b"recipe patch\n")
        (self.repo / "scripts").mkdir()
        (self.repo / "scripts/shared_helper.sh").write_bytes(b"#!/bin/sh\ntrue\n")
        (self.repo / "scripts/shared_helper.sh").chmod(0o755)
        (self.repo / ".env").write_bytes(b"SECRET=do-not-copy\n")
        (self.context / "undeclared-secret").write_bytes(b"not a declared recipe file\n")
        (self.repo / "ci/environments").mkdir(parents=True)
        self.catalog = {"schema_version": 1, "registry": ci.REGISTRY, "families": {FAMILY: {
            "platform": "linux/amd64", "dockerfile": "docker/example/Dockerfile", "context": "docker/example",
            "recipe_files": ["docker/example/Dockerfile", "docker/example/recipe.patch", "scripts/shared_helper.sh"],
            "profiles": ["deb", "asan-lsan", "ubsan"]}}}
        self.save_catalog()
        self.file("inputs/payload.bin", b"resolved input bytes")
        self.inputs = self.envelope()
        self.commands = []
        self.context_files = {}
        self.oci_bytes, self.loaded_image = oci_fixture()
        self.docker_bytes = b"synthetic Docker companion bytes; fake process handles loading"
        self.load_stdout = "Loaded image ID: " + self.loaded_image["Id"] + "\n"

    def save_catalog(self):
        (self.repo / "ci/environments/recipes.json").write_bytes(encoded(self.catalog))

    def file(self, relative, data):
        path = self.materials / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def envelope(self, family=FAMILY):
        records = [{"path": path.relative_to(self.materials).as_posix(), "size": path.stat().st_size, "sha256": sha(path.read_bytes())}
                   for path in sorted(self.materials.rglob("*")) if path.is_file()]
        return {"schema_version": 1, "family": family, "platform": "linux/amd64",
                "build_args": {"UBUNTU_IMAGE": BASE}, "files": records, "apt_bundles": []}

    def docker(self, command, **kwargs):
        self.commands.append(command)
        self.assertEqual(command[0], "docker")
        self.assertNotIn("shell", kwargs)
        if command[1:3] == ["buildx", "build"]:
            context = pathlib.Path(command[-1])
            self.context_files = {path.relative_to(context).as_posix(): path.read_bytes()
                                  for path in context.rglob("*") if path.is_file()}
            self.assertEqual((context / "shared_helper.sh").stat().st_mode & 0o777, 0o755)
            outputs = exports(command)
            self.assertEqual(len(outputs), 2)
            for options in outputs:
                pathlib.Path(options["dest"]).write_bytes(self.oci_bytes if options["type"] == "oci" else self.docker_bytes)
            return subprocess.CompletedProcess(command, 0, "build completed\n", "")
        if command[1] == "load":
            self.assertEqual(pathlib.Path(command[command.index("--input") + 1]).read_bytes(), self.docker_bytes)
            return subprocess.CompletedProcess(command, 0, self.load_stdout, "")
        if command[1:3] == ["image", "inspect"]:
            return subprocess.CompletedProcess(command, 0, json.dumps([self.loaded_image]), "")
        self.fail("unexpected Docker operation: " + repr(command))

    def run_build(self, **overrides):
        args = dict(repo_root=self.repo, family=FAMILY, inputs=self.inputs, materials=self.materials,
                    source=source(), output=self.output, builder="isolated-builder", runner=self.docker)
        args.update(overrides)
        return build.build_environment(**args)

    def test_one_offline_build_retains_authoritative_oci_and_companion(self):
        candidate = self.run_build()
        self.assertEqual(candidate.get("kind"), "candidate")
        self.assertEqual([command[1:3] for command in self.commands], [["buildx", "build"], ["load", "--input"], ["image", "inspect"]])
        command = self.commands[0]
        self.assertEqual(command[command.index("--platform") + 1], "linux/amd64")
        for flag in ("--network=none", "--provenance=false", "--sbom=false"):
            self.assertIn(flag, command)
        self.assertIn("--builder", command)
        self.assertIn("isolated-builder", command)
        self.assertFalse(any(token in command for token in ("--push", "--load", "--tag")))
        self.assertEqual(set(self.context_files), {"Dockerfile", "recipe.patch", "shared_helper.sh", "inputs/payload.bin"})
        self.assertEqual((self.output / "candidate.oci.tar").read_bytes(), self.oci_bytes)
        self.assertEqual((self.output / "candidate.docker.tar").read_bytes(), self.docker_bytes)
        self.assertEqual(ci.load_json(self.output / "candidate.json"), candidate)
        self.assertEqual(candidate["recipe_digest"], ci.recipe_identity(self.catalog, FAMILY, self.repo))
        self.assertEqual(candidate["input_digest"], ci.input_identity(self.inputs))
        self.assertNotIn("candidate_artifact_id", candidate)
        transport = ci.load_json(self.output / "transport.json")
        self.assertEqual(transport["oci_archive"], {"path": "candidate.oci.tar", "sha256": sha(self.oci_bytes)})
        self.assertEqual(transport["docker_archive"], {"path": "candidate.docker.tar", "sha256": sha(self.docker_bytes)})
        self.assertEqual(transport["candidate_digest"], ci.canonical_digest(candidate))
        self.assertTrue(transport["loaded_image_matches"])
        self.assertEqual(transport["config_digest"], candidate["image"]["config_digest"])
        self.assertEqual(transport["diff_ids"], candidate["image"]["diff_ids"])
        self.assertFalse(pathlib.Path(command[-1]).exists())

    def test_canonical_inputs_are_retained_and_bound_without_output_self_reference(self):
        candidate = self.run_build()
        path = self.output / "inputs.json"
        self.assertTrue(path.is_file(), "usable candidate must retain the verified input envelope")
        self.assertEqual(path.read_bytes(), encoded(self.inputs) + b"\n")
        retained = ci.load_json(path)
        self.assertEqual(ci.input_identity(retained), candidate["input_digest"])
        transport = ci.load_json(self.output / "transport.json")
        self.assertEqual(transport["inputs_manifest"], {"path": "inputs.json", "sha256": sha(path.read_bytes())})
        self.assertEqual(retained, self.inputs)
        self.assertNotIn("candidate.oci.tar", path.read_text(encoding="ascii"))
        self.assertFalse((self.output / "material-manifest.json").exists(),
                         "legacy inputs must not gain invented material provenance")

    def add_material_manifest(self):
        # Deliberately noncanonical formatting: the artifact retains raw bytes,
        # not a newly serialized interpretation of the materializer's document.
        data = (b'{\n  "schema_version": 1, "kind": "ci-material-acquisition",\n'
                b'  "family": "jammy-qt5", "definition": {},\n'
                b'  "assets": [{"name": "fixture-tool", "url": "https://example.invalid/tool"}],\n'
                b'  "transforms": [{"operation": "extract"}]\n}\n\n')
        path = self.file("inputs/material-manifest.json", data)
        self.inputs = self.envelope()
        return path, data

    def test_declared_material_manifest_retains_exact_staged_bytes_and_existing_binding(self):
        original, data = self.add_material_manifest()
        def change_original_after_staging(command, **kwargs):
            result = self.docker(command, **kwargs)
            if command[1:3] == ["buildx", "build"]:
                original.write_bytes(b"changed outside the staged context")
            return result
        candidate = self.run_build(runner=change_original_after_staging)
        retained = self.output / "material-manifest.json"
        self.assertTrue(retained.is_file(), "candidate lost the declared tool acquisition/transform provenance")
        self.assertEqual(retained.read_bytes(), data)
        self.assertNotEqual(original.read_bytes(), retained.read_bytes())
        inputs = ci.load_json(self.output / "inputs.json")
        record = next(record for record in inputs["files"] if record["path"] == "inputs/material-manifest.json")
        self.assertEqual(record, {"path": "inputs/material-manifest.json", "size": len(data), "sha256": sha(data)})
        self.assertEqual(ci.input_identity(inputs), candidate["input_digest"])
        self.assertEqual(set(ci.load_json(self.output / "transport.json")), {
            "schema_version", "kind", "candidate_digest", "oci_archive", "docker_archive",
            "inputs_manifest", "config_digest", "diff_ids", "loaded_image_matches",
        })

    def test_material_manifest_changed_or_missing_in_staged_context_never_publishes(self):
        self.add_material_manifest()
        for mutation in ("tampered", "missing", "symlink"):
            with self.subTest(mutation=mutation):
                def change_staged_manifest(command, **kwargs):
                    result = self.docker(command, **kwargs)
                    if command[1:3] == ["buildx", "build"]:
                        path = pathlib.Path(command[-1]) / "inputs/material-manifest.json"
                        if mutation == "tampered":
                            path.write_bytes(b"unverified provenance")
                        else:
                            path.unlink()
                            if mutation == "symlink":
                                path.symlink_to(self.repo / ".env")
                    return result
                output = self.root / mutation
                with self.assertRaises(ci.ContractError):
                    self.run_build(runner=change_staged_manifest, output=output)
                self.assertFalse(output.exists())

    def test_retained_input_identity_is_checked_before_candidate_becomes_usable(self):
        def mutating(command, **kwargs):
            result = self.docker(command, **kwargs)
            if command[1:3] == ["buildx", "build"]:
                self.inputs["build_args"]["QT_BUILD_JOBS"] = "8"
            return result
        with self.assertRaisesRegex(ci.ContractError, "retained input identity"):
            self.run_build(runner=mutating)
        self.assertFalse(self.output.exists())

    def test_tampered_missing_or_extra_materials_fail_before_docker(self):
        for mutation in ("tamper", "missing", "extra", "symlink", "hardlink", "empty-directory"):
            with self.subTest(mutation=mutation):
                path = self.materials / "inputs/payload.bin"
                original = path.read_bytes()
                extra = self.materials / "extra"
                try:
                    if mutation == "tamper":
                        path.write_bytes(b"wrong")
                    elif mutation == "missing":
                        path.unlink()
                    elif mutation == "symlink":
                        path.unlink()
                        path.symlink_to(self.repo / ".env")
                    elif mutation == "hardlink":
                        os.link(path, extra)
                    elif mutation == "empty-directory":
                        extra.mkdir()
                    else:
                        extra.write_bytes(b"extra")
                    with self.assertRaises(ci.ContractError):
                        self.run_build()
                    self.assertEqual(self.commands, [])
                    self.assertFalse(self.output.exists())
                finally:
                    if path.is_symlink():
                        path.unlink()
                    if extra.is_dir():
                        extra.rmdir()
                    elif extra.exists():
                        extra.unlink()
                    path.write_bytes(original)

    def test_unsafe_family_paths_source_and_build_arguments_fail_before_docker(self):
        mutations = [lambda value: value.update(family="other"), lambda value: value.update(platform="linux/arm64"),
                     lambda value: value.update(schema_version=True),
                     lambda value: value["build_args"].update(UBUNTU_IMAGE="ubuntu:jammy"),
                     lambda value: value["build_args"].update(UBUNTU_IMAGE="ubuntu@sha256:" + "0" * 64),
                     lambda value: value["build_args"].update(HTTP_PROXY="http://proxy.invalid"),
                     lambda value: value["build_args"].update({"--secret": "secret"}),
                     lambda value: value["build_args"].update(QT_BUILD_JOBS="2;id"),
                     lambda value: value["build_args"].update(QT_BUILD_JOBS="2\nINJECTED=1"),
                     lambda value: value["files"][0].update(path="../escape"),
                     lambda value: value["files"][0].update(path="/absolute"),
                     lambda value: value["files"].append(dict(value["files"][0]))]
        for mutation in mutations:
            envelope = copy.deepcopy(self.inputs)
            mutation(envelope)
            with self.subTest(mutation=mutation), self.assertRaises(ci.ContractError):
                self.run_build(inputs=envelope)
        for identity in (dict(source(), run_attempt=True), dict(source(), sha="0" * 40), dict(source(), repository="other/repo")):
            with self.assertRaises(ci.ContractError):
                self.run_build(source=identity)
        with self.assertRaises(ci.ContractError):
            self.run_build(builder="--allow=network.host")
        self.assertEqual(self.commands, [])

    def fixed_stage(self):
        dockerfile = self.context / "Dockerfile"
        dockerfile.write_text("ARG KLOGG_APT_STAGE=online\n" + dockerfile.read_text(encoding="ascii"), encoding="ascii")
        self.catalog["families"][FAMILY]["build_args"] = {"KLOGG_APT_STAGE": "locked"}
        self.save_catalog()

    def test_input_envelope_cannot_override_catalog_locked_stage_with_online(self):
        self.fixed_stage()
        envelope = copy.deepcopy(self.inputs)
        envelope["build_args"]["KLOGG_APT_STAGE"] = "online"
        with self.assertRaisesRegex(ci.ContractError, "conflicts with catalog"):
            self.run_build(inputs=envelope)
        self.assertEqual(self.commands, [])
        self.assertFalse(self.output.exists())

    def test_fixed_catalog_stage_is_applied_when_omitted_or_repeated_identically(self):
        self.fixed_stage()
        for repeat in (False, True):
            envelope = copy.deepcopy(self.inputs)
            if repeat:
                envelope["build_args"]["KLOGG_APT_STAGE"] = "locked"
            self.commands.clear()
            with self.subTest(repeat=repeat):
                self.run_build(inputs=envelope, output=self.root / ("repeated" if repeat else "omitted"))
                command = self.commands[0]
                arguments = [command[index + 1] for index, value in enumerate(command[:-1]) if value == "--build-arg"]
                self.assertEqual(arguments.count("KLOGG_APT_STAGE=locked"), 1)
                self.assertNotIn("KLOGG_APT_STAGE=online", arguments)
                self.assertIn("--network=none", command)

    def test_resolved_envelope_must_supply_base_even_if_catalog_has_default(self):
        self.catalog["families"][FAMILY]["build_args"] = {"UBUNTU_IMAGE": BASE}
        self.save_catalog()
        envelope = copy.deepcopy(self.inputs)
        envelope["build_args"].clear()
        with self.assertRaises(ci.ContractError):
            self.run_build(inputs=envelope)
        self.assertEqual(self.commands, [])

    def test_material_and_external_recipe_collisions_are_rejected(self):
        self.file("Dockerfile", b"attacker recipe")
        with self.assertRaises(ci.ContractError):
            self.run_build(inputs=self.envelope())
        (self.materials / "Dockerfile").unlink()
        (self.repo / "scripts/Dockerfile").write_bytes(b"colliding external recipe")
        self.catalog["families"][FAMILY]["recipe_files"].append("scripts/Dockerfile")
        self.save_catalog()
        with self.assertRaises(ci.ContractError):
            self.run_build()
        self.assertEqual(self.commands, [])

    def test_export_destination_commas_cannot_inject_exporter_options(self):
        output = self.root / "parent,push=true" / "candidate"
        self.run_build(output=output)
        for options in exports(self.commands[0]):
            self.assertNotIn("push", options)
            self.assertIn("parent,push=true", options["dest"])
        self.assertEqual((output / "candidate.oci.tar").read_bytes(), self.oci_bytes)

    def test_output_must_be_new_or_empty_and_cannot_be_reused(self):
        self.output.mkdir()
        self.run_build()
        count = len(self.commands)
        with self.assertRaises(ci.ContractError):
            self.run_build()
        self.assertEqual(len(self.commands), count)
        link = self.root / "linked-output"
        link.symlink_to(self.output, target_is_directory=True)
        with self.assertRaises(ci.ContractError):
            self.run_build(output=link)
        self.assertEqual(len(self.commands), count)

    def test_wrong_load_identity_or_diff_ids_never_emit_candidate(self):
        for wrong in ("load", "config", "diffids"):
            with self.subTest(wrong=wrong):
                original = copy.deepcopy(self.loaded_image)
                original_output = self.load_stdout
                try:
                    if wrong == "load":
                        self.load_stdout = "Loaded image ID: sha256:" + "c" * 64 + "\n"
                    elif wrong == "config":
                        self.loaded_image["Id"] = "sha256:" + "c" * 64
                    else:
                        self.loaded_image["RootFS"]["Layers"] = ["sha256:" + "c" * 64]
                    with self.assertRaises(ci.ContractError):
                        self.run_build()
                    self.assertFalse(self.output.exists())
                    self.assertFalse(any(command[1] in ("rmi", "rm", "push", "tag") for command in self.commands))
                finally:
                    self.loaded_image = original
                    self.load_stdout = original_output

    def test_invalid_or_missing_build_exports_are_rejected_before_load(self):
        for invalid in ("corrupt-oci", "missing-companion", "symlink-companion"):
            self.commands.clear()
            def malformed(command, **kwargs):
                result = self.docker(command, **kwargs)
                if command[1:3] == ["buildx", "build"]:
                    artifact_dir = pathlib.Path(exports(command)[0]["dest"]).parent
                    if invalid == "corrupt-oci":
                        (artifact_dir / "candidate.oci.tar").write_bytes(b"not an OCI archive")
                    else:
                        companion = artifact_dir / "candidate.docker.tar"
                        companion.unlink()
                        if invalid == "symlink-companion":
                            companion.symlink_to(self.repo / ".env")
                return result
            with self.subTest(invalid=invalid), self.assertRaises(ci.ContractError):
                self.run_build(runner=malformed)
            self.assertEqual(len(self.commands), 1)
            self.assertFalse(self.output.exists())

    def test_archive_mutation_during_load_cannot_become_transport_authority(self):
        for name in ("candidate.oci.tar", "candidate.docker.tar"):
            def mutating(command, **kwargs):
                result = self.docker(command, **kwargs)
                if command[1] == "load":
                    target = pathlib.Path(command[command.index("--input") + 1]).parent / name
                    target.write_bytes(target.read_bytes() + b"changed")
                return result
            with self.subTest(name=name), self.assertRaises(ci.ContractError):
                self.run_build(runner=mutating)
            self.assertFalse(self.output.exists())

    def test_build_failure_does_not_retry_or_publish_partial_candidate(self):
        def failing(command, **kwargs):
            self.commands.append(command)
            raise subprocess.CalledProcessError(1, command, stderr="offline build failed")
        with self.assertRaises(ci.ContractError):
            self.run_build(runner=failing)
        self.assertEqual(len(self.commands), 1)
        self.assertFalse(self.output.exists())

    def add_apt_bundle(self, base=BASE):
        root = self.materials / "inputs/apt"
        root.mkdir(parents=True)
        (root / "lists").mkdir()
        (root / "debs").mkdir()
        stage = {"schema_version": 1, "stage": "build", "platform": "linux/amd64", "base_image": base,
                 "sources": ["deb http://archive.ubuntu.com/ubuntu jammy main"], "requested_packages": ["example"],
                 "acquisition": {"mode": "apt-signed"}}
        (root / "sources.list").write_text(stage["sources"][0] + "\n", encoding="ascii")
        (root / "base-packages.tsv").write_text("base-files\t1.0\tamd64\tinstalled\n", encoding="ascii")
        (root / "index-targets.txt").write_text("repo_dists_jammy_main_binary-amd64_Packages.lz4\n", encoding="ascii")
        (root / "lists/repo_dists_jammy_InRelease").write_bytes(b"synthetic release\n")
        (root / "lists/repo_dists_jammy_main_binary-amd64_Packages.lz4").write_bytes(b"synthetic index\n")
        (root / "debs/example_1.2_amd64.deb").write_bytes(b"synthetic package\n")
        (root / "resolution.tsv").write_text("example\t1.2\tamd64\texample_1.2_amd64.deb\n", encoding="ascii")
        manifest = apt.finalize(root, stage)
        self.inputs = self.envelope()
        self.inputs["apt_bundles"] = [{"path": "inputs/apt", "manifest_sha256": sha((root / "manifest.json").read_bytes()),
                                      "build_arg": "APT_RUNTIME_LOCK_SHA256", "manifest": copy.deepcopy(manifest)}]
        return manifest

    def test_apt_runtime_arg_is_derived_from_validated_raw_manifest(self):
        manifest = self.add_apt_bundle()
        original = copy.deepcopy(self.inputs)
        self.run_build()
        args = [self.commands[0][index + 1] for index, value in enumerate(self.commands[0][:-1]) if value == "--build-arg"]
        self.assertIn("APT_RUNTIME_LOCK_SHA256=" + manifest["runtime_lock"]["sha256"], args)
        self.assertEqual(self.inputs, original)

    def test_apt_package_versions_and_source_closure_survive_in_retained_inputs(self):
        manifest = self.add_apt_bundle()
        self.run_build()
        retained = ci.load_json(self.output / "inputs.json")
        self.assertEqual(retained["apt_bundles"][0]["manifest"], manifest)
        self.assertEqual(retained["apt_bundles"][0]["manifest"]["packages"][0]["version"], "1.2")
        self.assertEqual(retained["apt_bundles"][0]["manifest"]["sources"], manifest["sources"])
        self.assertEqual(retained["apt_bundles"][0]["manifest_sha256"], sha((self.materials / "inputs/apt/manifest.json").read_bytes()))
        self.assertFalse((self.output / "inputs/apt/debs").exists())

    def test_embedded_apt_manifest_is_required_and_must_match_validated_bytes(self):
        self.add_apt_bundle()
        missing = copy.deepcopy(self.inputs)
        del missing["apt_bundles"][0]["manifest"]
        with self.assertRaisesRegex(ci.ContractError, "APT bundle reference"):
            self.run_build(inputs=missing)
        mutations = [lambda manifest: manifest["packages"][0].update(version="9.9"),
                     lambda manifest: manifest.update(sources=["deb https://example.invalid/ubuntu jammy main"]),
                     lambda manifest: manifest["runtime_lock"].update(sha256="c" * 64)]
        for mutate in mutations:
            envelope = copy.deepcopy(self.inputs)
            mutate(envelope["apt_bundles"][0]["manifest"])
            with self.subTest(mutate=mutate), self.assertRaisesRegex(ci.ContractError, "embedded APT manifest"):
                self.run_build(inputs=envelope)
        self.assertEqual(self.commands, [])

    def test_apt_manifest_hash_base_and_runtime_arg_conflicts_fail_before_docker(self):
        manifest = self.add_apt_bundle()
        for mutation in ("manifest", "base", "argument", "duplicate"):
            envelope = copy.deepcopy(self.inputs)
            if mutation == "manifest":
                envelope["apt_bundles"][0]["manifest_sha256"] = "d" * 64
            elif mutation == "base":
                envelope["build_args"]["UBUNTU_IMAGE"] = BASE[:-64] + "d" * 64
            elif mutation == "argument":
                envelope["build_args"]["APT_RUNTIME_LOCK_SHA256"] = "d" * 64
            else:
                envelope["apt_bundles"].append(dict(envelope["apt_bundles"][0]))
            with self.subTest(mutation=mutation), self.assertRaises(ci.ContractError):
                self.run_build(inputs=envelope)
        self.assertEqual(self.commands, [])
        self.assertNotEqual(manifest["runtime_lock"]["sha256"], "d" * 64)

    def test_required_apt_lock_argument_needs_a_validated_bundle(self):
        dockerfile = self.context / "Dockerfile"
        dockerfile.write_text(dockerfile.read_text(encoding="ascii").replace("ARG APT_RUNTIME_LOCK_SHA256=\n", "ARG APT_RUNTIME_LOCK_SHA256\n"), encoding="ascii")
        with self.assertRaises(ci.ContractError):
            self.run_build()
        self.assertEqual(self.commands, [])

    def test_omitted_apt_reference_cannot_bypass_bundle_validation(self):
        manifest = self.add_apt_bundle()
        envelope = copy.deepcopy(self.inputs)
        envelope["apt_bundles"] = []
        envelope["build_args"]["APT_RUNTIME_LOCK_SHA256"] = manifest["runtime_lock"]["sha256"]
        with self.assertRaises(ci.ContractError):
            self.run_build(inputs=envelope)
        self.assertEqual(self.commands, [])

    def test_symlinked_output_parent_cannot_alias_materials(self):
        alias = self.root / "material-alias"
        alias.symlink_to(self.materials, target_is_directory=True)
        with self.assertRaises(ci.ContractError):
            self.run_build(output=alias / "candidate")
        self.assertEqual(self.commands, [])

    def test_load_reporting_an_extra_tagged_image_is_rejected(self):
        self.load_stdout += "Loaded image: example.invalid/extra:latest\n"
        with self.assertRaises(ci.ContractError):
            self.run_build()
        self.assertFalse(self.output.exists())

    def test_analysis_tools_hash_must_bind_declared_archive_bytes(self):
        family = "noble-qt693-analysis"
        self.catalog["families"][family] = self.catalog["families"].pop(FAMILY)
        self.save_catalog()
        self.file("inputs/tools.tar", b"synthetic tools archive")
        envelope = self.envelope(family)
        envelope["build_args"]["TOOLS_ARCHIVE_SHA256"] = "e" * 64
        with self.assertRaises(ci.ContractError):
            self.run_build(family=family, inputs=envelope)
        self.assertEqual(self.commands, [])
        envelope["build_args"]["TOOLS_ARCHIVE_SHA256"] = sha(b"synthetic tools archive")
        self.assertEqual(self.run_build(family=family, inputs=envelope)["family"], family)

    def test_cli_emits_candidate_json_only_after_companion_comparison(self):
        inputs = self.root / "inputs.json"
        identity = self.root / "source.json"
        inputs.write_bytes(encoded(self.inputs))
        identity.write_bytes(encoded(source()))
        stdout = io.StringIO()
        with mock.patch.object(build.subprocess, "run", side_effect=self.docker), contextlib.redirect_stdout(stdout):
            code = build.main(["--repo-root", str(self.repo), "--family", FAMILY, "--inputs", str(inputs),
                               "--materials", str(self.materials), "--source", str(identity), "--output", str(self.output)])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(stdout.getvalue()), ci.load_json(self.output / "candidate.json"))
        self.assertEqual(len(self.commands), 3)

    def test_shell_wrapper_help_is_available_without_docker(self):
        result = subprocess.run(["sh", str(ROOT / "scripts/build_ci_environment.sh"), "--help"],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        for option in ("--family", "--materials", "--source", "--inputs", "--output", "--builder"):
            self.assertIn(option, result.stdout)


if __name__ == "__main__":
    unittest.main()
