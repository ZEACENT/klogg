"""Anonymous registry retrieval verifies exact cross-platform archive bytes."""

import hashlib
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
import urllib.error

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_dependency_artifact as artifact
import ci_dependency_registry as registry


class Response(io.BytesIO):
    def __init__(self, data, headers=None):
        super().__init__(data)
        self.headers = headers or {}


class Transport:
    def __init__(self, responses):
        self.responses = list(responses)
        self.requests = []

    def open(self, request, timeout):
        self.requests.append(request)
        if not self.responses:
            raise AssertionError("unexpected registry request")
        response = self.responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return Response(response)


class DependencyRegistryTest(unittest.TestCase):
    def setUp(self):
        self.blob = b"synthetic native core archive"
        self.blob_digest = "sha256:" + hashlib.sha256(self.blob).hexdigest()
        self.manifest = json.dumps({
            "schemaVersion": 2,
            "mediaType": artifact.MANIFEST_MEDIA_TYPE,
            "artifactType": artifact.ARTIFACT_TYPE,
            "config": {
                "mediaType": artifact.EMPTY_CONFIG_MEDIA_TYPE,
                "digest": artifact.EMPTY_CONFIG_DIGEST,
                "size": 2,
                "data": "e30=",
            },
            "layers": [{
                "mediaType": artifact.CORE_LAYER_MEDIA_TYPE,
                "digest": self.blob_digest,
                "size": len(self.blob),
                "annotations": {"org.opencontainers.image.title": "ios-native-arm64.tar.gz"},
            }],
            "annotations": {"org.opencontainers.image.created": "2026-09-27T10:00:00Z"},
        }, separators=(",", ":")).encode()
        self.manifest_digest = "sha256:" + hashlib.sha256(self.manifest).hexdigest()

    def retrieve(self, responses, *, destination=None):
        transport = Transport(responses)
        client = registry.DependencyRegistryClient(transport)
        if destination is None:
            self.temporary = tempfile.TemporaryDirectory()
            self.addCleanup(self.temporary.cleanup)
            destination = pathlib.Path(self.temporary.name) / "native-core.tar.gz"
        result = client.retrieve(
            self.manifest_digest,
            self.blob_digest,
            len(self.blob),
            "ios-native-arm64.tar.gz",
            destination,
        )
        return result, destination, transport

    def test_anonymous_manifest_and_blob_are_digest_verified(self):
        identity, destination, transport = self.retrieve([self.manifest, self.blob])
        self.assertEqual(destination.read_bytes(), self.blob)
        self.assertEqual(identity["blob_digest"], self.blob_digest)
        self.assertEqual(identity["manifest_bytes"], self.manifest)
        self.assertEqual(len(transport.requests), 2)
        self.assertTrue(all(
            request.full_url.startswith("https://ghcr.io/v2/zeacent/klogg-ci-deps/")
            for request in transport.requests
        ))
        self.assertTrue(all(request.get_header("Authorization") is None
                            for request in transport.requests))

    def test_blob_substitution_and_truncation_leave_no_final_archive(self):
        for blob in (b"tampered native core archive", self.blob[:-1], self.blob + b"extra"):
            with self.subTest(blob=blob), tempfile.TemporaryDirectory() as temporary:
                destination = pathlib.Path(temporary) / "native-core.tar.gz"
                with self.assertRaises(registry.DependencyRegistryError):
                    self.retrieve([self.manifest, blob], destination=destination)
                self.assertFalse(destination.exists())

    def test_anonymous_token_scope_is_dependency_package_not_environment(self):
        challenge = urllib.error.HTTPError(
            "https://ghcr.io/v2/zeacent/klogg-ci-deps/manifests/" + self.manifest_digest,
            401, "Unauthorized", {"WWW-Authenticate": (
                'Bearer realm="https://ghcr.io/token",service="ghcr.io",'
                'scope="repository:zeacent/klogg-ci-deps:pull"'
            )}, None,
        )
        token = json.dumps({"token": "public-read-token"}).encode()
        identity, destination, transport = self.retrieve([
            challenge, token, self.manifest, self.blob
        ])
        self.assertEqual(identity["blob_digest"], self.blob_digest)
        self.assertIn("repository%3Azeacent%2Fklogg-ci-deps%3Apull", transport.requests[1].full_url)
        self.assertEqual(transport.requests[-1].get_header("Authorization"), "Bearer public-read-token")

    def test_blob_only_challenge_obtains_the_same_anonymous_package_scope(self):
        blob_url = "https://ghcr.io/v2/zeacent/klogg-ci-deps/blobs/" + self.blob_digest
        challenge = urllib.error.HTTPError(
            blob_url, 401, "Unauthorized", {"WWW-Authenticate": (
                'Bearer realm="https://ghcr.io/token",service="ghcr.io",'
                'scope="repository:zeacent/klogg-ci-deps:pull"'
            )}, None,
        )
        token = json.dumps({"token": "public-blob-token"}).encode()
        identity, destination, transport = self.retrieve([
            self.manifest, challenge, token, self.blob
        ])
        self.assertEqual(identity["blob_digest"], self.blob_digest)
        self.assertEqual(destination.read_bytes(), self.blob)
        self.assertEqual(transport.requests[-1].get_header("Authorization"), "Bearer public-blob-token")

    def test_blob_cdn_redirect_never_receives_the_ghcr_token(self):
        token_challenge = urllib.error.HTTPError(
            "https://ghcr.io/v2/zeacent/klogg-ci-deps/manifests/" + self.manifest_digest,
            401, "Unauthorized", {"WWW-Authenticate": (
                'Bearer realm="https://ghcr.io/token",service="ghcr.io",'
                'scope="repository:zeacent/klogg-ci-deps:pull"'
            )}, None,
        )
        cdn = "https://pkg-containers.githubusercontent.com/verified-blob"
        redirect = urllib.error.HTTPError(
            "https://ghcr.io/v2/zeacent/klogg-ci-deps/blobs/" + self.blob_digest,
            307, "Redirect", {"Location": cdn}, None,
        )
        identity, destination, transport = self.retrieve([
            token_challenge, json.dumps({"token": "public-read-token"}).encode(),
            self.manifest, redirect, self.blob,
        ])
        self.assertEqual(identity["blob_digest"], self.blob_digest)
        self.assertEqual(destination.read_bytes(), self.blob)
        self.assertEqual(transport.requests[-1].full_url, cdn)
        self.assertIsNone(transport.requests[-1].get_header("Authorization"))

    def test_blob_redirect_cannot_escape_to_unapproved_hosts(self):
        for location in ("http://ghcr.io/other", "https://attacker.invalid/core", "https://ghcr.io/v2/attacker/core"):
            with self.subTest(location=location), tempfile.TemporaryDirectory() as temporary:
                redirect = urllib.error.HTTPError(
                    "https://ghcr.io/v2/zeacent/klogg-ci-deps/blobs/" + self.blob_digest,
                    307, "Redirect", {"Location": location}, None,
                )
                destination = pathlib.Path(temporary) / "native-core.tar.gz"
                with self.assertRaises(registry.DependencyRegistryError):
                    self.retrieve([self.manifest, redirect], destination=destination)
                self.assertFalse(destination.exists())

    def test_invalid_manifest_identity_is_rejected_before_registry_access(self):
        with tempfile.TemporaryDirectory() as temporary:
            transport = Transport([])
            client = registry.DependencyRegistryClient(transport)
            destination = pathlib.Path(temporary) / "native-core.tar.gz"
            for digest in ("latest", "../blobs/other", "sha256:" + "0" * 64):
                with self.subTest(digest=digest), self.assertRaises(
                    registry.DependencyRegistryError
                ):
                    client.retrieve(digest, self.blob_digest, len(self.blob),
                                    "ios-native-arm64.tar.gz", destination)
            self.assertEqual(transport.requests, [])
            self.assertFalse(destination.exists())

    def test_private_registry_and_unsafe_destination_fail_closed(self):
        private = urllib.error.HTTPError(
            "https://ghcr.io/v2/zeacent/klogg-ci-deps/manifests/" + self.manifest_digest,
            404, "Not Found", {}, None,
        )
        with self.assertRaises(registry.DependencyRegistryError):
            self.retrieve([private])
        with tempfile.TemporaryDirectory() as temporary:
            destination = pathlib.Path(temporary) / "native-core.tar.gz"
            destination.write_bytes(b"existing user data")
            with self.assertRaises(registry.DependencyRegistryError):
                self.retrieve([self.manifest, self.blob], destination=destination)
            self.assertEqual(destination.read_bytes(), b"existing user data")


class DependencyAttestationTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = pathlib.Path(self.temp.name)
        self.subject = root / "manifest.json"
        self.subject.write_bytes(b"signed dependency manifest")
        self.bundle = root / "bundle.json"
        self.bundle.write_text("{}", encoding="utf-8")
        self.source = {
            "repository": "ZEACENT/klogg", "sha": "a" * 40,
            "ref": "refs/heads/worktree-master-ci-fail",
            "workflow": ".github/workflows/ci-dependencies.yml",
            "run_id": 42, "run_attempt": 1,
        }
        self.commands = []

    def runner(self, name, *, digest=None):
        def execute(command, **kwargs):
            self.commands.append(command)
            result = [{"verificationResult": {"statement": {
                "predicateType": "https://slsa.dev/provenance/v1",
                "subject": [{"name": name, "digest": {
                    "sha256": digest or hashlib.sha256(self.subject.read_bytes()).hexdigest()
                }}],
            }}}]
            return subprocess.CompletedProcess(command, 0, json.dumps(result), "")
        return execute

    def test_dependency_attestations_pin_signer_source_and_exact_subject(self):
        self.assertTrue(registry.verify_dependency_attestation(
            self.subject, self.bundle, self.source,
            "ghcr.io/zeacent/klogg-ci-deps",
            self.runner("ghcr.io/zeacent/klogg-ci-deps"),
        ))
        command = self.commands[0]
        self.assertIn("--signer-workflow", command)
        self.assertEqual(command[command.index("--signer-workflow") + 1],
                         "ZEACENT/klogg/.github/workflows/ci-dependencies.yml")
        self.assertIn("--deny-self-hosted-runners", command)
        self.assertEqual(command[command.index("--source-digest") + 1], self.source["sha"])

    def test_dependency_attestations_reject_wrong_workflow_or_subject(self):
        wrong = dict(self.source, workflow=".github/workflows/ci-environments.yml")
        with self.assertRaises(registry.DependencyRegistryError):
            registry.verify_dependency_attestation(
                self.subject, self.bundle, wrong, "ghcr.io/zeacent/klogg-ci-deps",
                self.runner("ghcr.io/zeacent/klogg-ci-deps"),
            )
        self.assertEqual(self.commands, [])
        for name, digest in (("ghcr.io/other/pkg", None),
                             ("ghcr.io/zeacent/klogg-ci-deps", "0" * 64)):
            with self.subTest(name=name, digest=digest):
                with self.assertRaises(registry.DependencyRegistryError):
                    registry.verify_dependency_attestation(
                        self.subject, self.bundle, self.source,
                        "ghcr.io/zeacent/klogg-ci-deps",
                        self.runner(name, digest=digest),
                    )


if __name__ == "__main__":
    unittest.main()
