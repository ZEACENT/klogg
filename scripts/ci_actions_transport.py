"""Bounded authenticated Actions transport, separate from artifact trust policy."""

from __future__ import annotations

import os
import pathlib
import selectors
import subprocess
import tempfile
import time

import ci_environment as core

REPOSITORY = "ZEACENT/klogg"


class TransportError(ValueError):
    """An authenticated Actions metadata or archive request failed."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise TransportError(message)


def metadata(path: str) -> dict:
    try:
        result = subprocess.run(["gh", "api", path], check=True, capture_output=True,
                                timeout=30)
    except (OSError, subprocess.SubprocessError) as error:
        raise TransportError("authenticated Actions metadata request failed") from error
    _require(len(result.stdout) <= core.MAX_METADATA_BYTES, "oversized Actions metadata")
    try:
        document = core._parse_json(result.stdout, "source cache Actions metadata")
    except ValueError as error:
        raise TransportError("invalid Actions metadata") from error
    _require(isinstance(document, dict), "Actions metadata must be an object")
    return document


def download_artifact_zip(artifact_id: int, archive: pathlib.Path, *,
                          limit: int, timeout: int) -> None:
    """Stream raw binary archive bytes; the caller authenticates and hashes them."""
    _require(isinstance(os.environ.get("GH_TOKEN"), str) and os.environ["GH_TOKEN"],
             "Actions read token is missing")
    command = ["gh", "api", "--allow-escape-sequences",
               "repos/" + REPOSITORY + "/actions/artifacts/{}/zip".format(artifact_id)]
    deadline = time.monotonic() + timeout
    with tempfile.TemporaryFile() as errors, pathlib.Path(archive).open("xb") as output:
        try:
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=errors)
        except OSError as error:
            raise TransportError("could not start authenticated artifact download") from error
        try:
            total = 0
            with selectors.DefaultSelector() as watcher:
                watcher.register(process.stdout, selectors.EVENT_READ)
                while True:
                    remaining = deadline - time.monotonic()
                    _require(remaining > 0, "source cache ZIP download exceeded deadline")
                    _require(watcher.select(remaining), "source cache ZIP download exceeded deadline")
                    chunk = os.read(process.stdout.fileno(), 65536)
                    if not chunk:
                        break
                    total += len(chunk)
                    _require(total <= limit, "source cache ZIP download exceeds byte limit")
                    output.write(chunk)
            remaining = deadline - time.monotonic()
            _require(remaining > 0, "source cache ZIP download exceeded deadline")
            _require(process.wait(timeout=remaining) == 0 and total > 0,
                     "authenticated source cache ZIP download failed")
        except (OSError, subprocess.SubprocessError) as error:
            raise TransportError("authenticated source cache ZIP download failed") from error
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
            process.stdout.close()
