"""Same-run Linux bootstrap orchestration with fake acquisition/build processes."""
from __future__ import annotations

import contextlib
import copy
import hashlib
import importlib
import io
import json
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
import ci_environment as core
import ci_environment_pipeline as pipeline
import build_adb_helper_legal_assets as legal
import verify_adb_helper_artifact as verifier
import verify_adb_helper_envelope as envelope
from source_publication_identity import published_source_name
import test_adb_helper_verifier_contract as adb_tests

VERSION = "26.09.24.1234"
SOURCE = {"repository": "ZEACENT/klogg", "sha": "a" * 40, "ref": "refs/heads/test-fixture",
          "workflow": ".github/workflows/ci-environments.yml", "run_id": 123, "run_attempt": 2}
TOOL_BYTES = b"synthetic pinned AppImage bytes for orchestration test\n"


class EnvironmentFixtureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (ROOT / "scripts/ci_environment_fixture.py").is_file():
            raise AssertionError("same-run Linux fixture orchestrator is missing")
        cls.module = importlib.import_module("ci_environment_fixture")

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="environment-fixture-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name).resolve()
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.output = self.root / "prepared"
        self.commands = []
        self.helper_fixture = adb_tests.AdbHelperVerifierContractTest("runTest")
        self.helper_fixture.root = self.root / "upstream-fixture"
        self.helper_fixture.root.mkdir()
        lock_path, receipt_path, package, release, receipt = self.helper_fixture.make_release_fixture()
        self.helper_package, self.legal_release = package, release
        lock = core.load_json(lock_path)
        lock["targets"]["linux-x86_64"]["toolchain"] = "linux-x86_64"
        lock["toolchains"] = {"linux-x86_64": {
            "container_image": "example.invalid/manylinux", "container_digest": "sha256:" + "b" * 64}}
        self.lock = self.repo / "packaging/adb/adb-helper.lock.json"
        self.lock.parent.mkdir(parents=True)
        self.lock.write_text(json.dumps(lock))
        self.image = "example.invalid/manylinux@sha256:" + "b" * 64
        offer = release / "ADB-HELPER-SOURCE-OFFER.txt"
        source_archive = release / "adb-helper-source-archive.tar.gz"
        offer.write_text("Published archive: " + published_source_name(VERSION, "adb-helper", pipeline.sha256(source_archive))
                         + "\nSHA-256: " + pipeline.sha256(source_archive) + "\n")
        source_set_path = release / "adb-helper-source-set-receipt.json"
        source_set = core.load_json(source_set_path)
        source_set["lock_sha256"] = pipeline.sha256(self.lock)
        for entry in source_set["package_support_assets"]:
            entry["sha256"] = pipeline.sha256(release / entry["file_name"])
        source_set_path.write_text(json.dumps(source_set))
        receipt["receipt_kind"] = "binary-build"
        receipt["lock_sha256"] = pipeline.sha256(self.lock)
        receipt["source_set_receipt_sha256"] = pipeline.sha256(source_set_path)
        receipt["layout"] = "artifact"
        receipt["helper"]["path"] = "helpers/adb"
        for entry in receipt["release_assets"]:
            entry["sha256"] = pipeline.sha256(release / entry["path"])
        self.binary_receipt = receipt
        for asset in lock["release_assets"]:
            legal.write_hash(release / asset["file_name"])
        (release / "adb-helper-release-assets.json").write_text(json.dumps(receipt["release_assets"]))
        self.pin = {
            "repository": "probonopd/linuxdeployqt", "release_id": 11, "tag": "11", "asset_id": 22,
            "asset_name": "linuxdeployqt-continuous-x86_64.AppImage", "size": len(TOOL_BYTES),
            "sha256": hashlib.sha256(TOOL_BYTES).hexdigest(),
            "url": "https://github.com/probonopd/linuxdeployqt/releases/download/11/linuxdeployqt-continuous-x86_64.AppImage",
        }
        directory = self.repo / "ci/environments"
        directory.mkdir(parents=True)
        (directory / "package-tools.json").write_text(json.dumps({"schema_version": 1, "linuxdeployqt": self.pin}))
        (self.repo / "scripts").mkdir()
        for name in ("prefetch_adb_helper_sources.py", "build_adb_helper_legal_assets.py", "verify_adb_helper_artifact.py",
                     "verify_adb_helper_toolchain.py", "build_adb_helper.py", "smoke_adb_helper.py"):
            (self.repo / "scripts" / name).write_text("# Test process seam, not executed.\n")
        self.verifier_failure = False
        self.current_sha = SOURCE["sha"]

    def asset_metadata(self, url, *, timeout):
        self.assertEqual(url, "https://api.github.com/repos/probonopd/linuxdeployqt/releases/assets/22")
        return {"id": 22, "name": self.pin["asset_name"], "size": self.pin["size"],
                "digest": "sha256:" + self.pin["sha256"], "browser_download_url": self.pin["url"], "state": "uploaded"}

    def download(self, url, destination, *, timeout):
        self.assertEqual(url, self.pin["url"])
        destination.write_bytes(TOOL_BYTES)

    def call_verifier(self, arguments):
        with mock.patch.object(sys, "argv", ["verify_adb_helper_artifact.py"] + arguments), contextlib.redirect_stdout(io.StringIO()):
            result = verifier.main()
        if result:
            raise subprocess.CalledProcessError(result, arguments)

    def docker_build_output(self, work):
        artifact = work / "fixture/prefetch_artifacts/adb-helper"
        shutil.copytree(self.helper_package / "usr/bin/helpers", artifact / "helpers")
        (artifact / "receipt.json").write_text(json.dumps(self.binary_receipt))
        smoke = self.helper_fixture.write_smoke_receipt(artifact / "helpers/adb")
        shutil.copyfile(smoke, artifact / "smoke.json")
        shutil.copyfile(smoke, artifact / "package-smoke.json")
        self.call_verifier([
            "--lock", str(self.lock), "--receipt", str(artifact / "receipt.json"),
            "--package-root", str(artifact), "--asset-scope", "package",
            "--source-assets-root", str(work / "package-support"), "--helper-path", "helpers/adb",
            "--expected-target", "linux-x86_64", "--binary-smoke-receipt", str(artifact / "package-smoke.json"),
            "--package-verification-receipt", str(artifact / "package-verification.json"), "--require-lock-binding",
        ])

    def runner(self, command, **kwargs):
        self.commands.append(command)
        self.assertTrue(kwargs["check"])
        if command[:3] == ["git", "rev-parse", "HEAD"]:
            self.assertEqual(pathlib.Path(kwargs["cwd"]), self.repo)
            stdout = self.current_sha + "\n"
        elif command[:2] == ["gh", "api"]:
            stdout = json.dumps({"id": 987, "expired": False, "workflow_run": {
                "id": SOURCE["run_id"], "head_sha": SOURCE["sha"], "head_branch": "test-fixture"}})
        elif command[:2] == ["docker", "pull"]:
            self.assertIn(self.image, command)
            stdout = "pulled pinned helper toolchain"
        elif command[:2] == ["docker", "run"]:
            self.assertIn("--network=none", command)
            self.assertIn("--platform=linux/amd64", command)
            self.assertNotIn("--privileged", command)
            mounts = [command[i + 1] for i, item in enumerate(command[:-1]) if item == "--mount"]
            work = pathlib.Path(next(item for item in mounts if "target=/work" in item).split("source=", 1)[1].split(",", 1)[0])
            script = command[command.index("-lc") + 1]
            for required in ("verify_adb_helper_toolchain.py", "--containerized", "build_adb_helper.py",
                             "smoke_adb_helper.py", "verify_adb_helper_artifact.py", "--require-lock-binding"):
                self.assertIn(required, script)
            self.assertNotIn("--allow-unlocked-local", script)
            self.docker_build_output(work)
            stdout = "built and smoke-verified fixture"
        else:
            name = pathlib.Path(command[1]).name
            if name == "prefetch_adb_helper_sources.py":
                directory = pathlib.Path(command[command.index("--download-root") + 1])
                directory.mkdir(exist_ok=True)
                if "--extract-root" in command:
                    self.assertIn("--offline", command)
                    pathlib.Path(command[command.index("--extract-root") + 1]).mkdir()
                stdout = "verified locked source closure"
            elif name == "build_adb_helper_legal_assets.py":
                self.assertEqual(command[command.index("--version") + 1], VERSION)
                release = pathlib.Path(command[command.index("--output") + 1])
                support = pathlib.Path(command[command.index("--package-support-output") + 1])
                shutil.copytree(self.legal_release, release)
                legal.materialize_package_support(release, support, core.load_json(self.lock)["release_assets"])
                stdout = "built legal assets using current version"
            elif name == "verify_adb_helper_artifact.py":
                if self.verifier_failure:
                    raise subprocess.CalledProcessError(1, command)
                self.call_verifier(command[2:])
                stdout = "existing production verifier passed"
            else:
                self.fail("unexpected subprocess: " + repr(command))
        return subprocess.CompletedProcess(command, 0, stdout, "")

    def prepare(self):
        return self.module.prepare_fixture(self.repo, SOURCE, VERSION, self.output, runner=self.runner,
            downloader=self.download, metadata_reader=self.asset_metadata)

    def transport(self):
        root = self.root / "transport"
        root.mkdir()
        archive = root / "fixture.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            output.add(self.output, arcname=".")
        return root, pipeline.sha256(archive)

    def consume(self, root, digest, **kwargs):
        return self.module.consume_fixture(self.repo, SOURCE, root, 987, digest, self.repo,
                                          runner=self.runner, version=VERSION, **kwargs)

    def test_prepare_reuses_real_legal_binary_smoke_and_envelope_contracts(self):
        document = self.prepare()
        self.assertEqual(document["kind"], "ci-linux-package-fixture")
        self.assertEqual(document["source"], SOURCE)
        self.assertEqual(document["version"], VERSION)
        self.assertEqual(document["lock_sha256"], pipeline.sha256(self.lock))
        artifact = self.output / "prefetch_artifacts/adb-helper"
        envelope.verify_artifact_envelope(self.lock, artifact, "linux-x86_64")
        self.assertTrue((artifact / "release/ADB-HELPER-SOURCE-OFFER.txt").is_file())
        self.assertFalse((artifact / "release/adb-helper-source-archive.tar.gz").exists())
        self.assertTrue((self.output / "source-assets/adb-helper/adb-helper-source-archive.tar.gz").is_file())
        self.assertEqual((self.output / "tools" / self.pin["asset_name"]).read_bytes(), TOOL_BYTES)
        commands = [pathlib.Path(command[1]).name for command in self.commands if len(command) > 1]
        self.assertEqual(commands.count("prefetch_adb_helper_sources.py"), 2)
        self.assertEqual(commands.count("run"), 1)
        self.assertNotIn("attestation", json.dumps(self.commands))
        self.assertNotIn("inspection-only", json.dumps(self.commands))

    def test_consume_checks_same_run_and_preserves_unrelated_workspace_material(self):
        self.prepare()
        root, digest = self.transport()
        (self.repo / "tools").mkdir()
        (self.repo / "tools/existing-script").write_bytes(b"keep")
        result = self.consume(root, digest)
        self.assertEqual(result["fixture_artifact_id"], 987)
        self.assertEqual(result["archive_sha256"], digest)
        self.assertEqual((self.repo / "tools/existing-script").read_bytes(), b"keep")
        self.assertEqual((self.repo / "tools" / self.pin["asset_name"]).stat().st_mode & 0o777, 0o755)
        envelope.verify_artifact_envelope(self.lock, self.repo / "prefetch_artifacts/adb-helper", "linux-x86_64")
        self.assertTrue(any(command[:2] == ["gh", "api"] for command in self.commands))
        self.assertTrue((self.repo / "prefetch_artifacts/ci-environment-fixture.json").is_file())

    def test_wrong_source_hash_pin_or_subprocess_failure_never_publishes(self):
        for failure in ("source", "download", "metadata", "verifier"):
            with self.subTest(failure=failure):
                self.current_sha = "c" * 40 if failure == "source" else SOURCE["sha"]
                self.verifier_failure = failure == "verifier"
                def download(url, path, **kwargs):
                    path.write_bytes(b"wrong" if failure == "download" else TOOL_BYTES)
                def metadata(url, **kwargs):
                    value = self.asset_metadata(url, **kwargs)
                    if failure == "metadata":
                        value["id"] += 1
                    return value
                with self.assertRaises(core.ContractError):
                    self.module.prepare_fixture(self.repo, SOURCE, VERSION, self.output, runner=self.runner,
                        downloader=download, metadata_reader=metadata)
                self.assertFalse(self.output.exists())

    def test_consumer_rejects_tampered_archive_without_workspace_writes(self):
        self.prepare()
        root, digest = self.transport()
        with (root / "fixture.tar.gz").open("ab") as stream:
            stream.write(b"tampered")
        with self.assertRaises(core.ContractError):
            self.consume(root, digest)
        self.assertFalse((self.repo / "prefetch_artifacts").exists())

    def test_consumer_rejects_other_attempt_version_and_rehashed_payload_tampering(self):
        self.prepare()
        pristine = core.load_json(self.output / "fixture.json")
        for mutation in ("attempt", "version", "payload"):
            with self.subTest(mutation=mutation):
                document = copy.deepcopy(pristine)
                if mutation == "attempt":
                    document["source"]["run_attempt"] -= 1
                elif mutation == "version":
                    document["version"] = "26.09.23.1234"
                else:
                    (self.output / "tools" / self.pin["asset_name"]).write_bytes(b"tampered")
                (self.output / "fixture.json").write_text(json.dumps(document))
                root, digest = self.transport()
                with self.assertRaises(core.ContractError):
                    self.consume(root, digest)
                shutil.rmtree(root)
                self.assertFalse((self.repo / "prefetch_artifacts").exists())

    def test_consumer_refuses_replacing_existing_helper_or_linked_parents(self):
        self.prepare()
        root, digest = self.transport()
        (self.repo / "prefetch_artifacts/adb-helper").mkdir(parents=True)
        with self.assertRaises(core.ContractError):
            self.consume(root, digest)
        shutil.rmtree(self.repo / "prefetch_artifacts")
        (self.repo / "prefetch_artifacts").symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(core.ContractError):
            self.consume(root, digest)
        self.assertFalse((self.root / "adb-helper").exists())

    def test_container_timeout_removes_only_its_named_helper_build(self):
        commands = []
        def runner(command, **kwargs):
            commands.append(command)
            if command[:2] == ["docker", "run"]:
                raise subprocess.TimeoutExpired(command, 20)
            if command[:2] == ["docker", "rm"]:
                return subprocess.CompletedProcess(command, 0, "removed", "")
            return self.runner(command, **kwargs)
        with self.assertRaises(core.ContractError):
            self.module.prepare_fixture(self.repo, SOURCE, VERSION, self.output, runner=runner,
                downloader=self.download, metadata_reader=self.asset_metadata, timeout=20)
        build = next(command for command in commands if command[:2] == ["docker", "run"])
        self.assertIn("--name", build)
        name = build[build.index("--name") + 1]
        self.assertIn(["docker", "rm", "--force", name], commands)
        self.assertFalse(any("prune" in command for command in commands))
        self.assertFalse(self.output.exists())

    def test_consumer_rolls_back_new_paths_if_later_publication_fails(self):
        self.prepare()
        root, digest = self.transport()
        original = pathlib.Path.rename
        tool = self.repo / "tools" / self.pin["asset_name"]
        for error_type in (OSError, KeyboardInterrupt):
            with self.subTest(error=error_type.__name__):
                def failing_rename(path, destination):
                    if pathlib.Path(destination) == tool:
                        raise error_type("injected second publication failure")
                    return original(path, destination)
                with mock.patch.object(pathlib.Path, "rename", failing_rename):
                    with self.assertRaises(KeyboardInterrupt if error_type is KeyboardInterrupt else core.ContractError):
                        self.consume(root, digest)
                self.assertFalse((self.repo / "prefetch_artifacts").exists())
                self.assertFalse((self.repo / "tools").exists())

    def test_consumer_rejects_hostile_archive_before_writing_any_payload(self):
        root = self.root / "transport"
        root.mkdir()
        archive = root / "fixture.tar.gz"
        for member_name, is_link in (("../escape", False), ("link", True)):
            with self.subTest(member=member_name):
                with tarfile.open(archive, "w:gz") as output:
                    member = tarfile.TarInfo(member_name)
                    if is_link:
                        member.type = tarfile.SYMTYPE
                        member.linkname = "../escape"
                        output.addfile(member)
                    else:
                        member.size = 3
                        output.addfile(member, io.BytesIO(b"bad"))
                with self.assertRaises(core.ContractError):
                    self.consume(root, pipeline.sha256(archive))
                self.assertFalse((self.repo / "prefetch_artifacts").exists())
                self.assertFalse((self.repo / "escape").exists())

    def test_consumer_bounds_expanded_payload_before_extraction(self):
        root = self.root / "transport"
        root.mkdir()
        archive = root / "fixture.tar.gz"
        with tarfile.open(archive, "w:gz") as output:
            member = tarfile.TarInfo("oversized")
            member.size = 5000
            output.addfile(member, io.BytesIO(b"x" * member.size))
        with mock.patch.object(self.module, "MAX_BYTES", 1024), \
                mock.patch.object(self.module, "extract_archive", wraps=self.module.extract_archive) as extract:
            with self.assertRaises(core.ContractError):
                self.consume(root, pipeline.sha256(archive))
            extract.assert_not_called()
        self.assertFalse((self.repo / "prefetch_artifacts").exists())

    def test_same_artifact_id_from_another_run_is_not_trusted(self):
        self.prepare()
        root, digest = self.transport()
        def runner(command, **kwargs):
            result = self.runner(command, **kwargs)
            if command[:2] == ["gh", "api"]:
                record = json.loads(result.stdout)
                record["workflow_run"]["id"] += 1
                return subprocess.CompletedProcess(command, 0, json.dumps(record), "")
            return result
        with self.assertRaises(core.ContractError):
            self.module.consume_fixture(self.repo, SOURCE, root, 987, digest, self.repo,
                                        runner=runner, version=VERSION)
        self.assertFalse((self.repo / "prefetch_artifacts").exists())

    def test_production_package_tool_pin_is_versioned_real_and_complete(self):
        pin = self.module.load_package_tool(ROOT)
        self.assertEqual(pin["tag"], "11")
        self.assertEqual(pin["asset_id"], 260471530)
        self.assertEqual(pin["sha256"], "65192b29bee5f8da8820672aeee91f217896b08ca6236c84d996813129fb3510")
        self.assertEqual(pin["size"], 17192128)
        self.assertNotIn("/download/continuous/", pin["url"])


if __name__ == "__main__":
    unittest.main()
