#!/usr/bin/env python3
"""Anonymous OCI metadata access and detached environment provenance checks.

This module never loads credentials from Docker configuration or a personal
access token. Registry data is untrusted until its exact bytes and detached
provenance have been checked. Signature verification belongs to GitHub CLI;
checking a JSON receipt alone is not signature verification.
"""

from __future__ import annotations

import hashlib
import http.client
import json
import pathlib
import re
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request

REGISTRY = "ghcr.io/zeacent/klogg-ci-env"
REPOSITORY = "ZEACENT/klogg"
PRODUCER_WORKFLOW = ".github/workflows/ci-environments.yml"
MANIFEST_TYPE = "application/vnd.oci.image.manifest.v1+json"
CONFIG_TYPE = "application/vnd.oci.image.config.v1+json"
LAYER_TYPES = {
    "application/vnd.oci.image.layer.v1.tar",
    "application/vnd.oci.image.layer.v1.tar+gzip",
    "application/vnd.oci.image.layer.v1.tar+zstd",
}
METADATA_LIMIT = 2 * 1024 * 1024
BUNDLE_LIMIT = 10 * 1024 * 1024
DIGEST_RE = re.compile(r"sha256:[0-9a-f]{64}\Z")
COMMIT_RE = re.compile(r"[0-9a-f]{40}\Z")
REF_RE = re.compile(r"refs/heads/[A-Za-z0-9][A-Za-z0-9._/-]*\Z")


class RegistryError(RuntimeError):
    """A published environment could not be verified."""


def _digest(data):
    return "sha256:" + hashlib.sha256(data).hexdigest()


def _require_digest(value):
    if not isinstance(value, str) or not DIGEST_RE.fullmatch(value) or value == "sha256:" + "0" * 64:
        raise RegistryError("an exact non-placeholder SHA-256 digest is required")
    return value


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise RegistryError("duplicate JSON object key")
        result[key] = value
    return result


def _invalid_constant(value):
    raise RegistryError("non-finite JSON constant: " + value)


def _json(data):
    try:
        return json.loads(data, object_pairs_hook=_unique_object, parse_constant=_invalid_constant)
    except (ValueError, UnicodeError) as error:
        raise RegistryError("invalid JSON metadata") from error


def _descriptor(value, media_types):
    if (not isinstance(value, dict) or not isinstance(value.get("mediaType"), str)
            or value["mediaType"] not in media_types):
        raise RegistryError("unsupported OCI descriptor")
    _require_digest(value.get("digest"))
    size = value.get("size")
    if type(size) is not int or size < 0 or size > 20 * 1024**3:
        raise RegistryError("invalid OCI descriptor size")
    if "urls" in value or "data" in value:
        raise RegistryError("external or embedded OCI descriptor data is not supported")
    return value


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, new_url):
        return None


def _close_error(error):
    """Close an HTTP error body if it has one.

    Real urlopen errors carry a response file; synthesized test errors and some
    Python versions have none, and close() must never mask the real failure.
    """
    try:
        error.close()
    except Exception:
        pass


class RegistryClient:
    """Fetch only public metadata from the project's one environment package."""

    def __init__(self, opener=None, sleeper=None):
        self.opener = opener or urllib.request.build_opener(_NoRedirect())
        self.sleeper = sleeper or time.sleep
        self.token = None

    def _open_bytes(self, request):
        # Retry transport failures only. Missing/private images, bad identities
        # and invalid evidence need operator action, not a different artifact.
        for attempt in range(4):
            try:
                with self.opener.open(request, timeout=30) as response:
                    if response.headers.get("Content-Encoding", "identity") != "identity":
                        raise RegistryError("registry metadata must preserve raw bytes")
                    data = response.read(METADATA_LIMIT + 1)
                    if len(data) > METADATA_LIMIT:
                        raise RegistryError("registry metadata exceeds size limit")
                    return data
            except urllib.error.HTTPError as error:
                if error.code not in (408, 429, 500, 502, 503, 504) or attempt == 3:
                    raise
                _close_error(error)
            except (urllib.error.URLError, OSError, http.client.IncompleteRead):
                if attempt == 3:
                    raise
            self.sleeper(2 ** attempt)
        raise RegistryError("registry transport exhausted its bounded attempts")

    def _read(self, url, *, token_request=False, redirects=0):
        parsed = urllib.parse.urlsplit(url)
        if (parsed.scheme != "https" or parsed.username or parsed.password
                or parsed.port not in (None, 443)
                or parsed.hostname not in ("ghcr.io", "pkg-containers.githubusercontent.com")):
            raise RegistryError("unexpected registry endpoint or redirect")
        headers = {"Accept": MANIFEST_TYPE, "Accept-Encoding": "identity",
                   "User-Agent": "klogg-ci-environment/1"}
        if self.token and parsed.hostname == "ghcr.io" and not token_request:
            headers["Authorization"] = "Bearer " + self.token
        request = urllib.request.Request(url, headers=headers)
        try:
            return self._open_bytes(request)
        except urllib.error.HTTPError as error:
            try:
                if error.code in (301, 302, 303, 307, 308):
                    if token_request or redirects >= 3:
                        raise RegistryError("unexpected or excessive registry redirects") from error
                    destination = urllib.parse.urljoin(url, error.headers.get("Location", ""))
                    if destination == url:
                        raise RegistryError("registry redirect has no destination") from error
                    return self._read(destination, redirects=redirects + 1)
                if (error.code == 401 and parsed.hostname == "ghcr.io"
                        and not self.token and not token_request):
                    challenge = error.headers.get("WWW-Authenticate", "")
                    realm = re.search(r'\brealm="([^"]+)"', challenge)
                    service = re.search(r'\bservice="([^"]+)"', challenge)
                    scope = re.search(r'\bscope="([^"]+)"', challenge)
                    if (not challenge.lower().startswith("bearer ") or not realm or not service
                            or realm.group(1) != "https://ghcr.io/token"
                            or service.group(1) != "ghcr.io"
                            or (scope and scope.group(1) != "repository:zeacent/klogg-ci-env:pull")):
                        raise RegistryError("unexpected registry authentication challenge") from error
                    query = urllib.parse.urlencode({
                        "service": "ghcr.io", "scope": "repository:zeacent/klogg-ci-env:pull",
                    })
                    document = _json(self._read("https://ghcr.io/token?" + query, token_request=True))
                    token = document.get("token") if isinstance(document, dict) else None
                    if not isinstance(token, str) or not token or len(token) > 16384 or any(c.isspace() for c in token):
                        raise RegistryError("invalid anonymous registry token")
                    self.token = token
                    return self._read(url, redirects=redirects)
                raise RegistryError("public registry request failed (HTTP {})".format(error.code)) from error
            finally:
                _close_error(error)
        except (urllib.error.URLError, OSError, http.client.IncompleteRead) as error:
            raise RegistryError("public registry request failed") from error

    def read_image(self, digest):
        _require_digest(digest)
        base = "https://ghcr.io/v2/zeacent/klogg-ci-env/"
        manifest_bytes = self._read(base + "manifests/" + digest)
        if _digest(manifest_bytes) != digest:
            raise RegistryError("registry manifest digest mismatch")
        manifest = _json(manifest_bytes)
        if (not isinstance(manifest, dict) or type(manifest.get("schemaVersion")) is not int
                or manifest.get("schemaVersion") != 2 or manifest.get("mediaType") != MANIFEST_TYPE
                or "artifactType" in manifest or "subject" in manifest):
            raise RegistryError("expected one OCI image manifest, not an index or artifact")
        config_descriptor = _descriptor(manifest.get("config"), {CONFIG_TYPE})
        if config_descriptor["size"] > METADATA_LIMIT:
            raise RegistryError("image configuration exceeds metadata limit")
        layers = manifest.get("layers")
        if not isinstance(layers, list) or not layers or len(layers) > 256:
            raise RegistryError("invalid OCI image layer list")
        for layer in layers:
            _descriptor(layer, LAYER_TYPES)
        config_bytes = self._read(base + "blobs/" + config_descriptor["digest"])
        if len(config_bytes) != config_descriptor["size"] or _digest(config_bytes) != config_descriptor["digest"]:
            raise RegistryError("registry image configuration digest or size mismatch")
        config = _json(config_bytes)
        if not isinstance(config, dict) or config.get("os") != "linux" or config.get("architecture") != "amd64":
            raise RegistryError("environment image must be linux/amd64")
        rootfs = config.get("rootfs")
        diff_ids = rootfs.get("diff_ids") if isinstance(rootfs, dict) else None
        if (not isinstance(rootfs, dict) or rootfs.get("type") != "layers"
                or not isinstance(diff_ids, list) or len(diff_ids) != len(layers)):
            raise RegistryError("image rootfs does not match its layer descriptors")
        for diff_id in diff_ids:
            _require_digest(diff_id)
        return {
            "manifest_digest": digest, "config_digest": config_descriptor["digest"],
            "platform": "linux/amd64", "diff_ids": diff_ids,
            "layer_digests": [layer["digest"] for layer in layers],
            "manifest_bytes": manifest_bytes,
        }


def verify_attestation(subject, bundle, source, expected_name, runner=None):
    """Verify cryptography/issuer through gh, then enforce the signed subject.

    These flags bind the certificate-backed signer/source identity. They must
    not be replaced with comparisons against self-reported receipt fields.
    """
    if (not isinstance(source, dict) or source.get("repository") != REPOSITORY
            or source.get("workflow") != PRODUCER_WORKFLOW):
        raise RegistryError("untrusted environment producer identity")
    sha, ref = source.get("sha"), source.get("ref")
    if not isinstance(sha, str) or not COMMIT_RE.fullmatch(sha) or sha == "0" * 40:
        raise RegistryError("invalid producer source commit")
    if (not isinstance(ref, str) or not REF_RE.fullmatch(ref) or ".." in ref
            or "//" in ref or ref.endswith(("/", "."))):
        raise RegistryError("producer provenance must name a trusted branch revision")
    if expected_name not in (REGISTRY, "verification.json"):
        raise RegistryError("unexpected attestation subject policy")
    subject, bundle = pathlib.Path(subject), pathlib.Path(bundle)
    for path, limit in ((subject, METADATA_LIMIT), (bundle, BUNDLE_LIMIT)):
        if path.is_symlink() or not path.is_file() or path.stat().st_size > limit:
            raise RegistryError("missing, unsupported or oversized attestation material")
    subject_digest = _digest(subject.read_bytes())[7:]
    command = [
        "gh", "attestation", "verify", str(subject), "--bundle", str(bundle),
        "--repo", REPOSITORY, "--signer-workflow", REPOSITORY + "/" + PRODUCER_WORKFLOW,
        "--signer-digest", sha, "--source-digest", sha, "--source-ref", ref,
        "--deny-self-hosted-runners", "--format", "json",
    ]
    try:
        result = (runner or subprocess.run)(command, check=True, capture_output=True, text=True, timeout=90)
    except (subprocess.SubprocessError, OSError) as error:
        raise RegistryError("detached attestation signature verification failed") from error
    if len(result.stdout) > BUNDLE_LIMIT:
        raise RegistryError("attestation verifier output exceeds size limit")
    verified = _json(result.stdout)
    if not isinstance(verified, list) or not verified:
        raise RegistryError("no verified attestation was returned")
    for record in verified:
        if not isinstance(record, dict):
            continue
        verification = record.get("verificationResult")
        statement = verification.get("statement") if isinstance(verification, dict) else None
        if not isinstance(statement, dict) or statement.get("predicateType") != "https://slsa.dev/provenance/v1":
            continue
        subjects = statement.get("subject")
        if not isinstance(subjects, list):
            continue
        if any(isinstance(item, dict) and item.get("name") == expected_name
               and item.get("digest") == {"sha256": subject_digest} for item in subjects):
            return True
    raise RegistryError("verified attestation does not bind the exact expected subject")
