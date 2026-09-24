"""Same-run candidate and qualification orchestration without external services."""
import copy
import hashlib
import importlib
import json
import os
import pathlib
import subprocess
import sys
import tarfile
import io
import tempfile
import unittest
from unittest import mock

import test_ci_environment_profiles as policy_fixtures

import test_build_ci_environment as fixtures

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))


class PipelineEntryTest(unittest.TestCase):
    def test_command_lists_real_candidate_and_qualification_operations(self):
        result = subprocess.run([sys.executable, str(ROOT / "scripts/ci_environment_pipeline.py"), "--help"],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        for command in ("source-context", "prepare-candidate", "profile-receipt", "aggregate"):
            self.assertIn(command, result.stdout)


class PipelineCandidateTest(unittest.TestCase):
    def setUp(self):
        self.pipeline = importlib.import_module("ci_environment_pipeline")
        self.fixture = fixtures.BuildEnvironmentTest()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.source = fixtures.source()
        self.source["workflow"] = ".github/workflows/ci-environments.yml"
        self.candidate = self.fixture.run_build(source=self.source)
        self.commands = []
        self.artifact = {"id": 321, "expired": False, "workflow_run": {
            "id": self.source["run_id"], "head_sha": self.source["sha"], "head_branch": "master"}}
        self.inspect = self.fixture.loaded_image
        self.load_output = "Loaded image ID: " + self.inspect["Id"] + "\n"

    def runner(self, command, **kwargs):
        self.commands.append(command)
        if command[:2] == ["gh", "api"]:
            return subprocess.CompletedProcess(command, 0, json.dumps(self.artifact), "")
        if command[:2] == ["docker", "load"]:
            return subprocess.CompletedProcess(command, 0, self.load_output, "")
        if command[:3] == ["docker", "image", "inspect"]:
            return subprocess.CompletedProcess(command, 0, json.dumps([self.inspect]), "")
        if command[:3] == ["docker", "image", "tag"]:
            return subprocess.CompletedProcess(command, 0, "", "")
        self.fail("unexpected command: " + repr(command))

    def prepare(self, **overrides):
        arguments = dict(repo_root=self.fixture.repo, family=fixtures.FAMILY,
                         candidate_root=self.fixture.output, candidate_artifact_id=321,
                         archive_sha256=self.candidate["image"]["archive_sha256"], source=self.source,
                         output=self.fixture.root / "prepared", runner=self.runner)
        arguments.update(overrides)
        return self.pipeline.prepare_candidate(**arguments)

    def write_document(self, name, document):
        (self.fixture.output / name).write_text(json.dumps(document), encoding="ascii")

    def test_verified_candidate_loads_original_companion_then_retags_exact_image(self):
        result = self.prepare()
        self.assertEqual(result["candidate_artifact_id"], 321)
        self.assertEqual(result["candidate_digest"], fixtures.ci.canonical_digest(self.candidate))
        self.assertEqual([command[0] for command in self.commands], ["gh", "docker", "docker", "docker"])
        self.assertEqual(self.commands[-1][-2:], [self.inspect["Id"], "zeacent/klogg_ubuntu22.04"])
        self.assertTrue((self.fixture.root / "prepared/preparation.json").is_file())
        self.assertFalse(any("build" in command or "push" in command for command in self.commands))

    def test_wrong_run_expired_or_substituted_artifact_never_loads(self):
        for change in ("run", "commit", "branch", "id", "expired"):
            with self.subTest(change=change):
                original = copy.deepcopy(self.artifact)
                if change == "run":
                    self.artifact["workflow_run"]["id"] += 1
                elif change == "commit":
                    self.artifact["workflow_run"]["head_sha"] = "c" * 40
                elif change == "branch":
                    self.artifact["workflow_run"]["head_branch"] = "other"
                elif change == "id":
                    self.artifact["id"] = 322
                else:
                    self.artifact["expired"] = True
                self.commands.clear()
                with self.assertRaises(self.pipeline.PipelineError):
                    self.prepare()
                self.assertFalse(any(command[0] == "docker" for command in self.commands))
                self.artifact = original

    def test_wrong_external_archive_hash_fails_before_execution(self):
        with self.assertRaises(self.pipeline.PipelineError):
            self.prepare(archive_sha256="c" * 64)
        self.assertEqual(self.commands, [])

    def test_tampered_companion_or_inputs_never_loads(self):
        for name in ("candidate.docker.tar", "inputs.json"):
            with self.subTest(name=name):
                path = self.fixture.output / name
                original = path.read_bytes()
                path.write_bytes(original + b" ")
                self.commands.clear()
                with self.assertRaises(self.pipeline.PipelineError):
                    self.prepare()
                self.assertFalse(any(command[0] == "docker" for command in self.commands))
                path.write_bytes(original)

    def test_recipe_or_run_attempt_drift_is_not_qualified(self):
        for change in ("recipe", "attempt"):
            with self.subTest(change=change):
                if change == "recipe":
                    path = self.fixture.context / "recipe.patch"
                    original = path.read_bytes()
                    path.write_bytes(original + b"changed")
                    arguments = {}
                else:
                    arguments = {"source": dict(self.source, run_attempt=3)}
                with self.assertRaises(self.pipeline.PipelineError):
                    self.prepare(**arguments)
                if change == "recipe":
                    path.write_bytes(original)
        self.assertEqual(self.commands, [])

    def test_different_loaded_identity_cannot_be_proved_by_preexisting_image(self):
        self.load_output = "Loaded image ID: sha256:" + "d" * 64 + "\n"
        with self.assertRaises(self.pipeline.PipelineError):
            self.prepare()
        self.assertFalse(any(command[:3] == ["docker", "image", "tag"] for command in self.commands))

    def test_declared_material_provenance_must_be_retained_with_exact_bytes(self):
        manifest = fixtures.encoded({"schema_version": 1, "kind": "ci-material-acquisition", "family": fixtures.FAMILY})
        inputs = json.loads((self.fixture.output / "inputs.json").read_text())
        inputs["files"].append({"path": "inputs/material-manifest.json", "size": len(manifest), "sha256": fixtures.sha(manifest)})
        self.write_document("inputs.json", inputs)
        self.candidate["input_digest"] = fixtures.ci.input_identity(inputs)
        self.write_document("candidate.json", self.candidate)
        transport = json.loads((self.fixture.output / "transport.json").read_text())
        transport["candidate_digest"] = fixtures.ci.canonical_digest(self.candidate)
        transport["inputs_manifest"]["sha256"] = fixtures.sha((self.fixture.output / "inputs.json").read_bytes())
        self.write_document("transport.json", transport)
        for contents in (None, manifest + b" "):
            with self.subTest(missing=contents is None):
                if contents is not None:
                    (self.fixture.output / "material-manifest.json").write_bytes(contents)
                with self.assertRaises(self.pipeline.PipelineError):
                    self.prepare()
        self.assertEqual(self.commands, [])
        (self.fixture.output / "material-manifest.json").write_bytes(manifest)
        self.prepare()

    def test_companion_symlink_is_rejected_before_docker(self):
        path = self.fixture.output / "candidate.docker.tar"
        outside = self.fixture.root / "outside.tar"
        path.rename(outside)
        path.symlink_to(outside)
        with self.assertRaises(self.pipeline.PipelineError):
            self.prepare()
        self.assertEqual(self.commands, [])


class PipelineSourceTest(unittest.TestCase):
    def test_only_exact_dispatch_checkout_and_ancestor_base_are_accepted(self):
        pipeline = importlib.import_module("ci_environment_pipeline")
        source = fixtures.source()
        env = {"GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_REPOSITORY": "ZEACENT/klogg",
               "GITHUB_SHA": source["sha"], "GITHUB_REF": source["ref"],
               "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "2"}
        commands = []
        def runner(command, **kwargs):
            commands.append(command)
            return subprocess.CompletedProcess(command, 0, source["sha"] + "\n", "")
        actual = pipeline.source_context(source["sha"], "e" * 40, environment=env, runner=runner)
        self.assertEqual(actual["workflow"], pipeline.WORKFLOW)
        self.assertEqual(commands[-1], ["git", "merge-base", "--is-ancestor", "e" * 40, source["sha"]])
        for field, value in (("GITHUB_EVENT_NAME", "pull_request"), ("GITHUB_REPOSITORY", "fork/klogg"),
                             ("GITHUB_REF", "refs/pull/77/merge"), ("GITHUB_SHA", "f" * 40),
                             ("GITHUB_RUN_ATTEMPT", "0")):
            with self.subTest(field=field), self.assertRaises(pipeline.PipelineError):
                pipeline.source_context(source["sha"], "e" * 40, environment=dict(env, **{field: value}), runner=runner)


class PipelineAggregateTest(unittest.TestCase):
    def setUp(self):
        self.pipeline = importlib.import_module("ci_environment_pipeline")
        fixture = policy_fixtures.EnvironmentProfilesTest()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        self.repo = fixture.root
        (self.repo / "ci/environments/recipes.json").write_text(json.dumps(fixture.catalog), encoding="ascii")
        (self.repo / "ci/environments/role-materials.json").write_text(
            json.dumps({"codeql": [fixture.codeql_material()]}), encoding="ascii")
        self.evidence = self.repo / "evidence"
        self.output = self.repo / "qualified"
        self.source = dict(fixtures.source(), workflow=self.pipeline.WORKFLOW)
        self.identities = {"schema_version": 1, "candidates": {}, "profiles": {}}
        self.results = {}
        self.artifacts = set()
        self.commands = []
        self.receipts = {}
        self.candidates = {}
        oci_bytes, image = fixtures.oci_fixture()
        next_id = 100
        for family in sorted(self.pipeline.BUILD_JOBS):
            directory = self.evidence / "candidates" / family
            directory.mkdir(parents=True)
            (directory / "candidate.oci.tar").write_bytes(oci_bytes)
            (directory / "candidate.docker.tar").write_bytes(b"fake companion never executed by aggregate")
            inputs = {"schema_version": 1, "family": family, "platform": "linux/amd64", "files": [],
                      "build_args": {}, "apt_bundles": []}
            (directory / "inputs.json").write_bytes(fixtures.encoded(inputs))
            candidate = {"schema_version": 1, "kind": "candidate", "family": family,
                         "source": self.source, "recipe_digest": fixtures.ci.recipe_identity(fixture.catalog, family, self.repo),
                         "input_digest": fixtures.ci.input_identity(inputs), "image": fixtures.ci.inspect_oci_archive(directory / "candidate.oci.tar")}
            self.candidates[family] = candidate
            (directory / "candidate.json").write_bytes(fixtures.encoded(candidate))
            transport = {"schema_version": 1, "kind": "docker-load-transport", "candidate_digest": fixtures.ci.canonical_digest(candidate),
                         "config_digest": image["Id"], "diff_ids": image["RootFS"]["Layers"], "loaded_image_matches": True}
            for field, filename in (("oci_archive", "candidate.oci.tar"), ("docker_archive", "candidate.docker.tar"), ("inputs_manifest", "inputs.json")):
                transport[field] = {"path": filename, "sha256": fixtures.sha((directory / filename).read_bytes())}
            (directory / "transport.json").write_bytes(fixtures.encoded(transport))
            job = self.pipeline.BUILD_JOBS[family]
            next_id += 1
            self.identities["candidates"][family] = {"job": job, "artifact_id": next_id, "archive_sha256": fixtures.sha(oci_bytes)}
            self.artifacts.add(next_id)
            self.results[job] = "success"
            materials = {"codeql": [fixture.codeql_material()]} if family == policy_fixtures.ANALYSIS else None
            policy = fixture.policy(family, materials)
            self.identities["profiles"][family] = {}
            for profile, job in self.pipeline.PROFILE_JOBS[family].items():
                receipt = {"schema_version": 1, "kind": "qualification-receipt", "profile": profile, "result": "passed",
                           "candidate": candidate, "candidate_artifact_id": next_id,
                           "policy_digest": fixtures.ci.policy_identity(policy)}
                self.receipts[(family, profile)] = receipt
            for profile, job in self.pipeline.PROFILE_JOBS[family].items():
                next_id += 1
                self.artifacts.add(next_id)
                self.results[job] = "success"
                self.identities["profiles"][family][profile] = {"job": job, "artifact_id": next_id}
                self.save_receipt(family, profile)

    def save_receipt(self, family, profile, *, extra=False):
        data = fixtures.encoded(self.receipts[(family, profile)])
        directory = self.evidence / "receipts" / family / profile
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / "qualification.tar.gz"
        with tarfile.open(path, "w:gz", format=tarfile.USTAR_FORMAT) as archive:
            member = tarfile.TarInfo("receipt.json")
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
            if extra:
                member = tarfile.TarInfo("../unexpected")
                archive.addfile(member, io.BytesIO(b""))
        self.identities["profiles"][family][profile].update({"archive_sha256": fixtures.sha(path.read_bytes()), "receipt_sha256": fixtures.sha(data)})

    def runner(self, command, **kwargs):
        self.commands.append(command)
        self.assertEqual(command[:2], ["gh", "api"], "aggregate must never load or execute candidate images")
        artifact_id = int(command[-1].rsplit("/", 1)[-1])
        self.assertIn(artifact_id, self.artifacts)
        result = {"id": artifact_id, "expired": False, "workflow_run": {"id": self.source["run_id"],
                  "head_sha": self.source["sha"], "head_branch": "master"}}
        return subprocess.CompletedProcess(command, 0, json.dumps(result), "")

    def aggregate(self, **overrides):
        arguments = dict(repo_root=self.repo, source=self.source, evidence_root=self.evidence,
                         job_results=self.results, artifact_identities=self.identities,
                         operation="publish", sarif_result="success", output=self.output, runner=self.runner)
        arguments.update(overrides)
        return self.pipeline.aggregate(**arguments)

    def test_all_six_families_and_ten_profiles_produce_only_qualified_metadata(self):
        result = self.aggregate()
        self.assertEqual(result["operation"], "publish")
        self.assertEqual(set(result["families"]), set(self.pipeline.BUILD_JOBS))
        self.assertEqual(len(self.commands), 16)
        self.assertTrue((self.output / "qualified.json").is_file())
        self.assertFalse(list(self.output.rglob("*.tar")))
        for family in self.pipeline.BUILD_JOBS:
            receipt = json.loads((self.output / "families" / family / "verification.json").read_text())
            self.assertEqual(receipt["profiles"], sorted(self.pipeline.PROFILE_JOBS[family]))
            self.assertEqual(result["families"][family]["receipt_sha256"], fixtures.sha(
                (self.output / "families" / family / "verification.json").read_bytes()))

    def test_missing_skipped_and_extra_authoritative_jobs_fail(self):
        for change in ("missing", "skipped", "extra"):
            results = dict(self.results)
            if change == "missing":
                del results["QualifyTsan"]
            elif change == "skipped":
                results["QualifyTsan"] = "skipped"
            else:
                results["UnknownJob"] = "success"
            with self.subTest(change=change), self.assertRaises(self.pipeline.PipelineError):
                self.aggregate(job_results=results)
        self.assertFalse(self.output.exists())

    def test_qualify_mode_is_not_silently_promoted_and_sarif_is_mandatory_for_publish(self):
        with self.assertRaises(self.pipeline.PipelineError):
            self.aggregate(sarif_result="skipped")
        result = self.aggregate(operation="qualify", sarif_result="skipped")
        self.assertEqual(result["operation"], "qualify")
        self.assertEqual(result["sarif_result"], "skipped")

    def test_cross_attempt_receipt_is_rejected_even_when_all_jobs_pass(self):
        family, profile = "jammy-qt5", "ubsan"
        receipt = copy.deepcopy(self.receipts[(family, profile)])
        receipt["candidate"]["source"]["run_attempt"] += 1
        self.receipts[(family, profile)] = receipt
        self.save_receipt(family, profile)
        with self.assertRaises(self.pipeline.PipelineError):
            self.aggregate()
        self.assertFalse(self.output.exists())

    def test_external_receipt_hash_and_exact_archive_inventory_are_required(self):
        family, profile = "focal-qt5-gcc13", "appimage"
        identity = self.identities["profiles"][family][profile]
        original = identity["receipt_sha256"]
        identity["receipt_sha256"] = "f" * 64
        with self.assertRaises(self.pipeline.PipelineError):
            self.aggregate()
        identity["receipt_sha256"] = original
        self.save_receipt(family, profile, extra=True)
        with self.assertRaises(self.pipeline.PipelineError):
            self.aggregate()
        self.assertFalse(self.output.exists())

    def test_duplicate_artifact_ids_or_profile_job_substitution_fail(self):
        for field, value in (("artifact_id", self.identities["candidates"]["jammy-qt5"]["artifact_id"]),
                             ("job", "QualifyAsan")):
            identities = copy.deepcopy(self.identities)
            identities["profiles"]["jammy-qt5"]["ubsan"][field] = value
            with self.subTest(field=field), self.assertRaises(self.pipeline.PipelineError):
                self.aggregate(artifact_identities=identities)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
