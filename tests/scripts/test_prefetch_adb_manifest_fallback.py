"""The AOSP manifest fallback can only reproduce the locked source archive."""

from __future__ import annotations

import base64
import gzip
import hashlib
import io
import json
import pathlib
import sys
import tarfile
import tempfile
import unittest
import urllib.error
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import prefetch_adb_manifest_fallback as fallback


class ManifestFallbackTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.downloads = self.root / "downloads"
        self.commit = "a" * 40
        self.files = {
            "GLOBAL-PREUPLOAD.cfg": b"G" * 850,
            "default.xml": b"D" * 111646,
        }
        self.archive_bytes = self.canonical_archive(self.files)
        self.policy = {
            "commit": self.commit,
            "archive_file": "aosp-manifest-" + self.commit + ".tar.gz",
            "sha256": hashlib.sha256(self.archive_bytes).hexdigest(),
            "files": {
                name: {
                    "size": len(data),
                    "git_blob": hashlib.sha1(
                        b"blob " + str(len(data)).encode() + b"\0" + data
                    ).hexdigest(),
                }
                for name, data in self.files.items()
            },
        }
        self.url = (
            "https://android.googlesource.com/platform/manifest/+archive/"
            + self.commit + ".tar.gz"
        )
        self.lock = self.root / "lock.json"
        self.record = {
            "id": "aosp-manifest",
            "commit": self.commit,
            "repository_url": "https://android.googlesource.com/platform/manifest",
            "archive_file": self.policy["archive_file"],
            "archive_url": self.url,
            "archive_sha256": self.policy["sha256"],
            "archive_identity": "canonical-tar-gz-v1",
        }
        self.write_lock()

    def write_lock(self):
        self.lock.write_text(json.dumps({"sources": [self.record]}), encoding="utf-8")

    @staticmethod
    def canonical_archive(files):
        output = io.BytesIO()
        with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode="w", format=tarfile.PAX_FORMAT) as tar:
                for name, data in sorted(files.items()):
                    member = tarfile.TarInfo(name)
                    member.mode = 0o644
                    member.mtime = member.uid = member.gid = 0
                    member.uname = member.gname = "root"
                    member.size = len(data)
                    tar.addfile(member, io.BytesIO(data))
        return output.getvalue()

    def blobs(self):
        prefix = (
            "https://android.googlesource.com/platform/manifest/+/"
            + self.commit + "/"
        )
        return {prefix + name + "?format=TEXT": data
                for name, data in self.files.items()}

    def fallback_from_503(self, blobs=None):
        failure = urllib.error.HTTPError(self.url, 503, "unavailable", {}, io.BytesIO())
        self.addCleanup(failure.close)
        downloader = mock.Mock(side_effect=failure)
        responses = self.blobs() if blobs is None else blobs
        fetch = mock.Mock(side_effect=lambda url: responses[url])
        destination = fallback.prefetch_manifest(
            self.lock, self.downloads, policy=self.policy,
            downloader=downloader, fetch=fetch,
        )
        downloader.assert_called_once()
        return destination, fetch

    def test_terminal_archive_503_reconstructs_exact_locked_tar(self):
        destination, fetch = self.fallback_from_503()
        self.assertEqual(destination.read_bytes(), self.archive_bytes)
        self.assertEqual(hashlib.sha256(destination.read_bytes()).hexdigest(),
                         self.policy["sha256"])
        self.assertEqual(fetch.call_count, 2)
        self.assertEqual(list(self.downloads.iterdir()), [destination])
        with tarfile.open(destination, "r:gz") as archive:
            self.assertEqual([member.name for member in archive], sorted(self.files))

    def test_successful_original_archive_never_uses_fallback(self):
        fetch = mock.Mock(side_effect=AssertionError("unexpected Gitiles fallback"))
        downloader = mock.Mock(side_effect=lambda url, path: path.write_bytes(self.archive_bytes))
        destination = fallback.prefetch_manifest(
            self.lock, self.downloads, policy=self.policy,
            downloader=downloader, fetch=fetch,
        )
        self.assertEqual(destination.read_bytes(), self.archive_bytes)
        fetch.assert_not_called()

    def test_source_commit_and_repository_must_match_reviewed_lock(self):
        for field, value in (("commit", "b" * 40),
                             ("repository_url", "https://unreviewed.example/manifest")):
            with self.subTest(field=field):
                self.record[field] = value
                self.write_lock()
                with self.assertRaisesRegex(RuntimeError, "unreviewed locked"):
                    fallback.prefetch_manifest(
                        self.lock, self.downloads, policy=self.policy,
                        downloader=mock.Mock(side_effect=AssertionError("must not fetch")),
                    )
                self.record[field] = (self.commit if field == "commit" else
                                      "https://android.googlesource.com/platform/manifest")

    def test_oversized_original_response_stops_before_writing_excess(self):
        class Response(io.BytesIO):
            def __exit__(self, *args):
                return False

        response = Response(b"A" * (fallback.MAX_ARCHIVE_BYTES * 4))
        with mock.patch.object(fallback.urllib.request, "urlopen", return_value=response):
            with self.assertRaisesRegex(RuntimeError, "oversized"):
                fallback.prefetch_manifest(self.lock, self.downloads, policy=self.policy)
        self.assertLessEqual(response.tell(), fallback.MAX_ARCHIVE_BYTES + 1)
        self.assertEqual(list(self.downloads.iterdir()), [])

    def test_original_archive_503_retries_then_stops_before_fallback(self):
        calls = []
        sleeps = []

        def unavailable(*args, **kwargs):
            calls.append(1)
            raise urllib.error.HTTPError(self.url, 503, "unavailable", {}, io.BytesIO())

        with mock.patch.object(fallback.urllib.request, "urlopen", side_effect=unavailable):
            with self.assertRaises(urllib.error.HTTPError):
                fallback.download_manifest(
                    self.url, self.downloads / "manifest.tar.gz", sleep=sleeps.append,
                )
        self.assertEqual(len(calls), fallback.DOWNLOAD_ATTEMPTS)
        self.assertEqual(sleeps, list(range(1, fallback.DOWNLOAD_ATTEMPTS)))

    def test_archive_404_or_wrong_bytes_cannot_trigger_fallback(self):
        fetch = mock.Mock(side_effect=AssertionError("unexpected Gitiles fallback"))
        for failure in (
            urllib.error.HTTPError(self.url, 404, "missing", {}, io.BytesIO()),
            None,
        ):
            with self.subTest(failure=failure):
                if failure is None:
                    downloader = lambda url, path: path.write_bytes(b"wrong archive")
                else:
                    self.addCleanup(failure.close)
                    downloader = mock.Mock(side_effect=failure)
                with self.assertRaises(Exception):
                    fallback.prefetch_manifest(
                        self.lock, self.downloads, policy=self.policy,
                        downloader=downloader, fetch=fetch,
                    )
                self.assertFalse((self.downloads / self.policy["archive_file"]).exists())
        fetch.assert_not_called()

    def test_bad_blob_bytes_size_or_identity_never_publish_archive(self):
        for mutation in (b"wrong", b"D" * 111645 + b"E"):
            with self.subTest(mutation=mutation[:16]):
                blobs = self.blobs()
                blobs[next(url for url in blobs if "/default.xml?" in url)] = mutation
                with self.assertRaises(Exception):
                    self.fallback_from_503(blobs)
                self.assertFalse((self.downloads / self.policy["archive_file"]).exists())
                self.assertEqual(list(self.downloads.iterdir()), [])

    def test_wrong_blob_identity_or_lock_url_fails_before_publication(self):
        self.policy["files"]["default.xml"]["git_blob"] = "0" * 40
        with self.assertRaises(Exception):
            self.fallback_from_503()
        self.assertFalse((self.downloads / self.policy["archive_file"]).exists())
        self.record["download_url"] = "https://unreviewed.example/source.tar.gz"
        self.write_lock()
        with self.assertRaises(Exception):
            fallback.prefetch_manifest(
                self.lock, self.downloads, policy=self.policy,
                downloader=mock.Mock(side_effect=AssertionError("must not fetch")),
                fetch=mock.Mock(side_effect=AssertionError("must not fetch")),
            )

    def test_gitiles_text_must_be_strict_base64_from_reviewed_host(self):
        url = next(iter(self.blobs()))

        class Response(io.BytesIO):
            def __init__(self, data, final_url):
                super().__init__(data)
                self.final_url = final_url

            def geturl(self):
                return self.final_url

        with mock.patch.object(fallback.urllib.request, "urlopen",
                               return_value=Response(base64.b64encode(b"safe"), url)):
            self.assertEqual(fallback.read_gitiles(url), b"safe")
        for encoded, final in ((b"%not-base64", url),
                               (base64.b64encode(b"safe"), "https://unreviewed.example/blob"),
                               (b"A" * (fallback.MAX_ENCODED_BYTES + 1), url)):
            with self.subTest(final=final, encoded_size=len(encoded)), \
                    mock.patch.object(fallback.urllib.request, "urlopen",
                                      return_value=Response(encoded, final)), \
                    self.assertRaises(RuntimeError):
                fallback.read_gitiles(url)

    def test_gitiles_blob_503_is_bounded_and_404_is_not_retried(self):
        url = next(iter(self.blobs()))
        for code, attempts in ((503, 4), (404, 1)):
            called = []
            sleeps = []

            def unavailable(*args, **kwargs):
                called.append(1)
                raise urllib.error.HTTPError(url, code, "unavailable", {}, io.BytesIO())

            with self.subTest(code=code), mock.patch.object(
                    fallback.urllib.request, "urlopen", side_effect=unavailable):
                with self.assertRaises(urllib.error.HTTPError):
                    fallback.read_gitiles(url, sleep=sleeps.append)
            self.assertEqual(len(called), attempts)
            self.assertEqual(sleeps, list(range(1, attempts)))


if __name__ == "__main__":
    unittest.main()
