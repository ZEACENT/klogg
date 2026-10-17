"""The native-core publisher may write only after independently trusted seven-way qualification."""

import copy
import gzip
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
import ci_dependency_artifact as artifact
import ci_dependency_catalog as catalog
import ci_dependency_pipeline as pipeline
import test_ci_dependency_pipeline as pipeline_tests


def sha(data):
    return hashlib.sha256(data).hexdigest()


def raw_json(document):
    return (json.dumps(document, sort_keys=True, separators=(",", ":")) + "\n").encode()


def gate_archive(document, materials=None):
    raw = raw_json(document)
    output = io.BytesIO()
    with gzip.GzipFile(fileobj=output, mode="wb", mtime=0) as zipped:
        with tarfile.open(fileobj=zipped, mode="w") as tar:
            item = tarfile.TarInfo("qualified.json")
            item.size = len(raw)
            tar.addfile(item, io.BytesIO(raw))
            for target, material in sorted((materials or {}).items()):
                item = tarfile.TarInfo(f"materials/{target}.json")
                item.size = len(material)
                tar.addfile(item, io.BytesIO(material))
    return output.getvalue()


class FakeRegistry:
    def __init__(self, archives):
        self.archives = archives
        self.manifests = {}
        self.requests = []
        self.invalid_manifest = False

    def _read(self, url):
        self.requests.append(url)
        name = url.rsplit("/", 1)[-1]
        return self.manifests[name] if not self.invalid_manifest else b'{}'

    def retrieve(self, digest, blob_digest, size, name, destination):
        archive = self.archives[name]
        self.requests.append(digest)
        if sha(archive) != blob_digest[7:] or len(archive) != size:
            raise ValueError("remote archive differs")
        destination.write_bytes(archive)
        return {"manifest_bytes": self.manifests[digest]}


class NativePublisherTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        pipeline_tests.DependencyPipelineTest.setUpClass()

    def setUp(self):
        import publish_ci_dependency as publisher
        self.publisher = publisher
        self.source = copy.deepcopy(pipeline_tests.DependencyPipelineTest.source)
        self.builders = copy.deepcopy(pipeline_tests.DependencyPipelineTest.builder)
        self.artifacts = copy.deepcopy(pipeline_tests.DependencyPipelineTest.artifacts)
        self.downloads = copy.deepcopy(pipeline_tests.DependencyPipelineTest.downloads)
        self.environment = copy.deepcopy(pipeline_tests.DependencyPipelineTest.environment)
        self.receipts = pipeline.qualify_from_repo(ROOT, self.source, self.builders,
                                                   self.artifacts, self.downloads,
                                                   trusted_environment=self.environment)
        self.signed_material = {}
        for target, receipt in self.receipts.items():
            document = {
                "schema_version": 1, "kind": "native-dependency-signed-material",
                "target": target, "source": self.source,
                "full_artifact_id": 1000 + len(self.signed_material),
                "full_tar_sha256": sha((target + ":full").encode()),
                "full_tar_bundle_sha256": sha((target + ":bundle").encode()),
                "candidate_artifact_id": receipt["artifact_id"],
                "core_sha256": receipt["archive"]["sha256"],
                "trusted_environment": self.environment[target],
            }
            if target.startswith("adb-"):
                document["legal"] = {
                    "support_artifact_id": 2000,
                    "support_zip_sha256": sha(b"support"),
                    "full_release_artifact_id": 2001,
                    "full_release_zip_sha256": sha(b"release"),
                    "source_set_receipt_sha256": sha(b"source set"),
                    "overlay_receipt_sha256": sha(b"overlay"),
                    "source_archive_sha256": sha(b"source archive"),
                }
            self.signed_material[target] = raw_json(document)
        self.gate_id = 999
        self.gate = {
            "schema_version": 1, "kind": "native-dependency-qualified-run",
            "result": "success", "source": self.source,
            "targets": {
                target: {"receipt": receipt,
                         "signed_material_sha256": sha(self.signed_material[target])}
                for target, receipt in self.receipts.items()},
        }
        self.gate_bytes = gate_archive(self.gate, self.signed_material)
        self.gate_sha = sha(self.gate_bytes)
        self.trusted = mock.Mock(return_value=copy.deepcopy(self.gate))
        self.registry = FakeRegistry({row["archive_name"]: self.downloads[self.receipts[target]["artifact_id"]]["archive"]
                                      for target, row in json.loads((ROOT / "ci/dependencies/catalog.json").read_bytes())["targets"].items()})
        self.commands = []
        self.token = "secret-do-not-log"
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)

    def run_publish(self, **overrides):
        args = dict(repo_root=ROOT, source=self.source, gate_artifact_id=self.gate_id,
                    gate_archive_sha256=self.gate_sha, gate_bytes=self.gate_bytes,
                    verify_gate=self.trusted, signed_material=self.signed_material,
                    builders=self.builders, artifacts=self.artifacts, downloads=self.downloads,
                    trusted_environment=self.environment, runner=self.run_oras,
                    registry_client=self.registry,
                    environment={"GH_TOKEN": self.token, "GITHUB_ACTOR": "tester"})
        args.update(overrides)
        return self.publisher.publish(**args)

    def run_oras(self, argv, **kwargs):
        self.commands.append((argv, kwargs))
        if argv[1] == "push":
            tag = argv[argv.index("--registry-config") + 2]
            path, media_type = argv[-1].rsplit(":", 1)
            source = pathlib.Path(path)
            if not source.is_absolute():
                source = pathlib.Path(kwargs["cwd"]) / source
            name = source.name
            archive = source.read_bytes()
            assert media_type == artifact.CORE_LAYER_MEDIA_TYPE
            assert archive == self.registry.archives[name]
            created = argv[argv.index("--annotation") + 1].split("=", 1)[1]
            manifest = raw_json({
                "schemaVersion": 2, "mediaType": artifact.MANIFEST_MEDIA_TYPE,
                "artifactType": artifact.ARTIFACT_TYPE,
                "config": {"mediaType": artifact.EMPTY_CONFIG_MEDIA_TYPE,
                           "digest": artifact.EMPTY_CONFIG_DIGEST, "size": 2, "data": "e30="},
                "layers": [{"mediaType": artifact.CORE_LAYER_MEDIA_TYPE,
                            "digest": "sha256:" + sha(archive), "size": len(archive),
                            "annotations": {"org.opencontainers.image.title": name}}],
                "annotations": {"org.opencontainers.image.created": created},
            })
            self.registry.manifests[tag.rsplit(":", 1)[-1]] = manifest
            self.registry.manifests["sha256:" + sha(manifest)] = manifest
        return mock.Mock(returncode=0, stdout="", stderr="")

    def test_gate_archive_carries_exact_seven_bound_material_records(self):
        archived = gate_archive(self.gate, self.signed_material)
        verified = self.publisher._qualified(
            ROOT, self.source, self.gate_id, sha(archived), archived,
            mock.Mock(return_value=self.gate), self.signed_material,
            self.builders, self.artifacts, self.downloads, self.environment,
        )
        self.assertEqual(verified, self.receipts)

    def test_rehashed_but_unbound_material_cannot_authorize_publication(self):
        for material in (b'{"x":1}\n',
                         raw_json({**json.loads(self.signed_material["ios-arm64"]),
                                   "target": "ios-x86_64"})):
            with self.subTest(material=material):
                signed = dict(self.signed_material, **{"ios-arm64": material})
                gate = copy.deepcopy(self.gate)
                gate["targets"]["ios-arm64"]["signed_material_sha256"] = sha(material)
                archived = gate_archive(gate, signed)
                with self.assertRaises(self.publisher.PublicationError):
                    self.publisher._qualified(
                        ROOT, self.source, self.gate_id, sha(archived), archived,
                        mock.Mock(return_value=gate), signed, self.builders,
                        self.artifacts, self.downloads, self.environment,
                    )

    def test_gate_archive_does_not_need_its_unavailable_upload_id_inside_itself(self):
        gate = copy.deepcopy(self.gate)
        self.assertNotIn("artifact_id", gate)
        archived = gate_archive(gate, self.signed_material)
        verified = self.publisher._qualified(
            ROOT, self.source, self.gate_id, sha(archived), archived,
            mock.Mock(return_value=gate), self.signed_material,
            self.builders, self.artifacts, self.downloads, self.environment,
        )
        self.assertEqual(verified, self.receipts)

    def test_missing_trusted_gate_is_never_promoted_from_candidate_receipts(self):
        with self.assertRaises(self.publisher.PublicationError):
            self.run_publish(verify_gate=None)
        self.assertEqual(self.commands, [])

    def test_wrong_seventh_target_fails_before_credentials_or_oras_acquisition(self):
        self.gate["targets"]["ios-x86_64"]["receipt"]["archive"]["sha256"] = "d" * 64
        self.gate_bytes = gate_archive(self.gate, self.signed_material)
        self.gate_sha = sha(self.gate_bytes)
        self.trusted.return_value = copy.deepcopy(self.gate)
        with mock.patch.object(self.publisher, "tool_pin") as tool, self.assertRaises(self.publisher.PublicationError):
            self.run_publish(environment={})
        tool.assert_not_called()
        self.assertEqual(self.commands, [])

    def test_modified_candidate_or_signed_material_fails_before_login(self):
        item = self.receipts["ios-x86_64"]["artifact_id"]
        self.downloads[item]["archive"] += b"changed"
        with mock.patch.object(self.publisher, "tool_pin") as tool, self.assertRaises(self.publisher.PublicationError):
            self.run_publish()
        tool.assert_not_called()
        self.downloads[item]["archive"] = self.registry.archives["ios-native-x86_64.tar.gz"]
        self.signed_material["ios-x86_64"] += b"changed"
        with mock.patch.object(self.publisher, "tool_pin") as tool, self.assertRaises(self.publisher.PublicationError):
            self.run_publish()
        tool.assert_not_called()

    def test_gate_must_be_authenticated_same_run_and_successful(self):
        for mutation in (lambda g: g.update(result="skipped"),
                         lambda g: g["source"].update(run_attempt=1),
                         lambda g: g.update(artifact_id=1000)):
            gate = copy.deepcopy(self.gate)
            mutation(gate)
            with self.subTest(gate=gate), self.assertRaises(self.publisher.PublicationError):
                self.run_publish(gate_bytes=gate_archive(gate, self.signed_material),
                                 gate_archive_sha256=sha(gate_archive(gate, self.signed_material)),
                                 verify_gate=mock.Mock(return_value=gate))
            self.assertEqual(self.commands, [])
        with self.assertRaises(self.publisher.PublicationError):
            self.run_publish(verify_gate=mock.Mock(return_value=False))

    def test_remote_manifest_or_blob_mismatch_fails_closed_without_token_diagnostics(self):
        for wrong in ("manifest", "blob"):
            registry = FakeRegistry(dict(self.registry.archives))
            if wrong == "manifest":
                registry.invalid_manifest = True
            else:
                registry.archives["ios-native-x86_64.tar.gz"] = b"tampered"
            with self.subTest(wrong=wrong), mock.patch.object(self.publisher, "tool_pin", return_value={
                "version": "1.3.4", "url": "https://example.test/oras", "sha256": sha(b"tool"),
                "executable": "oras",
            }), mock.patch.object(self.publisher, "_acquire_oras", return_value="/tmp/oras"):
                with self.assertRaises(Exception) as failure:
                    self.run_publish(registry_client=registry)
                self.assertNotIn(self.token, str(failure.exception))

    def test_success_returns_actual_raw_manifests_for_later_detached_signing(self):
        with mock.patch.object(self.publisher, "tool_pin", return_value={
            "version": "1.3.4", "url": "https://example.test/oras", "sha256": sha(b"tool"),
            "executable": "oras",
        }), mock.patch.object(self.publisher, "_acquire_oras", return_value="/tmp/oras"):
            publication = self.run_publish()
        self.assertEqual(set(publication["targets"]), set(catalog.TARGETS))
        self.assertEqual(publication["source"], self.source)
        self.assertEqual(publication["qualified_artifact_id"], self.gate_id)
        for target, record in publication["targets"].items():
            raw = record["manifest_bytes"]
            self.assertEqual(record["manifest_digest"], "sha256:" + sha(raw))
            self.assertEqual(record["blob_digest"], "sha256:" + self.receipts[target]["archive"]["sha256"])
            self.assertNotIn("signature", record)
        self.assertEqual(len([args for args, _ in self.commands if args[1] == "push"]), 7)
        self.assertEqual(len([args for args, _ in self.commands if args[1] == "login"]), 1)
        self.assertTrue(all(self.token not in str(args) for args, _ in self.commands))

    def test_oras_push_uses_relative_basename_and_archive_working_directory(self):
        with mock.patch.object(self.publisher, "tool_pin", return_value={
            "version": "1.3.4", "url": "https://example.test/oras", "sha256": sha(b"tool"),
            "executable": "oras",
        }), mock.patch.object(self.publisher, "_acquire_oras", return_value="/tmp/oras"):
            self.run_publish()
        pushes = [(argv, options) for argv, options in self.commands if argv[1] == "push"]
        self.assertEqual(len(pushes), 7)
        for argv, options in pushes:
            source, media_type = argv[-1].rsplit(":", 1)
            with self.subTest(source=source):
                self.assertFalse(pathlib.Path(source).is_absolute())
                self.assertEqual(source, pathlib.Path(source).name)
                self.assertTrue(pathlib.Path(options["cwd"]).is_absolute())
                self.assertEqual(media_type, artifact.CORE_LAYER_MEDIA_TYPE)

    def test_oras_checksum_mismatch_cannot_login(self):
        def unpinned_download(_url, destination):
            destination.write_bytes(b"not the pinned ORAS archive")

        with mock.patch.object(self.publisher, "tool_pin", return_value={
            "version": "1.3.4", "url": "https://example.test/oras", "sha256": sha(b"expected"),
            "executable": "oras",
        }), self.assertRaisesRegex(self.publisher.PublicationError, "SHA-verified"):
            self.run_publish(downloader=unpinned_download)
        self.assertEqual(self.commands, [])

    def test_oras_failed_command_never_exposes_stdin_or_stderr_token(self):
        def failed(_argv, **kwargs):
            raise subprocess.CalledProcessError(
                1, _argv, stderr=self.token + " sensitive stderr")

        with mock.patch.object(self.publisher, "tool_pin", return_value={
            "version": "1.3.4", "url": "https://example.test/oras", "sha256": sha(b"tool"),
            "executable": "oras",
        }), mock.patch.object(self.publisher, "_acquire_oras", return_value="/tmp/oras"):
            with self.assertRaises(self.publisher.PublicationError) as failure:
                self.run_publish(runner=failed)
        self.assertNotIn(self.token, str(failure.exception))

    def test_pinned_tool_is_checked_against_official_release_and_checksum(self):
        pin = self.publisher.tool_pin(ROOT)
        self.assertEqual(pin["version"], "1.3.4")
        self.assertEqual(len(pin["sha256"]), 64)
        self.assertEqual(pin["executable"], "oras")


if __name__ == "__main__":
    unittest.main()
