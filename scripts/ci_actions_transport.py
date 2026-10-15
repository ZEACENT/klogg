"""Bounded authenticated Actions transport, separate from artifact trust policy."""

from __future__ import annotations

import os
import pathlib
import re
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


def _endpoint_context(path: str) -> str:
    # Do not echo arbitrary paths, repository names, queries or signed URLs.
    prefix = "repos/" + REPOSITORY + "/actions/"
    if not isinstance(path, str) or not path.startswith(prefix):
        return "endpoint=other"
    suffix = path[len(prefix):].partition("?")[0]
    artifact = re.fullmatch(r"artifacts/([1-9][0-9]{0,19})", suffix)
    if artifact:
        return "endpoint=artifact artifact=" + artifact[1]
    run = re.fullmatch(r"runs/([1-9][0-9]{0,19})(?:/attempts/([1-9][0-9]{0,9})(/jobs)?)?", suffix)
    if run:
        if run[2]:
            endpoint = "run-attempt-jobs" if run[3] else "run-attempt"
            return "endpoint={} run={} attempt={}".format(endpoint, run[1], run[2])
        return "endpoint=run run=" + run[1]
    artifacts = re.fullmatch(r"runs/([1-9][0-9]{0,19})/artifacts", suffix)
    if artifacts:
        return "endpoint=run-artifacts run=" + artifacts[1]
    return "endpoint=runs" if suffix == "runs" else "endpoint=other"


def _http_status(stderr) -> str:
    # Only a single canonical gh status trailer is diagnostic evidence. Never
    # publish its message/body; ambiguous, oversized or non-ASCII data is opaque.
    if not isinstance(stderr, (bytes, str)) or len(stderr) > 4096:
        return "unknown"
    try:
        text = stderr.decode("ascii") if isinstance(stderr, bytes) else stderr
        text.encode("ascii")
    except UnicodeError:
        return "unknown"
    lines = [line for line in text.splitlines() if line]
    if not lines or len(re.findall(r"\(HTTP [1-5][0-9]{2}\)", text)) != 1:
        return "unknown"
    status = re.fullmatch(r"gh: [^\r\n]* \(HTTP ([1-5][0-9]{2})\)", lines[-1])
    return status[1] if status else "unknown"


def _metadata_failure(path: str, error: BaseException) -> str:
    fields = [_endpoint_context(path)]
    http = "unknown"
    if isinstance(error, subprocess.CalledProcessError):
        fields.append("failure=exit")
        code = error.returncode
        fields.append("exit=" + (str(code) if type(code) is int and -255 <= code <= 255 else "unknown"))
        http = _http_status(error.stderr)
    elif isinstance(error, subprocess.TimeoutExpired):
        fields.append("failure=timeout")
    elif isinstance(error, OSError):
        fields.append("failure=os-error")
        code = error.errno
        fields.append("errno=" + (str(code) if type(code) is int and 0 <= code <= 65535 else "unknown"))
    else:
        fields.append("failure=subprocess")
    fields.append("http=" + http)
    return "authenticated Actions metadata request failed [" + " ".join(fields) + "]"


def metadata(path: str) -> dict:
    try:
        result = subprocess.run(["gh", "api", path], check=True, capture_output=True,
                                timeout=30)
    except (OSError, subprocess.SubprocessError) as error:
        raise TransportError(_metadata_failure(path, error)) from error
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
