"""Full native provenance must come from the parent run, not candidate receipts."""

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
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_dependency_full_evidence as evidence
from ci_dependency_core import ADB_RUNTIME, package_core
from ci_dependency_pipeline import BUILDER_JOBS, compute_identities


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def make_tar(files, *, link=None):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w:gz") as archive:
        for name, (data, mode) in files.items():
            item = tarfile.TarInfo("./" + name)
            item.mode, item.size = mode, len(data)
            archive.addfile(item, io.BytesIO(data))
        if link:
            name, target = link
            item = tarfile.TarInfo("./" + name)
            item.type, item.mode, item.linkname = tarfile.SYMTYPE, 0o777, target
            archive.addfile(item)
    return stream.getvalue()


class FullEvidenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = json.loads((ROOT / "ci/dependencies/catalog.json").read_bytes())
        cls.identities = compute_identities(ROOT, cls.catalog)
        cls.source = {"repository": "ZEACENT/klogg", "workflow": ".github/workflows/ci-build.yml",
                      "event_name": "workflow_dispatch", "sha": "a" * 40,
                      "ref": "refs/heads/master", "run_id": 45, "run_attempt": 2}
        cls.workflow = (ROOT / ".github/workflows/ci-build.yml").read_text()
        cls.run_metadata = {"id": 45, "run_attempt": 2, "head_sha": "a" * 40,
                   "head_branch": "master", "event": "workflow_dispatch", "path": ".github/workflows/ci-build.yml",
                   "repository": {"full_name": "ZEACENT/klogg"}, "status": "in_progress", "conclusion": None}
        cls.jobs, cls.needs, cls.artifacts, cls.downloads, cls.cores, cls.bundles = [], {}, [], {}, {}, {}
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for index, (target_id, row) in enumerate(sorted(cls.catalog["targets"].items()), 1):
                stage = root / target_id
                if row["component"] == "adb-helper":
                    helper = "adb.exe" if row["target"].startswith("windows-") else "adb"
                    names = [helper, *ADB_RUNTIME[row["target"]]]
                    files = {"helpers/" + name: ((target_id + name).encode(), 0o755 if name == helper else 0o644)
                             for name in names}
                    receipt_name = "receipt.json"
                    receipt = {"receipt_kind": "binary-build", "target": row["target"]}
                else:
                    files = {"lib/libfixture.1.dylib": (target_id.encode(), 0o755)}
                    receipt_name = "ios-native-build-receipt.json"
                    lock_path = ROOT / "3rdparty/libimobiledevice/libimobiledevice.lock.json"
                    lock = json.loads(lock_path.read_bytes())
                    receipt = {"schema_version": 1, "receipt_kind": "ios-native-build",
                               "architecture": row["target"], "toolchain": row["toolchain"],
                               "native_qualified": True, "qualification": "native",
                               "lock_sha256": sha(lock_path.read_bytes()),
                               "deployment_target": lock["artifact_contract"]["thin_artifacts"][row["target"]]["deployment_target"]}
                for name, (data, mode) in files.items():
                    path = stage / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                    path.chmod(mode)
                if row["component"] == "ios-native":
                    alias = stage / "lib/libfixture.dylib"
                    alias.symlink_to("libfixture.1.dylib")
                    link = ("lib/libfixture.dylib", "libfixture.1.dylib")
                else:
                    link = None
                core = root / row["archive_name"]
                package_core(stage, core, component=row["component"], target=row["target"],
                             core_identity=cls.identities[target_id]["core_identity"])
                cls.cores[target_id] = core.read_bytes()
                receipt_data = json.dumps(receipt).encode()
                files[receipt_name] = (receipt_data, 0o644)
                if row["component"] == "adb-helper":
                    for name in ("smoke.json", "package-smoke.json", "package-verification.json"):
                        files[name] = (b"{}", 0o644)
                checksums = "".join(f"{sha(data)}  {name}\n" for name, (data, _) in sorted(files.items()))
                files["SHA256SUMS"] = (checksums.encode(), 0o644)
                cls.downloads[index + 100] = make_tar(files, link=link)
                cls.bundles[target_id] = b'{"bundle":"placeholder"}'
                artifact_name = f"adb-helper-{row['target']}" if row["component"] == "adb-helper" else f"ios-native-{row['target']}"
                cls.artifacts.append({"id": index + 100, "name": artifact_name,
                                      "expired": False, "workflow_run": {"id": 45, "head_sha": "a" * 40,
                                                                       "head_branch": "master"}})
                steps = [{"name": "Attest exact full native tar provenance", "status": "completed", "conclusion": "success"}]
                if row["component"] == "ios-native":
                    steps.extend({"name": name, "status": "completed", "conclusion": "success"}
                                 for name in (evidence.STEP_IOS_PREFLIGHT, evidence.STEP_IOS_HOST_PROBE))
                job_name = (f"Source-built ADB helper {row['target']}" if row["component"] == "adb-helper"
                            else f"iOS native stack {row['target']}")
                cls.jobs.append({"id": index + 200, "run_id": 45, "run_attempt": 2,
                                 "head_sha": "a" * 40, "name": job_name,
                                 "status": "completed", "conclusion": "success",
                                 "labels": [row["runner"]], "runner_name": "GitHub Actions 1", "steps": steps})
                cls.needs[BUILDER_JOBS[target_id]] = {"result": "success", "outputs": {
                    "full_artifact_id": str(index + 100)}}

    def inputs(self):
        return dict(repo_root=ROOT, source=copy.deepcopy(self.source), run=copy.deepcopy(self.run_metadata),
                    attempt_jobs=copy.deepcopy(self.jobs), needs=copy.deepcopy(self.needs),
                    artifacts=copy.deepcopy(self.artifacts), full_downloads=copy.deepcopy(self.downloads),
                    candidate_archives=copy.deepcopy(self.cores), bundles=copy.deepcopy(self.bundles),
                    catalog=copy.deepcopy(self.catalog), workflow_text=self.workflow)

    def run_gate(self, values=None, **overrides):
        values = values or self.inputs()
        values.update(overrides)

        def verifier(command, **kwargs):
            # The full verifier is intentionally injected; these synthetic fixtures
            # contain no executable ADB, Mach-O, or full source/legal artifacts.
            if "--package-verification-receipt" in command:
                output = pathlib.Path(command[command.index("--package-verification-receipt") + 1])
                output.write_bytes(b"{}")
            return subprocess.CompletedProcess(command, 0, stdout="ok", stderr="")

        def attestor(command, **kwargs):
            path = pathlib.Path(command[3])
            statement = {"predicateType": "https://slsa.dev/provenance/v1",
                         "subject": [{"name": path.name, "digest": {"sha256": sha(path.read_bytes())}}]}
            return subprocess.CompletedProcess(command, 0, stdout=json.dumps([
                {"verificationResult": {"statement": statement}}]), stderr="")

        with mock.patch.object(evidence, "verify_artifact_envelope"):
            return evidence.verify_full_evidence(**values, run_verifier=verifier,
                                                 attestation_runner=attestor)

    def test_seven_signed_full_archives_compare_every_core_member_and_yield_scheduler_evidence(self):
        trusted = self.run_gate()
        self.assertEqual(set(trusted), set(self.cores))
        for key, value in trusted.items():
            self.assertEqual(value["runner"], self.catalog["targets"][key]["runner"])
            if key.startswith("ios-"):
                self.assertEqual(value["toolchain"], self.catalog["targets"][key]["toolchain"])
            self.assertNotIn("hardware_attestation", value)

    def test_missing_signature_or_full_artifact_id_fails_closed(self):
        for field in ("bundle", "empty-bundle", "full_artifact_id"):
            values = self.inputs()
            if field == "bundle":
                values["bundles"].pop("ios-arm64")
            elif field == "empty-bundle":
                values["bundles"]["ios-arm64"] = b""
            else:
                values["needs"]["BuildIosNativeArm64"]["outputs"].pop(field)
            with self.subTest(field=field), self.assertRaises(evidence.FullEvidenceError):
                self.run_gate(values)

    def test_artifact_substitution_and_wrong_job_attempt_ref_fail_closed(self):
        for variant in ("artifact-id", "artifact-name", "artifact-run", "tar-bytes", "job",
                        "attempt", "job-result", "step", "ref", "event", "repo", "needs"):
            values = self.inputs()
            if variant == "artifact-id":
                values["needs"]["BuildIosNativeArm64"]["outputs"]["full_artifact_id"] = "999"
            elif variant == "artifact-name":
                values["artifacts"][0]["name"] = "unreviewed"
            elif variant == "artifact-run":
                values["artifacts"][0]["workflow_run"]["id"] = 99
            elif variant == "tar-bytes":
                values["full_downloads"][101] = values["full_downloads"][102]
            elif variant == "job":
                values["attempt_jobs"][0]["name"] = "unreviewed"
            elif variant == "attempt":
                values["attempt_jobs"][0]["run_attempt"] = 1
            elif variant == "job-result":
                values["attempt_jobs"][0]["conclusion"] = "failure"
            elif variant == "step":
                values["attempt_jobs"][0]["steps"].pop()
            elif variant == "ref":
                values["run"]["head_branch"] = "feature"
            elif variant == "event":
                values["run"]["event"] = "push"
            elif variant == "repo":
                values["run"]["repository"]["full_name"] = "fake/klogg"
            else:
                values["needs"]["BuildIosNativeArm64"]["result"] = "failure"
            with self.subTest(variant=variant), self.assertRaises(evidence.FullEvidenceError):
                self.run_gate(values)

    def test_runner_mapping_and_toolchain_report_fail_closed(self):
        values = self.inputs()
        values["workflow_text"] = values["workflow_text"].replace(
            "  BuildIosNativeArm64:\n    needs:",
            "  BuildIosNativeArm64:\n    runs-on: self-hosted\n    needs:", 1)
        with self.assertRaises(evidence.FullEvidenceError):
            self.run_gate(values)
        values = self.inputs()
        values["attempt_jobs"][-1]["labels"] = ["self-hosted"]
        with self.assertRaises(evidence.FullEvidenceError):
            self.run_gate(values)
        values = self.inputs()
        ios_job = next(job for job in values["attempt_jobs"] if job["name"] == "iOS native stack arm64")
        next(step for step in ios_job["steps"] if step["name"] == evidence.STEP_IOS_PREFLIGHT)["conclusion"] = "failure"
        with self.assertRaises(evidence.FullEvidenceError):
            self.run_gate(values)
        values = self.inputs()
        ios_job = next(job for job in values["attempt_jobs"] if job["name"] == "iOS native stack arm64")
        next(step for step in ios_job["steps"] if step["name"] == evidence.STEP_IOS_HOST_PROBE)["conclusion"] = "skipped"
        with self.assertRaises(evidence.FullEvidenceError):
            self.run_gate(values)
        values = self.inputs()
        target = "ios-arm64"
        artifact = next(item for item in values["artifacts"] if item["name"] == "ios-native-arm64")
        raw = values["full_downloads"][artifact["id"]]
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:gz") as archive:
            files = {item.name[2:]: (archive.extractfile(item).read(), item.mode)
                     for item in archive if item.isfile()}
        receipt = json.loads(files["ios-native-build-receipt.json"][0]); receipt["toolchain"]["ninja"] = "unreviewed"
        files["ios-native-build-receipt.json"] = (json.dumps(receipt).encode(), 0o644)
        files["SHA256SUMS"] = ("".join(f"{sha(data)}  {name}\n" for name, (data, _) in sorted(files.items())
                                  if name != "SHA256SUMS").encode(), 0o644)
        values["full_downloads"][artifact["id"]] = make_tar(files, link=("lib/libfixture.dylib", "libfixture.1.dylib"))
        with self.assertRaises(evidence.FullEvidenceError):
            self.run_gate(values)

    def test_windows_ntfs_modes_match_canonical_core_modes_without_relaxing_bytes(self):
        for runtime_mode in (0o666, 0o755, 0o777):
            values = self.inputs()
            artifact = next(item for item in values["artifacts"] if item["name"] == "adb-helper-windows-x86_64")
            with tarfile.open(fileobj=io.BytesIO(values["full_downloads"][artifact["id"]]), mode="r:gz") as archive:
                files = {item.name[2:]: (archive.extractfile(item).read(), item.mode)
                         for item in archive if item.isfile()}
            files["helpers/adb.exe"] = (files["helpers/adb.exe"][0], 0o777)
            for name in files:
                if name.startswith("helpers/") and name != "helpers/adb.exe":
                    files[name] = (files[name][0], runtime_mode)
            values["full_downloads"][artifact["id"]] = make_tar(files)
            with self.subTest(runtime_mode=oct(runtime_mode)):
                self.assertEqual(set(self.run_gate(values)), set(self.cores))

    def test_commented_out_pinned_preflight_cannot_spoof_reviewed_workflow(self):
        values = self.inputs()
        values["workflow_text"] = values["workflow_text"].replace(
            "          python3 scripts/ci_dependency_toolchain.py \\\n",
            "          # python3 scripts/ci_dependency_toolchain.py \\\n", 1)
        with self.assertRaises(evidence.FullEvidenceError):
            self.run_gate(values)

    def test_commented_out_host_probe_cannot_spoof_successful_step(self):
        values = self.inputs()
        marker = "      - name: Observe unreviewed iOS host tool inputs\n"
        before, separator, after = values["workflow_text"].partition(marker)
        self.assertTrue(separator)
        self.assertIn("          python3 scripts/ci_dependency_toolchain.py \\\n", after)
        after = after.replace("          python3 scripts/ci_dependency_toolchain.py \\\n",
                              "          # python3 scripts/ci_dependency_toolchain.py \\\n", 1)
        values["workflow_text"] = before + separator + after
        with self.assertRaises(evidence.FullEvidenceError):
            self.run_gate(values)

    def test_wrong_ios_symlink_and_binary_substitution_fail_even_with_valid_new_signature(self):
        for variant in ("link", "binary", "mode"):
            values = self.inputs()
            artifact = next(item for item in values["artifacts"] if item["name"] == "ios-native-arm64")
            with tarfile.open(fileobj=io.BytesIO(values["full_downloads"][artifact["id"]]), mode="r:gz") as archive:
                files = {item.name[2:]: (archive.extractfile(item).read(), item.mode)
                         for item in archive if item.isfile()}
            if variant == "binary":
                files["lib/libfixture.1.dylib"] = (b"evil", 0o755)
                files["SHA256SUMS"] = ("".join(f"{sha(data)}  {name}\n" for name, (data, _) in sorted(files.items())
                                          if name != "SHA256SUMS").encode(), 0o644)
            elif variant == "mode":
                files["lib/libfixture.1.dylib"] = (files["lib/libfixture.1.dylib"][0], 0o644)
            values["full_downloads"][artifact["id"]] = make_tar(files, link=(
                "lib/libfixture.dylib", "libwrong.1.dylib" if variant == "link" else "libfixture.1.dylib"))
            with self.subTest(variant=variant), self.assertRaises(evidence.FullEvidenceError):
                self.run_gate(values)

    def test_signed_statement_must_bind_exact_full_tar_name_and_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            subject = pathlib.Path(temporary) / "ios-native-arm64.tar.gz"
            subject.write_bytes(b"full tar")
            for name, digest in (("SHA256SUMS", sha(subject.read_bytes())),
                                 (subject.name, "b" * 64)):
                def wrong_subject(command, **kwargs):
                    self.assertEqual(command[0:3], ["gh", "attestation", "verify"])
                    self.assertIn("--deny-self-hosted-runners", command)
                    self.assertEqual(command[command.index("--signer-digest") + 1], self.source["sha"])
                    self.assertEqual(command[command.index("--source-ref") + 1], self.source["ref"])
                    return subprocess.CompletedProcess(command, 0, stdout=json.dumps([{
                        "verificationResult": {"statement": {"predicateType": "https://slsa.dev/provenance/v1",
                            "subject": [{"name": name, "digest": {"sha256": digest}}]}}}]))
                with self.subTest(name=name, digest=digest), self.assertRaises(evidence.FullEvidenceError):
                    evidence._verified_full_tar(subject, b"signed-bundle", self.source, wrong_subject)

    def test_archive_rejects_traversal_and_hardlinks_before_extraction(self):
        for member_name, kind in (("./../escape", "file"), ("./lib/libalias.dylib", "hardlink")):
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode="w:gz") as archive:
                item = tarfile.TarInfo(member_name)
                if kind == "hardlink":
                    item.type, item.linkname = tarfile.LNKTYPE, "lib/libfixture.1.dylib"
                else:
                    item.size = 1
                archive.addfile(item, io.BytesIO(b"X") if kind == "file" else None)
            with tempfile.TemporaryDirectory() as temporary:
                with self.subTest(member_name=member_name), self.assertRaises(evidence.FullEvidenceError):
                    evidence._unpack_full(stream.getvalue(), pathlib.Path(temporary))
                self.assertFalse((pathlib.Path(temporary) / "lib").exists())


if __name__ == "__main__":
    unittest.main()
