from __future__ import annotations

import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest
import urllib.error
from unittest import mock

ROOT = pathlib.Path(__file__).parents[2]
PREFETCH_SCRIPT = ROOT / "scripts" / "prefetch_adb_helper_sources.py"


def load_prefetch_module():
    spec = importlib.util.spec_from_file_location(
        "prefetch_adb_helper_sources", PREFETCH_SCRIPT
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc_info):
        self.close()
        return False


def http_error(code: int) -> urllib.error.HTTPError:
    return urllib.error.HTTPError(
        "https://example.invalid/archive.tar.gz",
        code,
        "status",
        {},
        io.BytesIO(b""),
    )


class PrefetchDownloadRetryTest(unittest.TestCase):
    def responder(self, responses):
        def respond(*args, **kwargs):
            response = responses.pop(0)
            if isinstance(response, BaseException):
                raise response
            return response

        return respond
    def test_transient_server_errors_are_retried_until_success(self):
        module = load_prefetch_module()
        responses = [
            http_error(503),
            http_error(502),
            FakeResponse(b"archive-bytes"),
        ]
        sleeps: list[float] = []
        with mock.patch.object(
            module.urllib.request,
            "urlopen",
            side_effect=self.responder(responses),
        ), tempfile.TemporaryDirectory() as parent:
            destination = pathlib.Path(parent) / "archive.tar.gz"
            module.download(
                "https://example.invalid/archive.tar.gz",
                destination,
                sleep=sleeps.append,
            )
            self.assertEqual(b"archive-bytes", destination.read_bytes())
        self.assertEqual([1, 2], sleeps)

    def test_persistent_server_error_fails_after_bounded_attempts(self):
        module = load_prefetch_module()
        attempts: list[int] = []

        def fail(*args, **kwargs):
            attempts.append(1)
            raise http_error(503)

        sleeps: list[float] = []
        with mock.patch.object(
            module.urllib.request, "urlopen", side_effect=fail
        ), tempfile.TemporaryDirectory() as parent:
            destination = pathlib.Path(parent) / "archive.tar.gz"
            with self.assertRaises(urllib.error.HTTPError):
                module.download(
                    "https://example.invalid/archive.tar.gz",
                    destination,
                    sleep=sleeps.append,
                )
            self.assertEqual(module.DOWNLOAD_ATTEMPTS, len(attempts))
            self.assertFalse(destination.exists())
        self.assertEqual([1, 2, 3], sleeps)

    def test_client_error_is_not_retried(self):
        module = load_prefetch_module()
        attempts: list[int] = []

        def fail(*args, **kwargs):
            attempts.append(1)
            raise http_error(404)

        sleeps: list[float] = []
        with mock.patch.object(
            module.urllib.request, "urlopen", side_effect=fail
        ), tempfile.TemporaryDirectory() as parent:
            destination = pathlib.Path(parent) / "archive.tar.gz"
            with self.assertRaises(urllib.error.HTTPError):
                module.download(
                    "https://example.invalid/archive.tar.gz",
                    destination,
                    sleep=sleeps.append,
                )
        self.assertEqual(1, len(attempts))
        self.assertEqual([], sleeps)

    def test_successful_download_leaves_no_temporary_files(self):
        module = load_prefetch_module()
        with mock.patch.object(
            module.urllib.request,
            "urlopen",
            side_effect=lambda *a, **k: FakeResponse(b"archive-bytes"),
        ), tempfile.TemporaryDirectory() as parent:
            destination = pathlib.Path(parent) / "archive.tar.gz"
            module.download(
                "https://example.invalid/archive.tar.gz",
                destination,
                sleep=self.fail,  # type: ignore[arg-type]
            )
            self.assertEqual(
                ["archive.tar.gz"],
                sorted(entry.name for entry in pathlib.Path(parent).iterdir()),
            )


if __name__ == "__main__":
    unittest.main()
