from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import pathlib
import subprocess
import tempfile
import unittest
import urllib.error

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "ci_environment_registry", ROOT / "scripts/ci_environment_registry.py"
)
registry = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(registry)


def digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def encode(value):
    return json.dumps(value, separators=(",", ":")).encode()


class Response(io.BytesIO):
    def __init__(self, data, headers=None):
        super().__init__(data)
        self.headers = headers or {}


class Transport:
    def __init__(self, responses):
        self.responses = list(responses)
        self.requests = []

    def open(self, request, timeout):
        self.requests.append((request, timeout))
        if not self.responses:
            raise AssertionError("unexpected network request")
        response = self.responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return Response(response)


def image_fixture(config_changes=None):
    layer = b"compressed layer fixture"
    config = {
        "architecture": "amd64", "os": "linux",
        "rootfs": {"type": "layers", "diff_ids": [digest(b"uncompressed layer")]},
    }
    config.update(config_changes or {})
    config_bytes = encode(config)
    manifest = {
        "schemaVersion": 2,
        "mediaType": "application/vnd.oci.image.manifest.v1+json",
        "config": {
            "mediaType": "application/vnd.oci.image.config.v1+json",
            "digest": digest(config_bytes), "size": len(config_bytes),
        },
        "layers": [{
            "mediaType": "application/vnd.oci.image.layer.v1.tar+gzip",
            "digest": digest(layer), "size": len(layer),
        }],
    }
    return encode(manifest), config_bytes


class RegistryImageTest(unittest.TestCase):
    def test_anonymous_manifest_and_config_have_locked_identity(self):
        manifest, config = image_fixture()
        transport = Transport([manifest, config])
        result = registry.RegistryClient(transport).read_image(digest(manifest))
        self.assertEqual(result.get("manifest_digest"), digest(manifest))
        self.assertEqual(result.get("config_digest"), digest(config))
        self.assertEqual(result.get("platform"), "linux/amd64")
        self.assertEqual(result.get("manifest_bytes"), manifest)
        self.assertEqual(len(transport.requests), 2)
        for request, timeout in transport.requests:
            self.assertTrue(request.full_url.startswith("https://ghcr.io/v2/zeacent/klogg-ci-env/"))
            self.assertIsNone(request.get_header("Authorization"))
            self.assertGreater(timeout, 0)
            self.assertLessEqual(timeout, 30)

    def test_only_an_anonymous_pull_token_is_requested_after_challenge(self):
        manifest, config = image_fixture()
        challenge = urllib.error.HTTPError(
            "https://ghcr.io/v2/zeacent/klogg-ci-env/manifests/" + digest(manifest),
            401, "Unauthorized", {"WWW-Authenticate": 'Bearer realm="https://ghcr.io/token",service="ghcr.io",scope="repository:zeacent/klogg-ci-env:pull"'}, None,
        )
        transport = Transport([challenge, encode({"token": "anonymous-token"}), manifest, config])
        registry.RegistryClient(transport).read_image(digest(manifest))
        self.assertEqual(len(transport.requests), 4)
        token_request = transport.requests[1][0]
        self.assertTrue(token_request.full_url.startswith("https://ghcr.io/token?"))
        self.assertIn("repository%3Azeacent%2Fklogg-ci-env%3Apull", token_request.full_url)
        self.assertIsNone(token_request.get_header("Authorization"))
        self.assertEqual(transport.requests[2][0].get_header("Authorization"), "Bearer anonymous-token")

    def test_transient_registry_failures_retry_only_bounded_transport(self):
        manifest, config = image_fixture()
        failures = [urllib.error.HTTPError("https://ghcr.io/", code, "temporary", {}, None)
                    for code in (503, 429)]
        transport = Transport(failures + [manifest, config])
        sleeps = []
        client = registry.RegistryClient(transport)
        client.sleeper = sleeps.append
        self.assertEqual(client.read_image(digest(manifest))["manifest_digest"], digest(manifest))
        self.assertEqual(sleeps, [1, 2])
        self.assertEqual(len(transport.requests), 4)

    def test_persistent_transport_failure_has_a_fixed_attempt_limit(self):
        manifest, _ = image_fixture()
        transport = Transport([urllib.error.URLError("connection timed out") for _ in range(4)])
        sleeps = []
        client = registry.RegistryClient(transport)
        client.sleeper = sleeps.append
        with self.assertRaises(registry.RegistryError):
            client.read_image(digest(manifest))
        self.assertEqual(len(transport.requests), 4)
        self.assertEqual(sleeps, [1, 2, 4])

    def test_private_missing_or_tampered_images_do_not_retry(self):
        manifest, _ = image_fixture()
        for result in (
            urllib.error.HTTPError("https://ghcr.io/", 403, "private", {}, None),
            urllib.error.HTTPError("https://ghcr.io/", 404, "missing", {}, None),
            manifest + b" ",
        ):
            with self.subTest(result=type(result).__name__):
                transport = Transport([result])
                sleeps = []
                client = registry.RegistryClient(transport)
                client.sleeper = sleeps.append
                with self.assertRaises(registry.RegistryError):
                    client.read_image(digest(manifest))
                self.assertEqual(len(transport.requests), 1)
                self.assertEqual(sleeps, [])

    def test_tampered_manifest_or_config_is_rejected(self):
        manifest, config = image_fixture()
        cases = ([manifest + b" ", config], [manifest, config + b" "])
        for responses in cases:
            with self.subTest(responses=responses):
                with self.assertRaises(registry.RegistryError):
                    registry.RegistryClient(Transport(responses)).read_image(digest(manifest))

    def test_mutable_references_and_placeholders_never_reach_network(self):
        for reference in ("latest", "sha256:" + "0" * 64, "sha256:abc", "sha256:" + "A" * 64, "../other", "sha256:" + "a" * 64 + "?tag=latest"):
            with self.subTest(reference=reference):
                transport = Transport([])
                with self.assertRaises(registry.RegistryError):
                    registry.RegistryClient(transport).read_image(reference)
                self.assertEqual(transport.requests, [])

    def test_other_architecture_or_invalid_rootfs_is_rejected(self):
        for changes in ({"architecture": "arm64"}, {"os": "windows"}, {"rootfs": {"type": "layers", "diff_ids": []}}):
            with self.subTest(changes=changes):
                manifest, config = image_fixture(changes)
                with self.assertRaises(registry.RegistryError):
                    registry.RegistryClient(Transport([manifest, config])).read_image(digest(manifest))

    def test_manifest_index_and_foreign_layer_urls_are_rejected(self):
        manifest, config = image_fixture()
        for change in ("index", "foreign-url", "bad-size"):
            with self.subTest(change=change):
                document = json.loads(manifest)
                if change == "index":
                    document["mediaType"] = "application/vnd.oci.image.index.v1+json"
                elif change == "foreign-url":
                    document["layers"][0]["urls"] = ["https://example.invalid/layer"]
                else:
                    document["config"]["size"] = True
                modified = encode(document)
                with self.assertRaises(registry.RegistryError):
                    registry.RegistryClient(Transport([modified, config])).read_image(digest(modified))

    def test_malformed_descriptor_types_are_contract_errors(self):
        manifest, config = image_fixture()
        for field, value in (("mediaType", []), ("mediaType", {}), ("digest", []), ("size", "151")):
            with self.subTest(field=field, value=value):
                document = json.loads(manifest)
                document["config"][field] = value
                modified = encode(document)
                with self.assertRaises(registry.RegistryError):
                    registry.RegistryClient(Transport([modified, config])).read_image(digest(modified))

    def test_authentication_cannot_redirect_to_another_token_issuer(self):
        manifest, _ = image_fixture()
        error = urllib.error.HTTPError("https://ghcr.io/", 401, "Unauthorized", {
            "WWW-Authenticate": 'Bearer realm="https://example.invalid/token",service="ghcr.io"'
        }, None)
        transport = Transport([error])
        with self.assertRaises(registry.RegistryError):
            registry.RegistryClient(transport).read_image(digest(manifest))
        self.assertEqual(len(transport.requests), 1)

    def test_redirect_never_leaks_authorization_or_accepts_unknown_hosts(self):
        manifest, config = image_fixture()
        redirect = urllib.error.HTTPError("https://ghcr.io/", 307, "Redirect", {
            "Location": "https://pkg-containers.githubusercontent.com/ghcr1/blobs/config"
        }, None)
        transport = Transport([manifest, redirect, config])
        client = registry.RegistryClient(transport)
        client.token = "anonymous-token"
        result = client.read_image(digest(manifest))
        self.assertEqual(result.get("config_digest"), digest(config))
        self.assertIsNone(transport.requests[-1][0].get_header("Authorization"))
        for url in ("http://pkg-containers.githubusercontent.com/blob", "https://example.invalid/blob", "https://ghcr.io@example.invalid/blob"):
            with self.subTest(url=url):
                error = urllib.error.HTTPError("https://ghcr.io/", 302, "Redirect", {"Location": url}, None)
                with self.assertRaises(registry.RegistryError):
                    registry.RegistryClient(Transport([error])).read_image(digest(manifest))

    def test_duplicate_json_and_oversized_metadata_fail_closed(self):
        for data in (b'{"schemaVersion":1,"schemaVersion":2}', b" " * (2 * 1024 * 1024 + 1)):
            with self.subTest(size=len(data)):
                with self.assertRaises(registry.RegistryError):
                    registry.RegistryClient(Transport([data])).read_image(digest(data))


class DetachedAttestationTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.subject = self.root / "manifest.json"
        self.subject.write_bytes(b"signed subject bytes")
        self.bundle = self.root / "bundle.json"
        self.bundle.write_text("{}", encoding="utf-8")
        self.source = {
            "repository": "ZEACENT/klogg", "sha": "1" * 40,
            "ref": "refs/heads/worktree-master-ci-fail",
            "workflow": ".github/workflows/ci-environments.yml",
            "run_id": 123, "run_attempt": 1,
        }
        self.commands = []

    def output(self, name=registry.REGISTRY, sha=None):
        return [{"verificationResult": {"statement": {
            "predicateType": "https://slsa.dev/provenance/v1",
            "subject": [{"name": name, "digest": {"sha256": sha or digest(self.subject.read_bytes())[7:]}}],
        }}}]

    def runner(self, output=None, fail=False):
        def run(command, **kwargs):
            self.commands.append((command, kwargs))
            if fail:
                raise subprocess.CalledProcessError(1, command, stderr="signature verification failed")
            return subprocess.CompletedProcess(command, 0, json.dumps(self.output() if output is None else output), "")
        return run

    def test_verification_is_local_file_based_and_pins_signer_and_source(self):
        self.assertTrue(registry.verify_attestation(self.subject, self.bundle, self.source, registry.REGISTRY, self.runner()))
        command, options = self.commands[0]
        self.assertEqual(command[:4], ["gh", "attestation", "verify", str(self.subject)])
        self.assertNotIn("oci://", " ".join(command))
        for flag, value in (
            ("--bundle", str(self.bundle)), ("--repo", "ZEACENT/klogg"),
            ("--signer-workflow", "ZEACENT/klogg/.github/workflows/ci-environments.yml"),
            ("--signer-digest", self.source["sha"]), ("--source-digest", self.source["sha"]),
            ("--source-ref", self.source["ref"]), ("--format", "json"),
        ):
            self.assertIn(flag, command)
            self.assertEqual(command[command.index(flag) + 1], value)
        self.assertIn("--deny-self-hosted-runners", command)
        self.assertTrue(options["check"])
        self.assertGreater(options["timeout"], 0)

    def test_cli_success_without_exact_verified_subject_is_not_success(self):
        outputs = ([], [{}], self.output(name="ghcr.io/other/image"), self.output(sha="2" * 64))
        for output in outputs:
            with self.subTest(output=output):
                with self.assertRaises(registry.RegistryError):
                    registry.verify_attestation(self.subject, self.bundle, self.source, registry.REGISTRY, self.runner(output))

    def test_failed_signature_verification_is_not_swallowed(self):
        with self.assertRaises(registry.RegistryError):
            registry.verify_attestation(self.subject, self.bundle, self.source, registry.REGISTRY, self.runner(fail=True))

    def test_untrusted_source_identity_is_rejected_before_cli_execution(self):
        for field, value in (("repository", "someone/klogg"), ("workflow", ".github/workflows/arbitrary.yml"), ("sha", "0" * 40), ("ref", "refs/pull/77/merge")):
            with self.subTest(field=field):
                source = dict(self.source, **{field: value})
                with self.assertRaises(registry.RegistryError):
                    registry.verify_attestation(self.subject, self.bundle, source, registry.REGISTRY, self.runner())
        self.assertEqual(self.commands, [])


if __name__ == "__main__":
    unittest.main()
