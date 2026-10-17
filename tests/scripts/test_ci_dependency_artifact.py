"""Reject substituted cross-platform native-core OCI artifact manifests."""

import copy
import hashlib
import json
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_dependency_artifact as artifact


class DependencyArtifactContractTest(unittest.TestCase):
    def setUp(self):
        self.blob = b"synthetic cross-platform dependency core\n"
        self.blob_digest = "sha256:" + hashlib.sha256(self.blob).hexdigest()
        self.document = {
            "schemaVersion": 2,
            "mediaType": artifact.MANIFEST_MEDIA_TYPE,
            "artifactType": artifact.ARTIFACT_TYPE,
            "config": {
                "mediaType": artifact.EMPTY_CONFIG_MEDIA_TYPE,
                "digest": artifact.EMPTY_CONFIG_DIGEST,
                "size": 2,
                "data": "e30=",
            },
            "annotations": {"org.opencontainers.image.created": "2026-09-27T16:36:55Z"},
            "layers": [{
                "mediaType": artifact.CORE_LAYER_MEDIA_TYPE,
                "digest": self.blob_digest,
                "size": len(self.blob),
                "annotations": {"org.opencontainers.image.title": "adb-helper-linux-x86_64.tar.gz"},
            }],
        }

    def inspect(self, document, *, expected=None):
        raw = json.dumps(document, separators=(",", ":")).encode("utf-8")
        manifest_digest = "sha256:" + hashlib.sha256(raw).hexdigest()
        return artifact.inspect_manifest(
            raw, expected or manifest_digest,
            expected_blob_digest=self.blob_digest,
            expected_blob_size=len(self.blob),
            expected_archive_name="adb-helper-linux-x86_64.tar.gz",
        )

    def test_cross_platform_core_is_an_opaque_artifact_not_a_linux_image(self):
        identity = self.inspect(self.document)
        self.assertEqual(identity["blob_digest"], self.blob_digest)
        self.assertEqual(identity["archive_name"], "adb-helper-linux-x86_64.tar.gz")
        self.assertNotIn("platform", identity)

    def test_manifest_and_blob_substitutions_fail_closed(self):
        for mutation in (
            lambda d: d["layers"][0].update(digest="sha256:" + "0" * 64),
            lambda d: d["layers"][0].update(size=len(self.blob) + 1),
            lambda d: d["layers"][0].update(mediaType="application/octet-stream"),
            lambda d: d.update(artifactType="application/vnd.other"),
            lambda d: d["config"].update(digest="sha256:" + "f" * 64),
            lambda d: d["layers"].append(copy.deepcopy(d["layers"][0])),
            lambda d: d.update(subject={"digest": self.blob_digest}),
            lambda d: d["layers"][0]["annotations"].update(
                {"org.opencontainers.image.title": "other-target.tar.gz"}
            ),
        ):
            document = copy.deepcopy(self.document)
            mutation(document)
            with self.subTest(document=document), self.assertRaises(artifact.ArtifactError):
                self.inspect(document)

    def test_reject_index_platform_or_unrecognized_manifest_fields(self):
        for mutation in (
            lambda d: d.update(mediaType="application/vnd.oci.image.index.v1+json"),
            lambda d: d["config"].update(platform={"os": "linux", "architecture": "amd64"}),
            lambda d: d["layers"][0].update(platform={"os": "linux", "architecture": "amd64"}),
            lambda d: d.update(annotations={"untrusted": "field"}),
            lambda d: d["config"].update(data="e30=not-exact"),
            lambda d: d["annotations"].update({"org.opencontainers.image.created": "yesterday"}),
        ):
            document = copy.deepcopy(self.document)
            mutation(document)
            with self.subTest(document=document), self.assertRaises(artifact.ArtifactError):
                self.inspect(document)

    def test_raw_manifest_sha_and_duplicate_json_keys_are_authoritative(self):
        raw = json.dumps(self.document, separators=(",", ":")).encode("utf-8")
        with self.assertRaises(artifact.ArtifactError):
            artifact.inspect_manifest(
                raw, "sha256:" + "0" * 64,
                expected_blob_digest=self.blob_digest,
                expected_blob_size=len(self.blob),
                expected_archive_name="adb-helper-linux-x86_64.tar.gz",
            )
        repeated = raw.replace(b'"schemaVersion":2,', b'"schemaVersion":2,"schemaVersion":2,', 1)
        with self.assertRaises(artifact.ArtifactError):
            artifact.inspect_manifest(
                repeated, "sha256:" + hashlib.sha256(repeated).hexdigest(),
                expected_blob_digest=self.blob_digest,
                expected_blob_size=len(self.blob),
                expected_archive_name="adb-helper-linux-x86_64.tar.gz",
            )

    def test_oversized_or_untrusted_archive_names_are_rejected(self):
        for name in ("../core.tar.gz", "other/core.tar.gz", "", "adb-helper-x64:latest"):
            with self.subTest(name=name), self.assertRaises(artifact.ArtifactError):
                artifact.validate_archive_name(name)
        document = copy.deepcopy(self.document)
        document["layers"][0]["size"] = artifact.MAX_CORE_BYTES + 1
        with self.assertRaises(artifact.ArtifactError):
            self.inspect(document)


if __name__ == "__main__":
    unittest.main()
