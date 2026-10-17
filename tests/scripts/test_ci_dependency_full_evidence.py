"""Full native provenance must come from the parent run, not candidate receipts."""

import contextlib
import copy
import hashlib
import io
import json
import os
import pathlib
import shutil
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
import test_ios_native_release_contract as ios_contract


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def make_tar(files, *, link=None, directories=None):
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode="w:gz") as archive:
        for name, mode in (directories or {}).items():
            item = tarfile.TarInfo("./" + name if name else ".")
            item.type, item.mode = tarfile.DIRTYPE, mode
            archive.addfile(item)
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
                if row["component"] == "ios-native":
                    files["include/fixture/original.h"] = (b"authenticated header sentinel\n", 0o644)
                    files["share/fixture/support.dat"] = (b"authenticated support sentinel\x00", 0o644)
                    for name in ("ios-native-source-set-receipt.json", "ios-native-legal-receipt.json",
                                 "ios-native-sbom.spdx.json", "corresponding-source/fixture.tar.gz"):
                        files[name] = (("original full artifact: " + name).encode(), 0o644)
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

        with mock.patch.object(evidence, "verify_artifact_envelope"):
            return evidence.verify_full_evidence(**values, run_verifier=self.mocked_metadata_runner,
                                                 attestation_runner=self.mocked_signature_runner)

    @staticmethod
    def mocked_metadata_runner(command, **kwargs):
        # Test fixtures contain no executable ADB, Mach-O, or full source/legal artifacts.
        return subprocess.CompletedProcess(command, 0, stdout="test-only", stderr="")

    @staticmethod
    def mocked_signature_runner(command, **kwargs):
        # Synthetic test seam only: no production signature verification occurs.
        subject = pathlib.Path(command[3])
        statement = {"predicateType": "https://slsa.dev/provenance/v1",
                     "subject": [{"name": subject.name, "digest": {"sha256": sha(subject.read_bytes())}}]}
        return subprocess.CompletedProcess(command, 0, stdout=json.dumps([
            {"verificationResult": {"statement": statement}}]), stderr="")

    @contextlib.contextmanager
    def projection(self, values=None, *, attestation_runner=None, run_verifier=None):
        # Native metadata is also mocked: fixtures contain no native executables.
        with mock.patch.object(evidence, "verify_artifact_envelope"):
            with evidence.project_authenticated_full_artifacts(
                    **(values or self.inputs()),
                    attestation_runner=attestation_runner or self.mocked_signature_runner,
                    run_verifier=run_verifier or self.mocked_metadata_runner) as result:
                yield result

    def full_files(self, values, target="ios-arm64"):
        name = evidence._full_name(values["catalog"]["targets"][target])
        artifact = next(item for item in values["artifacts"] if item["name"] == name)
        with tarfile.open(fileobj=io.BytesIO(values["full_downloads"][artifact["id"]]), mode="r:gz") as archive:
            files = {item.name[2:]: (archive.extractfile(item).read(), item.mode)
                     for item in archive if item.isfile()}
        return artifact["id"], files

    def ios_assembly_inputs(self, root):
        values = self.inputs()
        originals = {}
        fixture = ios_contract.IosNativeReleaseContractTest()
        for target in ("ios-arm64", "ios-x86_64"):
            row = values["catalog"]["targets"][target]
            stack = root / target / "stack"
            stack.mkdir(parents=True)
            receipt = {"schema_version": 1, "receipt_kind": "ios-native-build",
                       "lock_sha256": ios_contract.LEGAL_MODULE.sha256(ios_contract.LOCK),
                       "architecture": row["target"], "toolchain": row["toolchain"],
                       "deployment_target": ios_contract.EXPECTED_THIN_ARTIFACTS[row["target"]],
                       "native_qualified": True, "qualification": "native",
                       "dylibs": [{"name": "libfixture.dylib",
                                   "sha256": sha(f"native binary {row['target']}".encode())}]}
            (stack / "ios-native-build-receipt.json").write_text(json.dumps(receipt) + "\n")
            stack, legal = fixture.generate_legal_fixture(
                stack.parent, "26.10.15", "https://example.org/new", row["target"])
            (stack / "include").mkdir()
            (stack / "include/empty").mkdir(mode=0o750)
            (stack / "include/original.h").write_bytes(b"authenticated header\n")
            shutil.copy2(stack / "ios-native-build-receipt.json",
                         stack / f"ios-native-{row['target']}-build-receipt.json")
            (stack / "lib/libfixture.dylib").chmod(0o755)
            (stack / "lib/libfixture-alias.dylib").symlink_to("libfixture.dylib")
            core = root / (target + ".tar.gz")
            binary = root / (target + "-binary")
            shutil.copytree(stack / "lib", binary / "lib", symlinks=True)
            package_core(binary, core, component="ios-native", target=row["target"],
                         core_identity=self.identities[target]["core_identity"])
            values["candidate_archives"][target] = core.read_bytes()
            files = {path.relative_to(base).as_posix(): (path.read_bytes(), path.stat().st_mode & 0o777)
                     for base in (stack, legal) for path in base.rglob("*")
                     if path.is_file() and not path.is_symlink()}
            files["SHA256SUMS"] = (("".join(f"{sha(data)}  {name}\n"
                                            for name, (data, _) in sorted(files.items()))).encode(), 0o644)
            artifact = next(item for item in values["artifacts"]
                            if item["name"] == evidence._full_name(row))
            values["full_downloads"][artifact["id"]] = make_tar(
                files, link=("lib/libfixture-alias.dylib", "libfixture.dylib"),
                directories={"": 0o750, "include": 0o755, "include/empty": 0o750, "lib": 0o755})
            originals[target] = (stack, legal)
        return values, originals

    @unittest.skipIf(os.name == "nt", "requires POSIX original stack assembly")
    def test_authenticated_full_roots_assemble_ios_only_after_all_seven_pass(self):
        for version in ("26.10.15", "26.10.16"):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                values, originals = self.ios_assembly_inputs(root)
                outputs = {target: root / (target + "-assembled") for target in originals}
                verified = []

                def attestor(command, **kwargs):
                    self.assertTrue(all(not path.exists() for path in outputs.values()))
                    verified.append(pathlib.Path(command[3]).name)
                    return self.mocked_signature_runner(command, **kwargs)

                with mock.patch.object(evidence, "verify_artifact_envelope"):
                    trusted = evidence.assemble_authenticated_ios_stacks(
                        **values, outputs=outputs, version=version, base_url="https://example.org/new",
                        attestation_runner=attestor, run_verifier=self.mocked_metadata_runner)
                self.assertEqual(len(verified), 7)
                self.assertEqual(trusted, self.run_gate(values))
                fixture = ios_contract.IosNativeReleaseContractTest()
                for target, (stack, legal) in originals.items():
                    expected = root / (target + "-expected")
                    shutil.copytree(legal, expected)
                    ios_contract.LEGAL_MODULE.write_overlay(
                        expected, json.loads((expected / "ios-native-source-set-receipt.json").read_bytes()),
                        version, "https://example.org/new")
                    self.assertEqual(fixture.tree_bytes(outputs[target]),
                                     {**fixture.tree_bytes(stack), **fixture.tree_bytes(expected)})
                    self.assertEqual(os.readlink(outputs[target] / "lib/libfixture-alias.dylib"),
                                     "libfixture.dylib")
                    self.assertEqual((outputs[target] / "include/original.h").read_bytes(),
                                     b"authenticated header\n")
                    self.assertEqual(outputs[target].stat().st_mode & 0o777, 0o750)
                    self.assertEqual((outputs[target] / "include/empty").stat().st_mode & 0o777, 0o750)

    @unittest.skipIf(os.name == "nt", "requires POSIX original stack assembly")
    def test_failed_seventh_signature_never_creates_ios_assembly_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            values, originals = self.ios_assembly_inputs(root)
            outputs = {target: root / (target + "-assembled") for target in originals}
            seen = []

            def attestor(command, **kwargs):
                seen.append(command)
                if len(seen) == 7:
                    return subprocess.CompletedProcess(command, 1, stdout="", stderr="fixture rejection")
                return self.mocked_signature_runner(command, **kwargs)

            with mock.patch.object(evidence, "verify_artifact_envelope"):
                with self.assertRaises(evidence.FullEvidenceError):
                    evidence.assemble_authenticated_ios_stacks(
                        **values, outputs=outputs, version="26.10.16", base_url="https://example.org/new",
                        attestation_runner=attestor, run_verifier=self.mocked_metadata_runner)
            self.assertEqual(len(seen), 7)
            self.assertTrue(all(not path.exists() for path in outputs.values()))

    @unittest.skipIf(os.name == "nt", "requires POSIX original stack assembly")
    def test_authenticated_assembly_preflights_both_outputs_before_writing_either(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            values, originals = self.ios_assembly_inputs(root)
            outputs = {target: root / (target + "-assembled") for target in originals}
            outputs["ios-x86_64"].mkdir()
            witness = outputs["ios-x86_64"] / "foreign"
            witness.write_bytes(b"must remain unchanged")
            with mock.patch.object(evidence, "verify_artifact_envelope"):
                with self.assertRaisesRegex(RuntimeError, "fresh empty directory"):
                    evidence.assemble_authenticated_ios_stacks(
                        **values, outputs=outputs, version="26.10.16", base_url="https://example.org/new",
                        attestation_runner=self.mocked_signature_runner,
                        run_verifier=self.mocked_metadata_runner)
            self.assertFalse(outputs["ios-arm64"].exists())
            self.assertEqual(witness.read_bytes(), b"must remain unchanged")

    def test_authenticated_assembly_rejects_missing_duplicate_and_nested_outputs_before_authentication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for outputs in ({"ios-arm64": root / "one"},
                            {"ios-arm64": root / "one", "ios-x86_64": root / "one"},
                            {"ios-arm64": root / "one", "ios-x86_64": root / "one/child"}):
                with self.subTest(outputs=outputs), mock.patch.object(
                        evidence, "project_authenticated_full_artifacts") as projection:
                    with self.assertRaises(evidence.FullEvidenceError):
                        evidence.assemble_authenticated_ios_stacks(
                            **self.inputs(), outputs=outputs, version="26.10.16",
                            base_url="https://example.org/new")
                    projection.assert_not_called()
            self.assertEqual(list(root.iterdir()), [])

    @unittest.skipIf(os.name == "nt", "requires POSIX directory mode semantics")
    def test_full_tar_preserves_authenticated_directory_modes_under_private_umask(self):
        raw = make_tar({"include/api.h": (b"header\n", 0o640)},
                       directories={"": 0o750, "include": 0o755, "include/empty": 0o750})
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "full"
            root.mkdir(mode=0o700)
            previous = os.umask(0o077)
            try:
                evidence._unpack_full(raw, root)
            finally:
                os.umask(previous)
            self.assertEqual(root.stat().st_mode & 0o777, 0o750)
            self.assertEqual((root / "include").stat().st_mode & 0o777, 0o755)
            self.assertEqual((root / "include/empty").stat().st_mode & 0o777, 0o750)
            self.assertEqual((root / "include/api.h").stat().st_mode & 0o777, 0o640)

    def test_full_tar_rejects_ambiguous_root_metadata_and_special_directory_permissions(self):
        for directories in ({"": 0o755, ".": 0o750}, {"": 0o4755}, {"include": 0o2755}):
            with self.subTest(directories=directories), tempfile.TemporaryDirectory() as temporary:
                raw = make_tar({"include/api.h": (b"header\n", 0o644)}, directories=directories)
                with self.assertRaises(evidence.FullEvidenceError):
                    evidence._unpack_full(raw, pathlib.Path(temporary))

    @unittest.skipIf(os.name == "nt", "requires POSIX directory mode semantics")
    def test_full_tar_rejects_unreclaimable_directory_modes_before_writing(self):
        for name, mode in (("", 0o555), ("", 0o000), ("include", 0o555)):
            with self.subTest(name=name, mode=mode), tempfile.TemporaryDirectory() as temporary:
                root = pathlib.Path(temporary)
                directory = root / name
                if name:
                    directory.mkdir(mode=0o700)
                raw = make_tar({"include/api.h": (b"header\n", 0o644)}, directories={name: mode})
                handle = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                try:
                    with self.assertRaisesRegex(evidence.FullEvidenceError, "directory permissions"):
                        evidence._unpack_full(raw, root)
                    self.assertFalse((root / "include/api.h").exists())
                finally:
                    # Only the descriptor of this test's own precreated directory
                    # is repaired, even when RED made it inaccessible by pathname.
                    os.fchmod(handle, 0o700)
                    os.close(handle)

    def test_projection_rejects_unreclaimable_directory_metadata_without_masking_or_retaining_base(self):
        values = self.inputs()
        identifier, files = self.full_files(values)
        values["full_downloads"][identifier] = make_tar(
            files, link=("lib/libfixture.dylib", "libfixture.1.dylib"),
            directories={"": 0o555})
        bases = []
        original = tempfile.mkdtemp

        def allocate(*args, **kwargs):
            base = original(*args, **kwargs)
            bases.append(pathlib.Path(base))
            return base

        with mock.patch.object(evidence.tempfile, "mkdtemp", side_effect=allocate):
            with self.assertRaises(evidence.FullEvidenceError) as error:
                with self.projection(values):
                    self.fail("unsupported read-only staging root was exposed")
        self.assertIn("directory permissions", str(error.exception.__cause__))
        self.assertEqual(len(bases), 1)
        self.assertFalse(bases[0].exists())

    @unittest.skipIf(os.name == "nt", "requires POSIX directory mode semantics")
    def test_projection_rechecks_metadata_after_external_verifiers(self):
        def change_mode(command, **kwargs):
            root = pathlib.Path(command[command.index("--stack-root") + 1])
            path = root / "include"
            path.chmod((path.stat().st_mode & 0o777) ^ 0o020)
            return self.mocked_metadata_runner(command, **kwargs)

        with self.assertRaises(evidence.FullEvidenceError) as error:
            with self.projection(run_verifier=change_mode):
                self.fail("changed authenticated directory metadata was exposed")
        self.assertIn("metadata changed", str(error.exception.__cause__))

    def test_fresh_workspace_temporary_parent_preserves_projection_and_existing_verification(self):
        values = self.inputs()
        original_inputs = copy.deepcopy(values)
        created = []
        mkdtemp = tempfile.mkdtemp
        with tempfile.TemporaryDirectory(prefix="full-evidence-workspace-", dir=ROOT) as temporary:
            parent = pathlib.Path(temporary)

            def workspace_temporary(*args, **kwargs):
                kwargs["dir"] = str(parent)
                base = pathlib.Path(mkdtemp(*args, **kwargs))
                created.append(base)
                return str(base)

            with mock.patch.object(evidence.tempfile, "mkdtemp", side_effect=workspace_temporary):
                with self.projection(values) as (trusted, roots):
                    for root in roots.values():
                        base = root.parent.parent
                        self.assertEqual(base.parent.resolve(), parent.resolve())
                        self.assertNotIn(base.resolve(), (ROOT.resolve(), *ROOT.resolve().parents))
                    self.assertEqual((roots["ios-arm64"] / "include/fixture/original.h").read_bytes(),
                                     b"authenticated header sentinel\n")
                self.assertTrue(all(not base.exists() for base in created))
                self.assertEqual(self.run_gate(values), trusted)
            self.assertEqual(len(created), 2)
            self.assertTrue(all(not base.exists() for base in created))
            self.assertEqual(values, original_inputs)
            self.assertEqual(compute_identities(ROOT, self.catalog), self.identities)

    def test_checkout_or_ancestor_output_has_no_cleanup_authority(self):
        for protected in (ROOT, ROOT.parent):
            with self.subTest(protected=str(protected)):
                # The cleanup spy is deliberately inert: destructive RED must
                # never delete checkout material, even with a broken guard.
                with mock.patch.object(evidence.tempfile, "mkdtemp", return_value=str(protected)), \
                     mock.patch.object(evidence.shutil, "rmtree") as cleanup:
                    with self.assertRaisesRegex(evidence.FullEvidenceError, "checkout"):
                        with self.projection():
                            self.fail("checkout or ancestor was exposed as owned output")
                    cleanup.assert_not_called()

    def test_projection_exposes_original_full_headers_support_receipts_and_archives_after_all_seven(self):
        values = self.inputs()
        verified = []

        def attestor(command, **kwargs):
            verified.append(pathlib.Path(command[3]).name)
            return self.mocked_signature_runner(command, **kwargs)

        with self.projection(values, attestation_runner=attestor) as (trusted, roots):
            self.assertEqual(len(verified), 7)
            self.assertEqual(trusted, self.run_gate(values))
            self.assertEqual(set(roots), set(values["candidate_archives"]))
            self.assertEqual(len({root.resolve() for root in roots.values()}), 7)
            for target, root in roots.items():
                identifier, files = self.full_files(values, target)
                self.assertNotIn(root.parent.parent.resolve(), (ROOT.resolve(), *ROOT.resolve().parents))
                for name, (raw, _) in files.items():
                    self.assertEqual((root / name).read_bytes(), raw, (target, name))
                tar_name = evidence._full_name(values["catalog"]["targets"][target]) + ".tar.gz"
                self.assertEqual((root.parent / tar_name).read_bytes(), values["full_downloads"][identifier])
            self.assertEqual((roots["ios-arm64"] / "include/fixture/original.h").read_bytes(),
                             b"authenticated header sentinel\n")
            self.assertEqual((roots["ios-arm64"] / "share/fixture/support.dat").read_bytes(),
                             b"authenticated support sentinel\x00")
            self.assertEqual(os.readlink(roots["ios-arm64"] / "lib/libfixture.dylib"),
                             "libfixture.1.dylib")
            owned = [root.parent.parent for root in roots.values()]
        self.assertTrue(all(not root.exists() for root in owned))

    def test_projection_cleans_its_own_storage_when_caller_raises(self):
        owned = None
        with self.assertRaisesRegex(RuntimeError, "caller stopped"):
            with self.projection() as (_, roots):
                owned = roots["ios-arm64"].parent.parent
                raise RuntimeError("caller stopped")
        self.assertIsNotNone(owned)
        self.assertFalse(owned.exists())

    def test_projection_rejects_bad_signature_and_subject_or_directory_substitution_before_yield(self):
        for variant in ("bad-signature", "changed-subject", "subject-symlink", "directory-alias"):
            paths = []
            with tempfile.TemporaryDirectory() as temporary:
                outside = pathlib.Path(temporary)
                sentinel = outside / "caller-owned"
                sentinel.write_bytes(b"keep")

                def attestor(command, **kwargs):
                    subject = pathlib.Path(command[3])
                    paths.append(subject.parent.parent)
                    if variant == "bad-signature":
                        raise subprocess.CalledProcessError(1, command)
                    if variant == "changed-subject":
                        subject.write_bytes(b"different signed subject")
                    elif variant == "subject-symlink":
                        replacement = outside / subject.name
                        replacement.write_bytes(subject.read_bytes())
                        subject.unlink()
                        subject.symlink_to(replacement)
                    else:
                        original = subject.parent
                        relocated = outside / "relocated-work"
                        original.rename(relocated)
                        original.symlink_to(relocated, target_is_directory=True)
                    return self.mocked_signature_runner(command, **kwargs)

                with self.subTest(variant=variant), self.assertRaises(evidence.FullEvidenceError):
                    with self.projection(attestation_runner=attestor):
                        self.fail("uncertain authenticated snapshot was exposed")
                self.assertTrue(paths)
                self.assertTrue(all(not path.exists() for path in paths))
                self.assertEqual(sentinel.read_bytes(), b"keep")

    def test_projection_rejects_invalid_full_members_and_core_mismatch_before_yield(self):
        for variant in ("mutated-header", "missing-header", "extra-member", "escaping-member",
                        "symlinked-header", "core-mismatch"):
            values = self.inputs()
            identifier, files = self.full_files(values)
            link = ("lib/libfixture.dylib", "libfixture.1.dylib")
            if variant == "mutated-header":
                files["include/fixture/original.h"] = (b"not the checksummed header", 0o644)
            elif variant == "missing-header":
                files.pop("include/fixture/original.h")
            elif variant == "extra-member":
                files["share/unlisted"] = (b"unlisted", 0o644)
            elif variant == "escaping-member":
                files["../escape"] = (b"escape", 0o644)
            elif variant == "symlinked-header":
                files.pop("include/fixture/original.h")
                link = ("include/fixture/original.h", "../../lib/libfixture.1.dylib")
            else:
                files["lib/libfixture.1.dylib"] = (b"not the core", 0o755)
                files["SHA256SUMS"] = ("".join(
                    f"{sha(data)}  {name}\n" for name, (data, _) in sorted(files.items())
                    if name != "SHA256SUMS").encode(), 0o644)
            values["full_downloads"][identifier] = make_tar(files, link=link)
            with self.subTest(variant=variant), self.assertRaises(evidence.FullEvidenceError):
                with self.projection(values):
                    self.fail("invalid full artifact was exposed")

    def test_projection_rejects_header_or_directory_mutation_by_external_metadata_runner(self):
        for variant in ("header", "checksum-rewrite", "directory-alias"):
            paths = []
            with tempfile.TemporaryDirectory() as temporary:
                outside = pathlib.Path(temporary)

                def verifier(command, **kwargs):
                    root = pathlib.Path(command[command.index("--stack-root") + 1])
                    paths.append(root.parent.parent)
                    if variant == "directory-alias":
                        relocated = outside / (root.parent.name + "-full")
                        root.rename(relocated)
                        root.symlink_to(relocated, target_is_directory=True)
                    else:
                        header = root / "include/fixture/original.h"
                        header.write_bytes(b"untrusted replacement")
                        if variant == "checksum-rewrite":
                            checksums = root / "SHA256SUMS"
                            checksums.write_text(checksums.read_text().replace(
                                sha(b"authenticated header sentinel\n"), sha(header.read_bytes())))
                    return subprocess.CompletedProcess(command, 0, stdout="test-only", stderr="")

                with self.subTest(variant=variant), self.assertRaises(evidence.FullEvidenceError):
                    with self.projection(run_verifier=verifier):
                        self.fail("mutated original full snapshot was exposed")
                self.assertTrue(paths)
                self.assertTrue(all(not path.exists() for path in paths))

    def test_projection_rejects_added_or_changed_links_after_external_metadata_runner(self):
        for variant in ("extra-header-link", "extra-directory-link", "changed-lib-link"):
            with tempfile.TemporaryDirectory() as temporary:
                outside = pathlib.Path(temporary)
                sentinel = outside / "caller-owned.h"
                sentinel.write_bytes(b"not authenticated by the full tar")

                def verifier(command, **kwargs):
                    root = pathlib.Path(command[command.index("--stack-root") + 1])
                    if variant == "changed-lib-link":
                        alias = root / "lib/libfixture.dylib"
                        alias.unlink()
                        alias.symlink_to(sentinel)
                    else:
                        alias = root / "include/fixture" / (
                            "extra.h" if variant == "extra-header-link" else "extra-directory")
                        alias.symlink_to(sentinel if variant == "extra-header-link" else outside,
                                         target_is_directory=variant == "extra-directory-link")
                    return self.mocked_metadata_runner(command, **kwargs)

                with self.subTest(variant=variant), self.assertRaises(evidence.FullEvidenceError):
                    with self.projection(run_verifier=verifier):
                        self.fail("unauthenticated link was exposed")
                self.assertEqual(sentinel.read_bytes(), b"not authenticated by the full tar")

    def test_projection_cleanup_preserves_foreign_base_replacement_from_signature_runner(self):
        with tempfile.TemporaryDirectory() as temporary:
            outside = pathlib.Path(temporary)
            replaced = []

            def attestor(command, **kwargs):
                subject = pathlib.Path(command[3])
                result = self.mocked_signature_runner(command, **kwargs)
                base = subject.parent.parent
                foreign = outside / "foreign"
                shutil.copytree(base, foreign)
                (foreign / "caller-owned").mkdir()
                (foreign / "caller-owned/sentinel").write_bytes(b"keep foreign tree")
                base.rename(outside / "original-owned-base")
                foreign.rename(base)
                replaced.append(base)
                return result

            try:
                with self.assertRaises(evidence.FullEvidenceError):
                    with self.projection(attestation_runner=attestor):
                        self.fail("foreign directory replacement was exposed")
                self.assertEqual(len(replaced), 1)
                self.assertEqual((replaced[0] / "caller-owned/sentinel").read_bytes(),
                                 b"keep foreign tree")
            finally:
                if replaced and replaced[0].exists():
                    replaced[0].rename(outside / "preserved-foreign")

    def test_projection_cleanup_preserves_foreign_base_and_original_caller_exception(self):
        with tempfile.TemporaryDirectory() as temporary:
            outside = pathlib.Path(temporary)
            replacement = outside / "foreign"
            replacement.mkdir()
            (replacement / "sentinel").write_bytes(b"keep foreign tree")
            base = None
            original_error = RuntimeError("original caller error")
            try:
                with self.assertRaises(RuntimeError) as caught:
                    with self.projection() as (_, roots):
                        base = roots["ios-arm64"].parent.parent
                        base.rename(outside / "original-owned-base")
                        replacement.rename(base)
                        raise original_error
                self.assertIs(caught.exception, original_error)
                self.assertIsNotNone(base)
                self.assertEqual((base / "sentinel").read_bytes(), b"keep foreign tree")
            finally:
                if base is not None and base.exists():
                    base.rename(outside / "preserved-foreign")

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
