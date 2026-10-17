#!/usr/bin/env python3
"""Anonymously retrieve one digest-locked native dependency archive from GHCR."""

from __future__ import annotations

import hashlib
import http.client
import os
import pathlib
import tempfile
import urllib.error
import urllib.parse
import urllib.request

from ci_dependency_artifact import (
    ArtifactError, MAX_CORE_BYTES, inspect_manifest, require_digest,
    validate_archive_name,
)
from ci_environment_registry import (
    DEPENDENCY_REGISTRY, RegistryClient, RegistryError, _close_error,
    _verify_attestation,
)

DependencyRegistryError = RegistryError
PRODUCER_WORKFLOW = ".github/workflows/ci-dependencies.yml"
QUALIFICATION_SUBJECT = "dependency-verification.json"
CHUNK_SIZE = 1024 * 1024


def verify_dependency_attestation(subject, bundle, source, expected_name, runner=None):
    """Require the dependency producer signer, not the environment workflow."""
    return _verify_attestation(
        subject, bundle, source, expected_name, runner,
        producer_workflow=PRODUCER_WORKFLOW,
        allowed_subjects=(DEPENDENCY_REGISTRY, QUALIFICATION_SUBJECT),
    )


class DependencyRegistryClient(RegistryClient):
    """Read only the project's dependency package; never invoke Docker."""

    def __init__(self, opener=None, sleeper=None):
        super().__init__(opener, sleeper, package=DEPENDENCY_REGISTRY)

    def _stream_blob(self, url, expected_digest, expected_size, destination, *, redirects=0):
        parsed = urllib.parse.urlsplit(url)
        if (parsed.scheme != "https" or parsed.username or parsed.password
                or parsed.port not in (None, 443)
                or parsed.hostname not in ("ghcr.io", "pkg-containers.githubusercontent.com")
                or (parsed.hostname == "ghcr.io" and not parsed.path.startswith(
                    "/v2/" + self.repository_path + "/blobs/"
                ))):
            raise DependencyRegistryError("unexpected dependency blob endpoint or redirect")
        headers = {"Accept-Encoding": "identity", "User-Agent": "klogg-ci-dependency/1"}
        if self.token and parsed.hostname == "ghcr.io":
            headers["Authorization"] = "Bearer " + self.token
        request = urllib.request.Request(url, headers=headers)
        for attempt in range(4):
            try:
                with self.opener.open(request, timeout=60) as response:
                    if response.headers.get("Content-Encoding", "identity") != "identity":
                        raise DependencyRegistryError("dependency blob bytes were transformed")
                    digest = hashlib.sha256()
                    size = 0
                    output = tempfile.NamedTemporaryFile(dir=destination.parent, delete=False)
                    temporary = pathlib.Path(output.name)
                    try:
                        with output:
                            while True:
                                chunk = response.read(CHUNK_SIZE)
                                if not chunk:
                                    break
                                size += len(chunk)
                                if size > expected_size:
                                    raise DependencyRegistryError("dependency blob exceeds locked size")
                                digest.update(chunk)
                                output.write(chunk)
                        if size != expected_size or "sha256:" + digest.hexdigest() != expected_digest:
                            raise DependencyRegistryError("dependency blob digest or size mismatch")
                        if destination.exists() or destination.is_symlink():
                            raise DependencyRegistryError("dependency destination already exists")
                        # Publish atomically without replacing a file another
                        # job placed here after the initial existence check.
                        # Close the tempfile first for Windows NTFS support.
                        try:
                            os.link(temporary, destination)
                        except FileExistsError as error:
                            raise DependencyRegistryError("dependency destination already exists") from error
                    finally:
                        temporary.unlink(missing_ok=True)
                return
            except urllib.error.HTTPError as error:
                try:
                    if error.code in (301, 302, 303, 307, 308):
                        if redirects >= 3:
                            raise DependencyRegistryError("too many dependency blob redirects") from error
                        redirected = urllib.parse.urljoin(url, error.headers.get("Location", ""))
                        if redirected == url:
                            raise DependencyRegistryError("dependency blob redirect has no destination") from error
                        return self._stream_blob(
                            redirected, expected_digest, expected_size, destination,
                            redirects=redirects + 1,
                        )
                    if error.code == 401 and parsed.hostname == "ghcr.io" and not self.token:
                        self._authorize(error.headers.get("WWW-Authenticate", ""))
                        return self._stream_blob(
                            url, expected_digest, expected_size, destination,
                            redirects=redirects,
                        )
                    if error.code not in (408, 429, 500, 502, 503, 504) or attempt == 3:
                        raise DependencyRegistryError(
                            f"public dependency registry request failed (HTTP {error.code})"
                        ) from error
                finally:
                    _close_error(error)
            except (urllib.error.URLError, OSError, http.client.IncompleteRead) as error:
                if attempt == 3:
                    raise DependencyRegistryError("public dependency blob request failed") from error
            self.sleeper(2 ** attempt)
        raise DependencyRegistryError("public dependency blob transport exhausted retries")

    def retrieve(
        self, manifest_digest, blob_digest, blob_size, archive_name,
        destination: pathlib.Path,
    ) -> dict[str, object]:
        destination = pathlib.Path(destination)
        if destination.exists() or destination.is_symlink():
            raise DependencyRegistryError("dependency destination already exists")
        if destination.parent.is_symlink():
            raise DependencyRegistryError("dependency destination parent must not be a symlink")
        try:
            require_digest(manifest_digest, "manifest digest")
            require_digest(blob_digest, "archive digest")
            validate_archive_name(archive_name)
        except ArtifactError as error:
            raise DependencyRegistryError("invalid dependency artifact lock") from error
        if type(blob_size) is not int or not 0 < blob_size <= MAX_CORE_BYTES:
            raise DependencyRegistryError("invalid dependency archive size")
        base = "https://ghcr.io/v2/" + self.repository_path + "/"
        manifest = self._read(base + "manifests/" + manifest_digest)
        identity = inspect_manifest(
            manifest, manifest_digest,
            expected_blob_digest=blob_digest,
            expected_blob_size=blob_size,
            expected_archive_name=archive_name,
        )
        destination.parent.mkdir(parents=True, exist_ok=True)
        self._stream_blob(base + "blobs/" + blob_digest, blob_digest, blob_size, destination)
        return {**identity, "manifest_bytes": manifest}
