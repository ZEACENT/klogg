"""Consumer tests use synthetic registry data and mocked trusted gh results.

These tests check orchestration, not real signatures or registry availability.
"""
import contextlib
import copy
import io
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parents[2] / "scripts" / "consume_ci_environment.py"
SPEC = importlib.util.spec_from_file_location("consume_ci_environment", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)
CORE = MODULE.core


def digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


class ConsumeCiEnvironmentTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name).resolve()
        self.family = "jammy-qt5"
        self.catalog = {
            "schema_version": 1, "registry": CORE.REGISTRY,
            "families": {self.family: {
                "platform": CORE.PLATFORM, "dockerfile": "docker/test/Dockerfile",
                "context": "docker/test", "recipe_files": ["docker/test/Dockerfile"],
                "profiles": ["deb"],
            }},
        }
        self.profiles = {
            "schema_version": 1, "verification_files": ["verify.py"],
            "linux_defaults": {"cmake_options": [], "verification_files": []},
            "families": {self.family: {"deb": {
                "configuration": {"cmake_options": [], "sanitizer": "", "package": False,
                                  "role": "build", "environment": {}, "build_root": "build_root",
                                  "package_settings": {}},
                "verification_files": [], "role_materials": [],
            }}},
        }
        self.inputs = {"schema_version": 1, "family": self.family, "platform": "linux/amd64", "materials": ["fixture-pin"]}
        self.write("docker/test/Dockerfile", b"FROM fixture-only\n")
        self.write("verify.py", b"# Fixture policy bytes, not a runnable verifier.\n")
        self.write_json("ci/environments/recipes.json", self.catalog)
        self.write_json("ci/environments/profiles.json", self.profiles)
        self.write_json("ci/environments/inputs/" + self.family + ".json", self.inputs)
        self.manifest = b'{"fixture":"synthetic registry metadata, not a real published image"}'
        self.image = {
            "schema_version": 1, "platform": CORE.PLATFORM, "archive_sha256": "a" * 64,
            "manifest_digest": digest(self.manifest), "config_digest": "sha256:" + "c" * 64,
            "diff_ids": ["sha256:" + "d" * 64], "layer_digests": ["sha256:" + "e" * 64],
        }
        self.source = {
            "repository": "ZEACENT/klogg", "sha": "1" * 40, "ref": "refs/heads/master",
            "workflow": ".github/workflows/ci-environments.yml", "run_id": 7, "run_attempt": 1,
        }
        self.policy = MODULE.profiles_module.build_policy(self.catalog, self.profiles, self.family, self.root)
        self.candidate = {
            "schema_version": 1, "kind": "candidate", "family": self.family,
            "recipe_digest": CORE.recipe_identity(self.catalog, self.family, self.root),
            "input_digest": CORE.input_identity(self.inputs), "source": self.source, "image": self.image,
        }
        self.receipt = {
            "schema_version": 1, "kind": "qualification", "candidate": self.candidate,
            "candidate_artifact_id": 17, "policy_digest": CORE.policy_identity(self.policy),
            "profiles": ["deb"], "receipts_digest": "sha256:" + "f" * 64,
        }
        base = "ci/environments/evidence/" + self.family + "/"
        self.evidence = {
            "digest": "sha256:" + "9" * 64, "receipt": base + "verification.json",
            "image_bundle": base + "image.sigstore.json", "receipt_bundle": base + "receipt.sigstore.json",
        }
        self.locked = {
            "image": CORE.REGISTRY + "@" + self.image["manifest_digest"], "platform": CORE.PLATFORM,
            "config_digest": self.image["config_digest"], "recipe_digest": self.candidate["recipe_digest"],
            "input_digest": self.candidate["input_digest"], "qualification": self.evidence, "source": self.source,
        }
        self.lock = {"schema_version": 1, "kind": "production-lock", "families": {self.family: self.locked}}
        self.write(self.evidence["image_bundle"], b"fixture mocked signature boundary\n")
        self.write(self.evidence["receipt_bundle"], b"fixture mocked signature boundary\n")
        self.save_receipt()
        self.events = []
        self.registry_data = {key: self.image[key] for key in (
            "manifest_digest", "config_digest", "platform", "diff_ids", "layer_digests")}
        self.registry_data["manifest_bytes"] = self.manifest
        self.client = mock.Mock()
        self.client.read_image.side_effect = self.read_image
        self.runner = mock.Mock(side_effect=self.run_command)

    def write(self, relative, data):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def write_json(self, relative, document):
        return self.write(relative, json.dumps(document, indent=2).encode("utf-8") + b"\n")

    def save_lock(self):
        self.write_json("ci/environments/lock.json", self.lock)

    def save_receipt(self):
        path = self.write_json(self.evidence["receipt"], self.receipt)
        self.evidence["digest"] = digest(path.read_bytes())
        self.save_lock()

    def read_image(self, value):
        self.events.append("anonymous-registry")
        self.assertEqual(value, self.image["manifest_digest"])
        return copy.deepcopy(self.registry_data)

    def run_command(self, command, **kwargs):
        self.assertTrue(kwargs["check"])
        if command[:3] == ["gh", "attestation", "verify"]:
            subject = pathlib.Path(command[3])
            name = "verification.json" if subject.name == "verification.json" else CORE.REGISTRY
            self.events.append("verify:" + name)
            self.assertEqual(command[command.index("--source-digest") + 1], self.source["sha"])
            self.assertEqual(command[command.index("--source-ref") + 1], self.source["ref"])
            statement = {
                "predicateType": "https://slsa.dev/provenance/v1",
                "subject": [{"name": name, "digest": {"sha256": hashlib.sha256(subject.read_bytes()).hexdigest()}}],
            }
            return subprocess.CompletedProcess(command, 0, json.dumps([{"verificationResult": {"statement": statement}}]), "")
        self.events.append(" ".join(command[:3]))
        if command[:3] == ["docker", "image", "inspect"]:
            return subprocess.CompletedProcess(command, 0, json.dumps([{
                "Id": self.image["config_digest"], "Os": "linux", "Architecture": "amd64",
                "RootFS": {"Type": "layers", "Layers": self.image["diff_ids"]},
            }]), "")
        return subprocess.CompletedProcess(command, 0, "", "")

    def consume(self, **kwargs):
        return MODULE.consume(self.family, self.root, client=self.client, runner=self.runner, **kwargs)

    def test_valid_resolution_requires_two_detached_verifications_and_public_metadata(self):
        result = self.consume()
        self.assertEqual(result["image"], self.locked["image"])
        self.assertEqual(self.runner.call_count, 2)
        self.client.read_image.assert_called_once_with(self.image["manifest_digest"])
        self.assertIn("verify:verification.json", self.events)
        self.assertIn("verify:" + CORE.REGISTRY, self.events)
        self.assertFalse(any(event.startswith("docker") for event in self.events))

    def test_self_consistent_fake_receipt_cannot_bypass_signature_failure(self):
        self.runner.side_effect = subprocess.CalledProcessError(1, ["gh", "attestation", "verify"])
        with self.assertRaises(MODULE.registry.RegistryError):
            self.consume()

    def test_missing_lock_never_bootstraps_builds_or_pulls(self):
        (self.root / "ci/environments/lock.json").unlink()
        with self.assertRaises((CORE.ContractError, OSError)):
            self.consume(pull=True)
        self.runner.assert_not_called()
        self.client.read_image.assert_not_called()

    def test_current_recipe_input_and_policy_changes_are_rejected_offline(self):
        for relative, replacement in (
            ("docker/test/Dockerfile", b"FROM different-fixture\n"),
            ("verify.py", b"# Changed qualification implementation.\n"),
            ("ci/environments/inputs/" + self.family + ".json", b'{"schema_version":1,"materials":["changed"]}'),
            ("ci/environments/profiles.json", json.dumps({
                **self.profiles, "linux_defaults": {"cmake_options": ["-DCHANGED=ON"], "verification_files": []},
            }).encode()),
        ):
            with self.subTest(relative=relative):
                path = self.root / relative
                before = path.read_bytes()
                path.write_bytes(replacement)
                try:
                    with self.assertRaises(CORE.ContractError):
                        self.consume()
                finally:
                    path.write_bytes(before)
        self.runner.assert_not_called()
        self.client.read_image.assert_not_called()

    def test_input_envelope_requires_exact_family_and_platform_even_when_hashes_agree(self):
        original = copy.deepcopy(self.inputs)
        for field, replacement in (("family", None), ("family", "other-family"),
                                   ("platform", None), ("platform", "linux/arm64")):
            with self.subTest(field=field, replacement=replacement):
                inputs = copy.deepcopy(original)
                if replacement is None:
                    del inputs[field]
                else:
                    inputs[field] = replacement
                self.write_json("ci/environments/inputs/" + self.family + ".json", inputs)
                self.locked["input_digest"] = CORE.input_identity(inputs)
                self.candidate["input_digest"] = self.locked["input_digest"]
                self.save_receipt()
                with self.assertRaises(CORE.ContractError):
                    self.consume()

    def test_receipt_digest_is_raw_file_sha_not_canonical_json_hash(self):
        self.assertNotEqual(self.evidence["digest"], CORE.canonical_digest(self.receipt))
        self.evidence["digest"] = CORE.canonical_digest(self.receipt)
        self.save_lock()
        with self.assertRaisesRegex(CORE.ContractError, "receipt.*digest"):
            self.consume()
        self.runner.assert_not_called()

    def test_receipt_whitespace_change_is_not_reserialized_away(self):
        path = self.root / self.evidence["receipt"]
        path.write_bytes(path.read_bytes() + b" \n")
        with self.assertRaisesRegex(CORE.ContractError, "receipt.*digest"):
            self.consume()

    def test_duplicate_json_keys_and_malformed_evidence_fail_closed(self):
        for data in (b'{"schema_version":1,"schema_version":1}', b'not json'):
            with self.subTest(data=data):
                self.write(self.evidence["receipt"], data)
                self.evidence["digest"] = digest(data)
                self.save_lock()
                with self.assertRaises(CORE.ContractError):
                    self.consume()
        self.runner.assert_not_called()

    def test_exact_receipt_kind_candidate_family_source_and_profile_set_are_required(self):
        original = copy.deepcopy(self.receipt)
        mutations = (
            lambda r: r.update(kind="candidate"),
            lambda r: r.update(profiles=[]),
            lambda r: r.update(profiles=["deb", "deb"]),
            lambda r: r.update(policy_digest="sha256:" + "2" * 64),
            lambda r: r["candidate"].update(family="other-family"),
            lambda r: r["candidate"]["source"].update(sha="2" * 40),
            lambda r: r["candidate"]["image"].update(config_digest="sha256:" + "2" * 64),
            lambda r: r.update(candidate_artifact_id=True),
        )
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                self.receipt = copy.deepcopy(original)
                mutate(self.receipt)
                self.save_receipt()
                with self.assertRaises(CORE.ContractError):
                    self.consume()
        self.runner.assert_not_called()
        self.client.read_image.assert_not_called()

    def test_noncanonical_producer_workflow_is_not_trusted_even_if_json_agrees(self):
        self.source["workflow"] = ".github/workflows/arbitrary.yml"
        self.save_receipt()
        with self.assertRaises(MODULE.registry.RegistryError):
            self.consume()
        self.runner.assert_not_called()

    def test_missing_detached_bundle_cannot_be_replaced_by_consistent_receipt(self):
        (self.root / self.evidence["image_bundle"]).unlink()
        with self.assertRaises((CORE.ContractError, OSError)):
            self.consume()
        self.runner.assert_not_called()
        self.client.read_image.assert_not_called()

    def test_cross_family_evidence_and_parent_escape_are_rejected(self):
        original = self.evidence["receipt"]
        for value in ("ci/environments/evidence/other/verification.json",
                      "ci/environments/evidence/" + self.family + "/../verification.json",
                      str(self.root / original)):
            with self.subTest(value=value):
                self.evidence["receipt"] = value
                self.save_lock()
                with self.assertRaises(CORE.ContractError):
                    self.consume()
        self.runner.assert_not_called()

    def test_symlink_evidence_file_or_parent_is_rejected(self):
        path = self.root / self.evidence["receipt"]
        saved = path.with_name("original.json")
        path.rename(saved)
        try:
            path.symlink_to(saved)
        except OSError as error:
            self.skipTest("host cannot create symlinks: " + str(error))
        with self.assertRaisesRegex(CORE.ContractError, "symlink"):
            self.consume()
        path.unlink()
        saved.rename(path)
        parent = path.parent
        saved_parent = parent.with_name("original-family")
        parent.rename(saved_parent)
        parent.symlink_to(saved_parent, target_is_directory=True)
        with self.assertRaisesRegex(CORE.ContractError, "symlink"):
            self.consume()
        self.runner.assert_not_called()

    def test_registry_must_match_every_signed_image_identity_field(self):
        original = copy.deepcopy(self.registry_data)
        for key, value in (("config_digest", "sha256:" + "2" * 64), ("platform", "linux/arm64"),
                           ("diff_ids", []), ("layer_digests", []),
                           ("manifest_digest", "sha256:" + "2" * 64), ("manifest_bytes", b"substituted")):
            with self.subTest(key=key):
                self.registry_data = {**original, key: value}
                with self.assertRaises((CORE.ContractError, MODULE.registry.RegistryError)):
                    self.consume(pull=True)
        self.assertFalse(any(event.startswith("docker") for event in self.events))

    def test_anonymous_registry_failure_cannot_fall_back_to_docker_credentials(self):
        self.client.read_image.side_effect = MODULE.registry.RegistryError("not publicly available")
        with self.assertRaises(MODULE.registry.RegistryError):
            self.consume(pull=True)
        self.assertFalse(any(event.startswith("docker") for event in self.events))

    def test_second_signature_failure_prevents_pull(self):
        original = self.runner.side_effect
        def reject_image(command, **kwargs):
            if command[0] == "gh" and pathlib.Path(command[3]).name != "verification.json":
                raise subprocess.CalledProcessError(1, command)
            return original(command, **kwargs)
        self.runner.side_effect = reject_image
        with self.assertRaises(MODULE.registry.RegistryError):
            self.consume(pull=True)
        self.assertFalse(any(event.startswith("docker") for event in self.events))

    def test_pull_inspects_exact_digest_before_fixed_local_retag(self):
        result = self.consume(pull=True, retag=True)
        commands = [call.args[0] for call in self.runner.call_args_list]
        docker = [command for command in commands if command[0] == "docker"]
        self.assertEqual(docker, [
            ["docker", "pull", "--platform", "linux/amd64", self.locked["image"]],
            ["docker", "image", "inspect", self.locked["image"]],
            ["docker", "tag", self.locked["image"], "zeacent/klogg_ubuntu22.04"],
        ])
        first_docker = next(index for index, event in enumerate(self.events) if event.startswith("docker"))
        self.assertLess(self.events.index("anonymous-registry"), first_docker)
        self.assertLess(self.events.index("verify:" + CORE.REGISTRY), first_docker)
        self.assertLess(self.events.index("verify:verification.json"), first_docker)
        self.assertEqual(result["local_tag"], "zeacent/klogg_ubuntu22.04")

    def test_loaded_image_mismatch_blocks_local_retag(self):
        original = self.runner.side_effect
        def wrong_loaded(command, **kwargs):
            result = original(command, **kwargs)
            if command[:3] == ["docker", "image", "inspect"]:
                data = json.loads(result.stdout)
                data[0]["Id"] = "sha256:" + "2" * 64
                result.stdout = json.dumps(data)
            return result
        self.runner.side_effect = wrong_loaded
        with self.assertRaisesRegex(CORE.ContractError, "config digest"):
            self.consume(pull=True, retag=True)
        self.assertFalse(any(call.args[0][:2] == ["docker", "tag"] for call in self.runner.call_args_list))

    def test_retag_requires_explicit_pull(self):
        with self.assertRaises(CORE.ContractError):
            self.consume(retag=True)
        self.runner.assert_not_called()
        self.client.read_image.assert_not_called()

    def test_cli_json_and_github_output_are_emitted_only_after_verification(self):
        output = self.root / "github-output"
        output.write_text("previous=value\n")
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = MODULE.main(["--family", self.family, "--repo-root", str(self.root),
                                  "--github-output", str(output)], client=self.client, runner=self.runner)
        self.assertEqual(status, 0, stderr.getvalue())
        self.assertEqual(json.loads(stdout.getvalue())["image"], self.locked["image"])
        self.assertEqual(output.read_text(), "previous=value\nimage=" + self.locked["image"] + "\n")
        output.unlink()
        self.runner.side_effect = subprocess.CalledProcessError(1, ["gh"])
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = MODULE.main(["--family", self.family, "--repo-root", str(self.root),
                                  "--github-output", str(output)], client=self.client, runner=self.runner)
        self.assertEqual(status, 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertFalse(output.exists())

    def test_codeql_policy_requires_current_official_locked_role_materials(self):
        profile = self.profiles["families"][self.family].pop("deb")
        profile["configuration"]["role"] = "codeql"
        profile["role_materials"] = ["codeql-bundle"]
        self.profiles["families"][self.family]["codeql"] = profile
        self.catalog["families"][self.family]["profiles"] = ["codeql"]
        self.write_json("ci/environments/recipes.json", self.catalog)
        self.write_json("ci/environments/profiles.json", self.profiles)
        materials = {"codeql": [{
            "name": "codeql-bundle", "sha256": "4" * 64,
            "url": "https://github.com/github/codeql-action/releases/download/codeql-bundle-v2.23.4/codeql-bundle-linux64.tar.gz",
        }]}
        self.policy = MODULE.profiles_module.build_policy(
            self.catalog, self.profiles, self.family, self.root, role_materials=materials)
        self.receipt["policy_digest"] = CORE.policy_identity(self.policy)
        self.receipt["profiles"] = ["codeql"]
        self.save_receipt()
        with self.assertRaises((CORE.ContractError, OSError)):
            self.consume()
        self.runner.assert_not_called()
        self.write_json("ci/environments/role-materials.json", materials)
        self.assertEqual(self.consume()["image"], self.locked["image"])
        self.runner.reset_mock()
        materials["codeql"][0]["sha256"] = "5" * 64
        self.write_json("ci/environments/role-materials.json", materials)
        with self.assertRaisesRegex(CORE.ContractError, "policy digest"):
            self.consume()
        materials["codeql"][0]["url"] = "https://mirror.invalid/codeql.tar.gz"
        self.write_json("ci/environments/role-materials.json", materials)
        with self.assertRaisesRegex(CORE.ContractError, "official"):
            self.consume()
        self.runner.assert_not_called()

    def test_analysis_family_has_no_invented_local_retag(self):
        with self.assertRaisesRegex(CORE.ContractError, "retag contract"):
            MODULE.consume("noble-qt693-analysis", self.root, pull=True, retag=True,
                           client=self.client, runner=self.runner)
        self.runner.assert_not_called()
        self.client.read_image.assert_not_called()

    def test_wrong_verified_subject_name_still_fails(self):
        original = self.runner.side_effect
        def wrong_subject(command, **kwargs):
            result = original(command, **kwargs)
            data = json.loads(result.stdout)
            data[0]["verificationResult"]["statement"]["subject"][0]["name"] = "other-project"
            result.stdout = json.dumps(data)
            return result
        self.runner.side_effect = wrong_subject
        with self.assertRaisesRegex(MODULE.registry.RegistryError, "expected subject"):
            self.consume(pull=True)
        self.assertFalse(any(event.startswith("docker") for event in self.events))

    def test_signature_verification_uses_checked_raw_byte_snapshots(self):
        original = self.runner.side_effect
        receipt = (self.root / self.evidence["receipt"]).read_bytes()
        def replace_checked_in_receipt(command, **kwargs):
            if pathlib.Path(command[3]).name == "verification.json":
                self.write(self.evidence["receipt"], b"replacement during verification")
                self.assertEqual(pathlib.Path(command[3]).read_bytes(), receipt)
                self.assertNotEqual(pathlib.Path(command[3]), self.root / self.evidence["receipt"])
            return original(command, **kwargs)
        self.runner.side_effect = replace_checked_in_receipt
        self.assertEqual(self.consume()["image"], self.locked["image"])

    def test_symlink_github_output_never_changes_its_target(self):
        target = self.root / "original-output"
        target.write_text("unchanged\n")
        output = self.root / "github-output"
        try:
            output.symlink_to(target)
        except OSError as error:
            self.skipTest("host cannot create symlinks: " + str(error))
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            status = MODULE.main(["--family", self.family, "--repo-root", str(self.root),
                                  "--github-output", str(output)], client=self.client, runner=self.runner)
        self.assertEqual(status, 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertEqual(target.read_text(), "unchanged\n")

    def test_cli_has_no_image_digest_tag_or_policy_override(self):
        for flag in ("--image", "--digest", "--tag", "--policy"):
            with self.subTest(flag=flag), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as caught:
                    MODULE.main(["--family", self.family, flag, "override"], client=self.client, runner=self.runner)
                self.assertEqual(caught.exception.code, 2)
        self.runner.assert_not_called()


if __name__ == "__main__":
    unittest.main()
