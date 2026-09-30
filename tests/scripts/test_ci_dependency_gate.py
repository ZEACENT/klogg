"""Real Actions transport must bind seven candidate cores to signed full builds."""

import copy
import hashlib
import io
import json
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_dependency_gate as gate
import publish_ci_dependency as publisher
from ci_dependency_pipeline import BUILDER_JOBS


def zipped(files):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as archive:
        for name, data in files.items():
            archive.writestr(name, data)
    return stream.getvalue()


class FakeAPI:
    def __init__(self, source, needs):
        self.source = source
        self.responses = {}
        self.catalog = json.loads((ROOT / "ci/dependencies/catalog.json").read_bytes())
        self.jobs = []
        artifacts = []
        for number, (target, row) in enumerate(sorted(self.catalog["targets"].items()), 1):
            job_id = BUILDER_JOBS[target]
            candidate_id, full_id = 100 + number, 200 + number
            archive = row["archive_name"]
            candidate = (target + " candidate").encode()
            core = (target + " core").encode()
            tar = (target + " full").encode()
            needs[job_id] = {"result": "success", "outputs": {
                "artifact_id": str(candidate_id), "candidate_sha256": hashlib.sha256(candidate).hexdigest(),
                "archive_sha256": hashlib.sha256(core).hexdigest(), "full_artifact_id": str(full_id)}}
            self.responses[f"/actions/artifacts/{candidate_id}/zip"] = zipped({"candidate.json": candidate, archive: core})
            self.responses[f"/actions/artifacts/{full_id}/zip"] = zipped({
                (f"adb-helper-{row['target']}" if target.startswith("adb-") else f"ios-native-{row['target']}") + ".tar.gz": tar})
            ancestor = {"id": source["run_id"], "head_sha": source["sha"], "head_branch": "master"}
            artifacts.extend([{"id": candidate_id, "name": f"native-core-candidate-{target}-{source['run_id']}-{source['run_attempt']}",
                               "expired": False, "workflow_run": ancestor},
                              {"id": full_id, "name": f"adb-helper-{row['target']}" if target.startswith("adb-") else f"ios-native-{row['target']}",
                               "expired": False, "workflow_run": ancestor}])
            steps = [{"name": "Attest exact full native tar provenance", "status": "completed", "conclusion": "success"}]
            if target.startswith("ios-"):
                steps.extend({"name": name, "status": "completed", "conclusion": "success"}
                             for name in ("Verify pinned iOS producer toolchain",
                                          "Observe unreviewed iOS host tool inputs"))
            self.jobs.append({"id": 300 + number, "run_id": source["run_id"], "run_attempt": source["run_attempt"],
                              "head_sha": source["sha"], "name": f"Source-built ADB helper {row['target']}" if target.startswith("adb-") else f"iOS native stack {row['target']}",
                              "status": "completed", "conclusion": "success", "labels": [row["runner"]],
                              "runner_name": "GitHub Actions 1", "steps": steps})
        self.responses[f"/actions/runs/{source['run_id']}"] = {
            "id": source["run_id"], "run_attempt": source["run_attempt"], "head_sha": source["sha"],
            "head_branch": "master", "event": "workflow_dispatch", "path": source["workflow"],
            "repository": {"full_name": source["repository"]}, "status": "in_progress", "conclusion": None}
        self.responses[f"/actions/runs/{source['run_id']}/attempts/{source['run_attempt']}/jobs?per_page=100&page=1"] = {
            "total_count": 8, "jobs": self.jobs}
        self.needs = needs
        needs[gate.LEGAL_JOB] = {"result": "success", "outputs": {
            "support_artifact_id": "501", "full_release_artifact_id": "502"}}
        self.jobs.append({"id": 500, "run_id": source["run_id"], "run_attempt": source["run_attempt"],
                          "head_sha": source["sha"], "name": "Build ADB helper legal assets",
                          "status": "completed", "conclusion": "success",
                          "labels": ["ubuntu-24.04"], "runner_name": "GitHub Actions 1"})
        for identifier, name in ((501, "adb-helper-package-support"), (502, "adb-helper-legal-assets")):
            metadata = {"id": identifier, "name": name, "expired": False, "workflow_run": ancestor}
            artifacts.append(metadata)
            self.responses[f"/actions/artifacts/{identifier}"] = metadata
            self.responses[f"/actions/artifacts/{identifier}/zip"] = zipped({"receipt.json": b"fixture"})
        self.responses[f"/actions/runs/{source['run_id']}/artifacts?per_page=100&page=1"] = {
            "total_count": 16, "artifacts": artifacts}
        for item in artifacts:
            self.responses.setdefault(f"/actions/artifacts/{item['id']}", item)

    def get_json(self, endpoint):
        return copy.deepcopy(self.responses[endpoint])

    def get_bytes(self, endpoint, limit):
        raw = self.responses[endpoint]
        if len(raw) > limit:
            raise gate.GateError("oversized artifact response")
        return raw


class DependencyGateTest(unittest.TestCase):
    def setUp(self):
        self.source = {"repository": "ZEACENT/klogg", "workflow": ".github/workflows/ci-build.yml",
                       "event_name": "workflow_dispatch", "sha": "a" * 40,
                       "ref": "refs/heads/master", "run_id": 45, "run_attempt": 2}
        self.needs = {}
        self.api = FakeAPI(self.source, self.needs)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.destination = pathlib.Path(self.temp.name) / "receipts"
        self.real_legal = gate._legal_closure
        self.real_fetch = gate.fetch_attestation_bundle
        # Transport fixtures simulate a future reviewed host-tool closure;
        # the real catalog deliberately lacks one until the runners are measured.
        self.host_tool_review = mock.patch.object(gate, "require_reviewed_ios_host_tools")
        self.patches = [self.host_tool_review,
                        mock.patch.object(gate, "checkout_head", return_value=self.source["sha"]),
                        mock.patch.object(gate, "verify_full_evidence", return_value={
                            target: {"runner": row["runner"], **({"toolchain": row["toolchain"]} if "toolchain" in row else {})}
                            for target, row in self.api.catalog["targets"].items()}),
                        mock.patch.object(gate, "qualify_from_repo", return_value={
                            target: {"schema_version": 1, "publication_status": "candidate-only", "catalog_target": target}
                            for target in self.api.catalog["targets"]}),
                        mock.patch.object(gate, "fetch_attestation_bundle", return_value=b'{"signed":"fixture"}'),
                        mock.patch.object(gate, "_legal_closure", return_value={
                            "support_artifact_id": 501, "support_zip_sha256": "b" * 64,
                            "full_release_artifact_id": 502, "full_release_zip_sha256": "c" * 64,
                            "source_set_receipt_sha256": "d" * 64,
                            "overlay_receipt_sha256": "e" * 64,
                            "source_archive_sha256": "f" * 64})]
        for patch in self.patches:
            patch.start()
            self.addCleanup(patch.stop)

    def test_unreviewed_ios_host_tools_block_qualification_before_receipts(self):
        self.host_tool_review.stop()
        with self.assertRaisesRegex(gate.GateError, "iOS host tool closure"):
            self.qualify()
        self.assertFalse(self.destination.exists())

    def qualify(self, **overrides):
        options = dict(repo_root=ROOT, output_dir=self.destination, mode="qualify",
                       source=self.source, needs=self.needs, api=self.api)
        options.update(overrides)
        return gate.run_gate(**options)

    def test_seven_artifacts_from_exact_run_and_attempt_bind_before_receipts_written(self):
        receipts = self.qualify()
        self.assertEqual(set(receipts), set(BUILDER_JOBS))
        self.assertEqual(len(list(self.destination.iterdir())), 9)
        self.assertEqual(len(list((self.destination / "materials").iterdir())), 7)
        qualified = json.loads((self.destination / "qualified.json").read_bytes())
        self.assertEqual(set(qualified), {"schema_version", "kind", "result", "source", "targets"})
        self.assertEqual(qualified["kind"], "native-dependency-qualified-run")
        self.assertEqual(qualified["result"], "success")
        self.assertEqual(qualified["source"], self.source)
        self.assertEqual(set(qualified["targets"]), set(BUILDER_JOBS))
        for target, entry in qualified["targets"].items():
            self.assertEqual(set(entry), {"receipt", "signed_material_sha256"})
            self.assertEqual(entry["receipt"], receipts[target])
            self.assertRegex(entry["signed_material_sha256"], r"^[0-9a-f]{64}$")
            raw = (self.destination / "materials" / f"{target}.json").read_bytes()
            self.assertEqual(hashlib.sha256(raw).hexdigest(), entry["signed_material_sha256"])
            material = json.loads(raw)
            self.assertEqual(material["source"], self.source)
            self.assertEqual(material["target"], target)
            if target.startswith("adb-"):
                self.assertIn("legal", material)
            else:
                self.assertNotIn("legal", material)
            self.assertEqual(json.loads((self.destination / f"{target}.json").read_bytes()), receipts[target])
        self.assertEqual(self.qualify(mode="publish", output_dir=self.destination.parent / "publish"), receipts)
        self.assertEqual(gate.verify_full_evidence.call_args.kwargs["source"], self.source)
        self.assertEqual(len(gate.verify_full_evidence.call_args.kwargs["bundles"]), 7)
        self.assertEqual(gate.qualify_from_repo.call_args.kwargs["trusted_environment"],
                         gate.verify_full_evidence.return_value)

    def test_gate_archive_selects_eight_exact_members_and_has_stable_raw_digest(self):
        self.qualify()
        first = self.destination.parent / "gate-first.tar.gz"
        second = self.destination.parent / "gate-second.tar.gz"
        descriptor = gate.write_gate_archive(self.destination, first)
        self.assertEqual(descriptor, gate.write_gate_archive(self.destination, second))
        raw = first.read_bytes()
        self.assertEqual(raw, second.read_bytes())
        self.assertEqual(descriptor, {"sha256": hashlib.sha256(raw).hexdigest(), "size": len(raw)})
        with tarfile.open(first, "r:gz") as archive:
            names = [member.name for member in archive]
            self.assertEqual(names, ["qualified.json", *sorted(
                f"materials/{target}.json" for target in BUILDER_JOBS)])
            self.assertTrue(all(member.isfile() and member.mtime == 0 for member in archive))
        self.assertNotIn("adb-linux-arm64.json", names)
        documented, materials = publisher._gate_document(raw)
        self.assertEqual(documented, json.loads((self.destination / "qualified.json").read_bytes()))
        self.assertEqual(set(materials), set(BUILDER_JOBS))
        for target in materials:
            self.assertEqual(materials[target],
                             (self.destination / "materials" / f"{target}.json").read_bytes())

    def test_gate_archive_rejects_modified_material_and_output_collision(self):
        self.qualify()
        target = self.destination / "materials" / "ios-arm64.json"
        target.write_bytes(target.read_bytes() + b" ")
        archive = self.destination.parent / "invalid.tar.gz"
        with self.assertRaisesRegex(gate.GateError, "material"):
            gate.write_gate_archive(self.destination, archive)
        self.assertFalse(archive.exists())
        target.unlink()
        with self.assertRaises(gate.GateError):
            gate.write_gate_archive(self.destination, archive)
        self.assertFalse(archive.exists())
        (self.destination / "materials" / "ios-arm64.json").write_bytes(b"bad")
        archive.write_bytes(b"existing")
        with self.assertRaisesRegex(gate.GateError, "already exists"):
            gate.write_gate_archive(self.destination, archive)
        self.assertEqual(archive.read_bytes(), b"existing")

    def test_wrong_attempt_name_checkout_sha_or_run_identity_fails_without_output(self):
        for variant in ("attempt", "name", "head", "run"):
            with self.subTest(variant=variant):
                api = copy.deepcopy(self.api)
                if variant == "attempt":
                    api.jobs[0]["run_attempt"] = 1
                elif variant == "name":
                    api.responses["/actions/runs/45/artifacts?per_page=100&page=1"]["artifacts"][0]["name"] = "fake"
                elif variant == "head":
                    with mock.patch.object(gate, "checkout_head", return_value="b" * 40):
                        with self.assertRaises(gate.GateError):
                            self.qualify(api=api)
                    continue
                else:
                    api.responses["/actions/runs/45"]["head_sha"] = "b" * 40
                with self.assertRaises(gate.GateError):
                    self.qualify(api=api)
                self.assertFalse(self.destination.exists())

    def test_unsafe_zip_entries_and_oversized_contents_fail_closed(self):
        for files in ({"../candidate.json": b"bad", "candidate.json": b"ok", "adb-helper-linux-arm64.tar.gz": b"core"},
                      {"candidate.json": b"x" * (gate.MAX_CANDIDATE_BYTES + 1), "adb-helper-linux-arm64.tar.gz": b"core"}):
            api = copy.deepcopy(self.api)
            api.responses["/actions/artifacts/101/zip"] = zipped(files)
            with self.subTest(files=list(files)), self.assertRaises(gate.GateError):
                self.qualify(api=api)
            self.assertFalse(self.destination.exists())

    def test_absent_signature_support_id_or_support_artifact_fails_closed(self):
        with mock.patch.object(gate, "fetch_attestation_bundle", side_effect=gate.GateError("no attestation")):
            with self.assertRaisesRegex(gate.GateError, "attestation"):
                self.qualify()
        needs = copy.deepcopy(self.needs)
        needs[gate.LEGAL_JOB]["outputs"].pop("support_artifact_id")
        with self.assertRaisesRegex(gate.GateError, "support artifact ID"):
            self.qualify(needs=needs)
        api = copy.deepcopy(self.api)
        api.responses["/actions/artifacts/501"]["name"] = "wrong"
        with self.assertRaises(gate.GateError):
            self.qualify(api=api)
        self.assertFalse(self.destination.exists())

    def test_actions_pagination_and_api_failure_are_not_silent(self):
        api = copy.deepcopy(self.api)
        endpoint = "/actions/runs/45/artifacts"
        original = api.responses[endpoint + "?per_page=100&page=1"]["artifacts"]
        repeated = (original * 7)[:101]
        api.responses[endpoint + "?per_page=100&page=1"] = {
            "total_count": 101, "artifacts": repeated[:100]}
        api.responses[endpoint + "?per_page=100&page=2"] = {
            "total_count": 101, "artifacts": repeated[100:]}
        self.assertEqual(len(gate._list(api, endpoint, "artifacts")), 101)
        original_get = api.get_json
        def failed_page(path):
            if path.endswith("page=2"):
                raise gate.GateError("Actions API unavailable")
            return original_get(path)
        with mock.patch.object(api, "get_json", side_effect=failed_page):
            with self.assertRaisesRegex(gate.GateError, "API unavailable"):
                gate._list(api, endpoint, "artifacts")
        api.responses[endpoint + "?per_page=100&page=1"] = {"total_count": 101, "artifacts": []}
        with self.assertRaisesRegex(gate.GateError, "paginated"):
            gate._list(api, endpoint, "artifacts")

    def test_attestation_bundle_api_is_required_and_source_url_is_restricted(self):
        full_tar = b"exact signed tar"
        digest = hashlib.sha256(full_tar).hexdigest()
        endpoint = "/attestations/sha256:" + digest + "?predicate_type=provenance&per_page=100"
        api = copy.deepcopy(self.api)
        api.responses[endpoint] = {"attestations": [{"bundle": {"mediaType": "application/vnd.dev.sigstore.bundle.v0.3+json"}}]}
        self.assertIn(b"mediaType", self.real_fetch(api, full_tar, self.source))
        api.responses[endpoint] = {"attestations": []}
        with self.assertRaisesRegex(gate.GateError, "attestation"):
            self.real_fetch(api, full_tar, self.source)
        with mock.patch.object(api, "get_json", side_effect=gate.GateError("HTTP 403")):
            with self.assertRaisesRegex(gate.GateError, "attestations:read may be required"):
                self.real_fetch(api, full_tar, self.source)
        api.responses[endpoint] = {"attestations": [{"bundle_url": "https://evil.example/repos/ZEACENT/klogg/attestations/1"}]}
        with self.assertRaisesRegex(gate.GateError, "untrusted"):
            self.real_fetch(api, full_tar, self.source)
        api.responses[endpoint] = {"attestations": [{"bundle_url": "https://api.github.com/repos/ZEACENT/klogg/attestations/123"}]}
        api.responses["/attestations/123"] = {"bundle": {"verified": "signed bytes"}}
        self.assertIn(b"verified", self.real_fetch(api, full_tar, self.source))

    def test_legal_artifact_ids_and_full_release_closure_are_mandatory(self):
        for missing in ("support_artifact_id", "full_release_artifact_id"):
            needs = copy.deepcopy(self.needs)
            needs[gate.LEGAL_JOB]["outputs"].pop(missing)
            with self.subTest(missing=missing), self.assertRaisesRegex(gate.GateError, "artifact ID"):
                self.qualify(needs=needs)
            self.assertFalse(self.destination.exists())
        api = copy.deepcopy(self.api)
        api.jobs[-1]["labels"] = ["self-hosted"]
        with self.assertRaisesRegex(gate.GateError, "legal assets job is untrusted"):
            self.qualify(api=api)
        with mock.patch.object(gate, "_legal_closure", side_effect=gate.GateError("source archive missing")):
            with self.assertRaisesRegex(gate.GateError, "source archive missing"):
                self.qualify()
        self.assertFalse(self.destination.exists())

    def test_real_legal_zip_requires_corresponding_source_and_exact_support_subset(self):
        lock = json.loads((ROOT / "packaging/adb/adb-helper.lock.json").read_bytes())
        assets = lock["release_assets"]
        release = {asset["file_name"]: b"source closure fixture" for asset in assets}
        release.update({asset["sha256_file"]: b"hash sidecar fixture" for asset in assets})
        support = {name: data for name, data in release.items() if name in {
            key for asset in assets if asset["distribution"]["package_required"]
            for key in (asset["file_name"], asset["sha256_file"])}}
        api = copy.deepcopy(self.api)
        without_source = dict(release)
        without_source.pop("adb-helper-source-archive.tar.gz")
        api.responses["/actions/artifacts/502/zip"] = zipped(without_source)
        api.responses["/actions/artifacts/501/zip"] = zipped(support)
        # Unpatch the success-path seam locally to exercise the actual ZIP verifier.
        with mock.patch.object(gate, "_legal_closure", side_effect=self.real_legal):
            with self.assertRaisesRegex(gate.GateError, "full legal source closure"):
                self.qualify(api=api)
            api.responses["/actions/artifacts/502/zip"] = zipped(release)
            support["adb-helper-source-set-receipt.json"] = b"wrong source receipt"
            api.responses["/actions/artifacts/501/zip"] = zipped(support)
            with self.assertRaisesRegex(gate.GateError, "package support differs"):
                self.qualify(api=api)
        self.assertFalse(self.destination.exists())

    def test_real_legal_projection_accepts_exact_source_archive_support_and_sidecars(self):
        lock = json.loads((ROOT / "packaging/adb/adb-helper.lock.json").read_bytes())
        assets = lock["release_assets"]
        data = b"independently supplied legal source bytes"
        source_hash = hashlib.sha256(data).hexdigest()
        release = {asset["file_name"]: data for asset in assets}
        release.update({asset["sha256_file"]: (hashlib.sha256(data).hexdigest() + "  "
                        + asset["file_name"] + "\n").encode() for asset in assets})
        release["adb-helper-release-assets.json"] = json.dumps([
            {"kind": asset["kind"], "path": asset["file_name"], "sha256": source_hash}
            for asset in assets]).encode()
        support = {name: value for name, value in release.items() if name in {
            key for asset in assets if asset["distribution"]["package_required"]
            for key in (asset["file_name"], asset["sha256_file"])}}
        api = copy.deepcopy(self.api)
        api.responses["/actions/artifacts/501/zip"] = zipped(support)
        api.responses["/actions/artifacts/502/zip"] = zipped(release)
        candidate = {"qualification": {"legacy_package_receipt_sha256": hashlib.sha256(b"legacy").hexdigest(),
                                       "rechecked_package_receipt_sha256": hashlib.sha256(b"rechecked").hexdigest()}}
        downloads = {"adb-linux-arm64": {"candidate": json.dumps(candidate).encode()}}

        def extract(raw, work):
            (work / "receipt.json").write_text(json.dumps({"source_set_receipt_sha256": source_hash}))
            (work / "package-verification.json").write_bytes(b"legacy")
            (work / "package-smoke.json").write_bytes(b"smoke")

        def verify(command, **kwargs):
            pathlib.Path(command[command.index("--package-verification-receipt") + 1]).write_bytes(b"rechecked")
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(gate, "_unpack_full", side_effect=extract), \
             mock.patch.object(gate, "validate_source_set_receipt", return_value=source_hash), \
             mock.patch.object(gate, "validate_overlay_receipt"), \
             mock.patch.object(gate.subprocess, "run", side_effect=verify):
            legal = self.real_legal(api, {"support": "501", "release": "502"}, ROOT,
                                    {"adb-linux-arm64": b"full signed tar"}, downloads)
        self.assertEqual(legal["source_set_receipt_sha256"], source_hash)
        self.assertEqual(legal["support_artifact_id"], 501)

    def test_mismatched_core_or_full_artifact_fails_before_output(self):
        api = copy.deepcopy(self.api)
        api.responses["/actions/artifacts/101/zip"] = zipped({"candidate.json": b"candidate", "adb-helper-linux-arm64.tar.gz": b"wrong"})
        with mock.patch.object(gate, "qualify_from_repo", side_effect=gate.GateError("core differs")):
            with self.assertRaises(gate.GateError):
                self.qualify(api=api)
        with mock.patch.object(gate, "verify_full_evidence", side_effect=gate.GateError("full differs")):
            with self.assertRaises(gate.GateError):
                self.qualify()
        self.assertFalse(self.destination.exists())


if __name__ == "__main__":
    unittest.main()
