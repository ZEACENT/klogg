"""Publisher orchestration with real OCI fixtures and mocked trusted services.

No actual images are built/published, no real signatures are claimed, and no
network tools are downloaded. Synthetic job results stand in for CI authority.
"""
import copy
import hashlib
import importlib
import io
import json
import os
import pathlib
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

import test_ci_environment_pipeline as pipeline_fixtures


ROOT = pathlib.Path(__file__).parents[2]
MODULE = importlib.import_module("publish_ci_environment")
CORE = MODULE.core
PIPELINE = MODULE.pipeline
TSAN = "jammy-qt5-tsan"
SOURCE_VERIFIER = "scripts/verify_tsan_qt_sources.py"


def encoded(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")


def sha(data):
    return hashlib.sha256(data).hexdigest()


class PublishCiEnvironmentTest(unittest.TestCase):
    def setUp(self):
        self.fixture = pipeline_fixtures.PipelineAggregateTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.repo = self.fixture.repo.resolve()
        self.source = self.fixture.source
        self.evidence = self.fixture.evidence.resolve()
        self.qualified = self.fixture.output.resolve()
        profiles_path = self.repo / "ci/environments/profiles.json"
        profiles = CORE.load_json(profiles_path)
        files = profiles["families"][TSAN]["tsan"]["verification_files"]
        if SOURCE_VERIFIER not in files:
            files.append(SOURCE_VERIFIER)
        (self.repo / SOURCE_VERIFIER).parent.mkdir(parents=True, exist_ok=True)
        (self.repo / SOURCE_VERIFIER).write_text("# Synthetic source-verification policy bytes.\n")
        profiles_path.write_bytes(encoded(profiles))
        self.materials = CORE.load_json(ROOT / "ci/environments/materials.json")
        (self.repo / "ci/environments/materials.json").write_bytes(encoded(self.materials))
        for family, candidate in self.fixture.candidates.items():
            directory = self.evidence / "candidates" / family
            definition = self.materials["families"][family]
            names = {item["asset"] for key in ("downloads", "tools") for item in definition[key]}
            manifest = {"schema_version": 1, "kind": "ci-material-acquisition", "family": family,
                        "definition": definition, "assets": {name: self.materials["assets"][name] for name in names},
                        "transforms": []}
            raw = encoded(manifest)
            (directory / "material-manifest.json").write_bytes(raw)
            inputs = CORE.load_json(directory / "inputs.json")
            inputs["build_args"] = dict(definition["build_args"], UBUNTU_IMAGE=definition["base_image"])
            inputs["files"] = [{"path": item["path"], "sha256": self.materials["assets"][item["asset"]]["sha256"], "size": 123}
                               for item in definition["downloads"]]
            inputs["files"].append({"path": "inputs/material-manifest.json", "sha256": sha(raw), "size": len(raw)})
            (directory / "inputs.json").write_bytes(encoded(inputs))
            candidate["input_digest"] = CORE.input_identity(inputs)
            (directory / "candidate.json").write_bytes(encoded(candidate))
            transport = CORE.load_json(directory / "transport.json")
            transport["candidate_digest"] = CORE.canonical_digest(candidate)
            transport["inputs_manifest"]["sha256"] = sha((directory / "inputs.json").read_bytes())
            (directory / "transport.json").write_bytes(encoded(transport))
            for profile in PIPELINE.PROFILE_JOBS[family]:
                receipt = self.fixture.receipts[(family, profile)]
                receipt["candidate"] = candidate
                receipt["policy_digest"] = CORE.policy_identity(PIPELINE.family_policy(self.repo, family))
                self.fixture.save_receipt(family, profile)
        self.fixture.aggregate()
        self.transport = self.repo / "qualified-transport"
        self.transport.mkdir()
        self.qualified_id = 901
        self.publication_id = 902
        self.pack_qualified()
        self.output = self.repo / "publication"
        self.commands = []
        self.stdin_values = []
        self.downloads = []
        self.environment = {"GH_TOKEN": "synthetic-package-token", "GITHUB_ACTOR": "fixture-user"}
        self.tool_archive = self.make_tool_archive()
        tool = {"schema_version": 1, "oras": {"version": "1.3.4", "sha256": sha(self.tool_archive),
                "url": "https://github.com/oras-project/oras/releases/download/v1.3.4/oras_1.3.4_linux_amd64.tar.gz",
                "executable": "oras"}}
        (self.repo / "ci/environments/publisher-tools.json").write_bytes(encoded(tool))
        self.image = next(iter(self.fixture.candidates.values()))["image"]
        archive = self.evidence / "candidates" / TSAN / "candidate.oci.tar"
        with tarfile.open(archive, "r:") as stream:
            self.manifest_bytes = stream.extractfile("blobs/sha256/" + self.image["manifest_digest"][7:]).read()
        self.runner = mock.Mock(side_effect=self.run_command)

    def make_tool_archive(self):
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w:gz", format=tarfile.USTAR_FORMAT) as archive:
            for name, data in (("oras", b"fixture tool, never executed directly\n"), ("LICENSE", b"fixture license\n")):
                member = tarfile.TarInfo(name)
                member.size = len(data)
                member.mode = 0o755 if name == "oras" else 0o644
                archive.addfile(member, io.BytesIO(data))
        return buffer.getvalue()

    def pack_qualified(self):
        archive = self.transport / "qualified.tar.gz"
        with tarfile.open(archive, "w:gz", format=tarfile.USTAR_FORMAT) as stream:
            for path in sorted(self.qualified.rglob("*")):
                if path.is_file():
                    stream.add(path, arcname=path.relative_to(self.qualified).as_posix())
        self.qualified_sha = sha(archive.read_bytes())

    def downloader(self, url, destination):
        self.downloads.append(url)
        pathlib.Path(destination).write_bytes(self.tool_archive)

    def run_command(self, command, **kwargs):
        self.commands.append(command)
        self.stdin_values.append(kwargs.get("input"))
        self.assertTrue(kwargs["check"])
        if command[:2] == ["gh", "api"]:
            identifier = int(command[-1].rsplit("/", 1)[-1])
            self.assertIn(identifier, self.fixture.artifacts | {self.qualified_id, self.publication_id})
            result = {"id": identifier, "expired": False, "workflow_run": {
                "id": self.source["run_id"], "head_sha": self.source["sha"], "head_branch": "master"}}
            return subprocess.CompletedProcess(command, 0, json.dumps(result), "")
        if pathlib.Path(command[0]).name == "oras":
            if command[1:3] == ["manifest", "fetch"]:
                pathlib.Path(command[command.index("--output") + 1]).write_bytes(self.manifest_bytes)
            return subprocess.CompletedProcess(command, 0, "", "")
        if command[:3] == ["gh", "attestation", "verify"]:
            subject = pathlib.Path(command[3])
            name = "verification.json" if subject.name == "verification.json" else CORE.REGISTRY
            statement = {"predicateType": "https://slsa.dev/provenance/v1", "subject": [
                {"name": name, "digest": {"sha256": sha(subject.read_bytes())}}]}
            return subprocess.CompletedProcess(command, 0, json.dumps([{"verificationResult": {"statement": statement}}]), "")
        self.fail("unexpected publisher executable: " + repr(command[:3]))

    def publish(self, **overrides):
        arguments = dict(repo_root=self.repo, source=self.source, qualified_root=self.transport,
                         qualified_artifact_id=self.qualified_id, qualified_archive_sha256=self.qualified_sha,
                         evidence_root=self.evidence, output=self.output, runner=self.runner,
                         downloader=self.downloader, environment=self.environment)
        arguments.update(overrides)
        return MODULE.publish(**arguments)

    def test_publication_copies_all_original_archives_without_executing_candidate_images(self):
        result = self.publish()
        copies = [command for command in self.commands if pathlib.Path(command[0]).name == "oras" and command[1] == "cp"]
        self.assertEqual(len(copies), 6)
        self.assertEqual(set(result["families"]), set(PIPELINE.BUILD_JOBS))
        for command in copies:
            self.assertIn("--from-oci-layout", command)
            self.assertTrue(any("candidate.oci.tar@sha256:" in argument for argument in command))
        self.assertFalse(any(command[0] == "docker" for command in self.commands))

    def test_qualify_only_evidence_cannot_acquire_tools_or_publish(self):
        path = self.qualified / "qualified.json"
        document = CORE.load_json(path)
        document.update(operation="qualify", sarif_result="skipped")
        path.write_bytes(encoded(document))
        self.pack_qualified()
        with self.assertRaises(CORE.ContractError):
            self.publish()
        self.assertEqual(self.downloads, [])
        self.assertFalse(any(pathlib.Path(command[0]).name == "oras" for command in self.commands))

    def assert_no_registry_execution(self):
        self.assertFalse(any(pathlib.Path(command[0]).name == "oras" for command in self.commands))
        self.assertEqual(self.downloads, [])

    def requalify(self):
        shutil.rmtree(self.qualified)
        for (family, profile), receipt in self.fixture.receipts.items():
            receipt["policy_digest"] = CORE.policy_identity(PIPELINE.family_policy(self.repo, family))
            self.fixture.save_receipt(family, profile)
        self.fixture.aggregate()
        self.pack_qualified()

    def test_all_families_validate_before_any_registry_write(self):
        family = sorted(PIPELINE.BUILD_JOBS)[-1]
        path = self.evidence / "candidates" / family / "candidate.oci.tar"
        path.write_bytes(path.read_bytes() + b"tampered")
        with self.assertRaises(CORE.ContractError):
            self.publish()
        self.assert_no_registry_execution()
        self.assertFalse(self.output.exists())

    def test_gate_tar_sha_source_and_artifact_identity_are_external_authority(self):
        for override in ({"qualified_archive_sha256": "f" * 64},
                         {"source": dict(self.source, run_attempt=self.source["run_attempt"] + 1)}):
            with self.subTest(override=override), self.assertRaises(CORE.ContractError):
                self.publish(**override)
        self.assert_no_registry_execution()

    def test_current_policy_reaggregation_rejects_self_consistent_gate_receipt_claims(self):
        file = self.repo / SOURCE_VERIFIER
        file.write_bytes(file.read_bytes() + b"# Changed current verifier.\n")
        with self.assertRaises(CORE.ContractError):
            self.publish()
        self.assert_no_registry_execution()

    def test_missing_source_verifier_policy_is_hard_block_even_if_requalified(self):
        path = self.repo / "ci/environments/profiles.json"
        profiles = CORE.load_json(path)
        profiles["families"][TSAN]["tsan"]["verification_files"].remove(SOURCE_VERIFIER)
        path.write_bytes(encoded(profiles))
        self.requalify()
        with self.assertRaisesRegex(CORE.ContractError, "source|redistribution"):
            self.publish()
        self.assert_no_registry_execution()

    def test_original_qt_archive_declarations_cannot_be_replaced_by_notices_only(self):
        directory = self.evidence / "candidates" / TSAN
        inputs = CORE.load_json(directory / "inputs.json")
        original_record = next(item for item in inputs["files"] if item["path"].startswith("inputs/qt/"))
        for change in ("missing", "wrong-hash"):
            with self.subTest(change=change):
                modified = copy.deepcopy(inputs)
                if change == "missing":
                    modified["files"] = [item for item in modified["files"] if item["path"] != original_record["path"]]
                else:
                    next(item for item in modified["files"] if item["path"] == original_record["path"])["sha256"] = "e" * 64
                candidate = self.fixture.candidates[TSAN]
                candidate["input_digest"] = CORE.input_identity(modified)
                (directory / "inputs.json").write_bytes(encoded(modified))
                (directory / "candidate.json").write_bytes(encoded(candidate))
                transport = CORE.load_json(directory / "transport.json")
                transport["candidate_digest"] = CORE.canonical_digest(candidate)
                transport["inputs_manifest"]["sha256"] = sha((directory / "inputs.json").read_bytes())
                (directory / "transport.json").write_bytes(encoded(transport))
                self.requalify()
                with self.assertRaisesRegex(CORE.ContractError, "source|Qt|archive"):
                    self.publish()
                self.assert_no_registry_execution()

    def test_raw_gate_verification_receipt_is_not_replaced_by_a_canonical_hash(self):
        manifest_path = self.qualified / "qualified.json"
        document = CORE.load_json(manifest_path)
        family = next(iter(PIPELINE.BUILD_JOBS))
        receipt = CORE.load_json(self.qualified / "families" / family / "verification.json")
        document["families"][family]["receipt_sha256"] = CORE.canonical_digest(receipt)[7:]
        manifest_path.write_bytes(encoded(document))
        self.pack_qualified()
        with self.assertRaisesRegex(CORE.ContractError, "receipt"):
            self.publish()
        self.assert_no_registry_execution()

    def test_workflow_gnu_tar_root_directory_and_dot_prefix_are_supported(self):
        archive = self.transport / "qualified.tar.gz"
        with tarfile.open(archive, "w:gz", format=tarfile.GNU_FORMAT) as stream:
            stream.add(self.qualified, arcname=".")
        self.qualified_sha = sha(archive.read_bytes())
        self.assertEqual(set(self.publish()["families"]), set(PIPELINE.BUILD_JOBS))

    def test_gate_tar_path_escape_symlink_and_extension_records_fail_closed(self):
        archive = self.transport / "qualified.tar.gz"
        for kind in ("escape", "symlink", "pax"):
            with self.subTest(kind=kind):
                with tarfile.open(archive, "w:gz", format=tarfile.PAX_FORMAT if kind == "pax" else tarfile.USTAR_FORMAT) as stream:
                    member = tarfile.TarInfo("../escaped" if kind == "escape" else "qualified.json")
                    data = b"{}"
                    member.size = len(data)
                    if kind == "symlink":
                        member.type, member.linkname, member.size = tarfile.SYMTYPE, "../outside", 0
                    if kind == "pax":
                        member.pax_headers = {"comment": "unsupported extension"}
                    stream.addfile(member, io.BytesIO(data) if member.isfile() else None)
                self.qualified_sha = sha(archive.read_bytes())
                with self.assertRaises(CORE.ContractError):
                    self.publish()
        self.assert_no_registry_execution()
        self.assertFalse((self.repo / "escaped").exists())

    def test_tool_pin_mismatch_and_missing_credentials_never_reach_oras(self):
        self.tool_archive += b"substituted download"
        with self.assertRaises(CORE.ContractError):
            self.publish()
        self.assertFalse(any(pathlib.Path(command[0]).name == "oras" for command in self.commands))
        self.downloads.clear()
        with self.assertRaises(CORE.ContractError):
            self.publish(environment={"GITHUB_ACTOR": "fixture-user"})
        self.assertEqual(self.downloads, [])

    def test_auth_is_private_temporary_and_token_is_stdin_only(self):
        self.publish()
        login_calls = [call for call in self.runner.call_args_list if pathlib.Path(call.args[0][0]).name == "oras" and call.args[0][1] == "login"]
        self.assertEqual(len(login_calls), 1)
        command = login_calls[0].args[0]
        self.assertIn("--password-stdin", command)
        self.assertEqual(login_calls[0].kwargs["input"], self.environment["GH_TOKEN"] + "\n")
        config = pathlib.Path(command[command.index("--registry-config") + 1])
        self.assertFalse(config.exists(), "temporary authentication must be removed")
        self.assertFalse(any(self.environment["GH_TOKEN"] in argument for command in self.commands for argument in command))
        copies = [command for command in self.commands if pathlib.Path(command[0]).name == "oras" and command[1] == "cp"]
        self.assertTrue(all(str(config) == command[command.index("--to-registry-config") + 1] for command in copies))
        self.assertFalse(any("docker" in command for command in self.commands))

    def test_authenticated_copy_digest_mismatch_does_not_emit_publication_success(self):
        original = self.runner.side_effect
        def wrong_manifest(command, **kwargs):
            result = original(command, **kwargs)
            if pathlib.Path(command[0]).name == "oras" and command[1:3] == ["manifest", "fetch"]:
                pathlib.Path(command[command.index("--output") + 1]).write_bytes(b"wrong remote manifest")
            return result
        self.runner.side_effect = wrong_manifest
        with self.assertRaises(CORE.ContractError):
            self.publish()
        self.assertFalse(self.output.exists())

    def make_bundles(self):
        directory = self.repo / "action-bundles"
        directory.mkdir(exist_ok=True)
        bundles = {}
        for family in PIPELINE.BUILD_JOBS:
            bundles[family] = {}
            for key in ("image_bundle", "receipt_bundle"):
                path = directory / (family + "-" + key + ".json")
                path.write_bytes(b"mocked trusted gh verification boundary\n")
                bundles[family][key] = str(path)
        return bundles

    def finalize(self, bundles=None, **overrides):
        arguments = dict(repo_root=self.repo, source=self.source, qualified_root=self.transport,
                         publication_root=self.output, bundle_map=bundles or self.make_bundles(),
                         output=self.repo / "publication-evidence", runner=self.runner)
        arguments.update(overrides)
        return MODULE.finalize_publication(**arguments)

    def test_finalization_verifies_twelve_detached_subjects_and_preserves_raw_receipts(self):
        self.publish()
        self.commands.clear()
        self.finalize()
        target = self.repo / "publication-evidence"
        verification = [command for command in self.commands if command[:3] == ["gh", "attestation", "verify"]]
        self.assertEqual(len(verification), 12)
        lock = CORE.load_json(target / "ci/environments/lock.json")
        CORE.validate_production_lock(lock, CORE.load_json(self.repo / "ci/environments/recipes.json"))
        for family in PIPELINE.BUILD_JOBS:
            raw = (self.qualified / "families" / family / "verification.json").read_bytes()
            evidence = lock["families"][family]["qualification"]
            self.assertEqual((target / evidence["receipt"]).read_bytes(), raw)
            self.assertEqual(evidence["digest"], "sha256:" + sha(raw))
            self.assertTrue((target / "ci/environments/inputs" / (family + ".json")).is_file())
        self.assertFalse((self.repo / "ci/environments/lock.json").exists())
        self.assertFalse(any(pathlib.Path(command[0]).name == "oras" for command in self.commands))

    def test_missing_or_invalid_detached_signature_never_produces_lock_proposal(self):
        self.publish()
        bundles = self.make_bundles()
        pathlib.Path(bundles[TSAN]["receipt_bundle"]).unlink()
        with self.assertRaises((CORE.ContractError, OSError)):
            self.finalize(bundles)
        bundles = self.make_bundles()
        original = self.runner.side_effect
        def invalid(command, **kwargs):
            if command[:3] == ["gh", "attestation", "verify"]:
                raise subprocess.CalledProcessError(1, command)
            return original(command, **kwargs)
        self.runner.side_effect = invalid
        with self.assertRaises(MODULE.registry.RegistryError):
            self.finalize(bundles)
        self.assertFalse((self.repo / "publication-evidence").exists())

    def publication_transport(self):
        directory = self.repo / "publication-transport"
        directory.mkdir()
        artifact = directory / "publication-evidence.tar.gz"
        evidence = self.repo / "publication-evidence"
        with tarfile.open(artifact, "w:gz", format=tarfile.USTAR_FORMAT) as stream:
            for path in sorted(evidence.rglob("*")):
                if path.is_file():
                    stream.add(path, arcname=path.relative_to(evidence).as_posix())
        return directory, sha(artifact.read_bytes())

    def public_client(self):
        client = mock.Mock()
        client.read_image.return_value = {key: self.image[key] for key in (
            "manifest_digest", "config_digest", "platform", "diff_ids", "layer_digests")}
        client.read_image.return_value["manifest_bytes"] = self.manifest_bytes
        return client

    def verify_public(self, client, **overrides):
        transport, digest = self.publication_transport()
        arguments = dict(repo_root=self.repo, source=self.source, publication_root=transport,
                         publication_artifact_id=self.publication_id, archive_sha256=digest,
                         output=self.repo / "public-verified", runner=self.runner, client=client)
        arguments.update(overrides)
        return MODULE.verify_publication(**arguments)

    def test_public_verification_checks_all_families_anonymously_and_writes_only_output(self):
        self.publish()
        self.finalize()
        self.commands.clear()
        client = self.public_client()
        self.verify_public(client)
        self.assertEqual(client.read_image.call_count, 6)
        self.assertEqual(sum(command[:3] == ["gh", "attestation", "verify"] for command in self.commands), 12)
        self.assertTrue((self.repo / "public-verified/ci/environments/lock.json").is_file())
        self.assertFalse((self.repo / "ci/environments/lock.json").exists())
        self.assertFalse(any(pathlib.Path(command[0]).name == "oras" or command[0] == "docker" for command in self.commands))

    def test_public_proposal_cannot_smuggle_unrelated_configuration_files(self):
        self.publish()
        self.finalize()
        root = self.repo / "publication-evidence"
        malicious = "ci/environments/profiles.json"
        (root / malicious).write_bytes(b'{"weakened":"policy"}\n')
        manifest_path = root / "publication-evidence.json"
        metadata = CORE.load_json(manifest_path)
        metadata["files"][malicious] = sha((root / malicious).read_bytes())
        manifest_path.write_bytes(encoded(metadata))
        with self.assertRaisesRegex(CORE.ContractError, "inventory|unexpected|proposal"):
            self.verify_public(self.public_client())
        self.assertFalse((self.repo / "public-verified").exists())

    def test_signature_bundle_symlink_parent_is_rejected(self):
        self.publish()
        bundles = self.make_bundles()
        real = self.repo / "action-bundles"
        alias = self.repo / "bundle-alias"
        try:
            alias.symlink_to(real, target_is_directory=True)
        except OSError as error:
            self.skipTest("host cannot create symlinks: " + str(error))
        bundles[TSAN]["receipt_bundle"] = str(alias / pathlib.Path(bundles[TSAN]["receipt_bundle"]).name)
        with self.assertRaisesRegex(CORE.ContractError, "symlink"):
            self.finalize(bundles)
        self.assertFalse((self.repo / "publication-evidence").exists())

    def test_matching_source_data_cannot_bypass_a_wrong_attested_subject(self):
        self.publish()
        original = self.runner.side_effect
        def wrong_subject(command, **kwargs):
            result = original(command, **kwargs)
            if command[:3] == ["gh", "attestation", "verify"]:
                verified = json.loads(result.stdout)
                verified[0]["verificationResult"]["statement"]["subject"][0]["name"] = "unrelated.json"
                result.stdout = json.dumps(verified)
            return result
        self.runner.side_effect = wrong_subject
        with self.assertRaises(MODULE.registry.RegistryError):
            self.finalize()
        self.assertFalse((self.repo / "publication-evidence").exists())

    def test_existing_output_is_never_overwritten_or_used_as_a_success_cache(self):
        self.output.mkdir()
        sentinel = self.output / "untouched"
        sentinel.write_bytes(b"existing state\n")
        with self.assertRaises(CORE.ContractError):
            self.publish()
        self.assertEqual(sentinel.read_bytes(), b"existing state\n")
        self.assert_no_registry_execution()

    def test_public_metadata_config_substitution_cannot_activate_a_proposal(self):
        self.publish()
        self.finalize()
        client = self.public_client()
        client.read_image.return_value["config_digest"] = "sha256:" + "f" * 64
        with self.assertRaises(CORE.ContractError):
            self.verify_public(client)
        self.assertFalse((self.repo / "public-verified").exists())

    def test_changed_current_policy_invalidates_previously_signed_public_proposal(self):
        self.publish()
        self.finalize()
        path = self.repo / SOURCE_VERIFIER
        path.write_bytes(path.read_bytes() + b"# New qualification policy.\n")
        with self.assertRaises(CORE.ContractError):
            self.verify_public(self.public_client())
        self.assertFalse((self.repo / "public-verified").exists())

    def test_private_image_does_not_destroy_retained_evidence_or_activate_lock(self):
        self.publish()
        self.finalize()
        retained = self.repo / "publication-evidence/ci/environments/lock.json"
        original = retained.read_bytes()
        client = self.public_client()
        client.read_image.side_effect = MODULE.registry.RegistryError("private image")
        with self.assertRaises(MODULE.registry.RegistryError):
            self.verify_public(client)
        self.assertEqual(retained.read_bytes(), original)
        self.assertFalse((self.repo / "public-verified").exists())
        self.assertFalse((self.repo / "ci/environments/lock.json").exists())


if __name__ == "__main__":
    unittest.main()
