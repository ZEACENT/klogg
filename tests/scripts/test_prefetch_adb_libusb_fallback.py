"""The AOSP libusb fallback must reproduce only the reviewed locked archive."""

from __future__ import annotations

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
import prefetch_adb_libusb_fallback as fallback


class LibusbFallbackTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.downloads = self.root / "downloads"
        self.downloads.mkdir()
        self.commit = "a" * 40
        self.files = {
            ".gitattributes": ("100644", b"*.txt export-ignore\n"),
            "INSTALL_WIN.txt": ("100644", b"windows\r\nsetup\r\n"),
            "README": ("100644", b"readme\n"),
            "README.md": ("120000", b"README"),
            "bin/tool": ("100755", b"#!/bin/sh\n"),
            "doc/libusb.png": ("100644", b"\x89PNG\r\n\x00"),
        }
        self.archive_bytes = self.canonical_archive(self.files)
        self.policy = {
            "commit": self.commit,
            "repository_url": "https://android.googlesource.com/platform/external/libusb",
            "archive_file": "aosp-libusb-" + self.commit + ".tar.gz",
            "sha256": hashlib.sha256(self.archive_bytes).hexdigest(),
            "entries": len(self.files),
            "directories": 2,
            "symlinks": 1,
        }
        self.url = self.policy["repository_url"] + "/+archive/" + self.commit + ".tar.gz"
        self.lock = self.root / "lock.json"
        self.record = {
            "id": "aosp-libusb",
            "commit": self.commit,
            "repository_url": self.policy["repository_url"],
            "archive_url": self.url,
            "archive_file": self.policy["archive_file"],
            "archive_sha256": self.policy["sha256"],
            "archive_identity": "canonical-tar-gz-v1",
            "build_input": False,
        }
        self.write_lock()

    def write_lock(self):
        self.lock.write_text(json.dumps({"sources": [self.record]}), encoding="utf-8")

    @staticmethod
    def canonical_archive(files):
        directories = {str(pathlib.PurePosixPath(name).parent)
                       for name in files if "/" in name}
        output = io.BytesIO()
        with gzip.GzipFile(filename="", mode="wb", fileobj=output, mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for name in sorted(directories | set(files)):
                    member = tarfile.TarInfo(name)
                    member.uid = member.gid = member.mtime = 0
                    member.uname = member.gname = "root"
                    if name in directories:
                        member.type = tarfile.DIRTYPE
                        member.mode = 0o755
                        archive.addfile(member)
                        continue
                    mode, data = files[name]
                    member.mode = 0o755 if mode == "100755" else 0o644
                    if mode == "120000":
                        member.type = tarfile.SYMTYPE
                        member.linkname = data.decode("ascii")
                        archive.addfile(member)
                    else:
                        member.size = len(data)
                        archive.addfile(member, io.BytesIO(data))
        return output.getvalue()

    def fallback_from_503(self, *, files=None, policy=None):
        error = urllib.error.HTTPError(self.url, 503, "unavailable", {}, io.BytesIO())
        self.addCleanup(error.close)
        downloader = mock.Mock(side_effect=error)
        fetch = mock.Mock(return_value=self.files if files is None else files)
        destination = fallback.prefetch_libusb(
            self.lock, self.downloads,
            policy=self.policy if policy is None else policy,
            downloader=downloader, fetch=fetch,
        )
        downloader.assert_called_once()
        fetch.assert_called_once()
        return destination

    def test_terminal_archive_503_reproduces_exact_locked_source(self):
        destination = self.fallback_from_503()
        self.assertEqual(destination.read_bytes(), self.archive_bytes)
        self.assertEqual(hashlib.sha256(destination.read_bytes()).hexdigest(),
                         self.policy["sha256"])
        self.assertEqual(list(self.downloads.iterdir()), [destination])
        with tarfile.open(destination, "r:gz") as archive:
            self.assertEqual([member.name for member in archive],
                             sorted({"bin", "doc", *self.files}))
            self.assertEqual(archive.extractfile("INSTALL_WIN.txt").read(),
                             b"windows\r\nsetup\r\n")
            self.assertEqual(archive.extractfile("doc/libusb.png").read(),
                             b"\x89PNG\r\n\x00")

    def test_existing_verified_archive_avoids_network(self):
        destination = self.downloads / self.policy["archive_file"]
        destination.write_bytes(self.archive_bytes)
        self.assertEqual(fallback.prefetch_libusb(
            self.lock, self.downloads, policy=self.policy,
            downloader=mock.Mock(side_effect=AssertionError("network")),
            fetch=mock.Mock(side_effect=AssertionError("network")),
        ), destination)

    def test_archive_404_and_wrong_bytes_never_trigger_fallback(self):
        error = urllib.error.HTTPError(self.url, 404, "missing", {}, io.BytesIO())
        self.addCleanup(error.close)
        for downloader, failure in ((mock.Mock(side_effect=error), urllib.error.HTTPError),
                                    (lambda url, path: path.write_bytes(self.canonical_archive({
                                        **self.files, "README": ("100644", b"wrong")
                                    })), RuntimeError)):
            with self.subTest(downloader=downloader):
                fetch = mock.Mock(side_effect=AssertionError("unexpected fallback"))
                if failure is RuntimeError:
                    with self.assertRaisesRegex(RuntimeError, "canonical SHA"):
                        fallback.prefetch_libusb(self.lock, self.downloads, policy=self.policy,
                                                 downloader=downloader, fetch=fetch)
                else:
                    with self.assertRaises(failure):
                        fallback.prefetch_libusb(self.lock, self.downloads, policy=self.policy,
                                                 downloader=downloader, fetch=fetch)
                fetch.assert_not_called()
                self.assertEqual(list(self.downloads.iterdir()), [])

    def test_trickling_archive_cannot_extend_past_total_download_deadline(self):
        response = mock.MagicMock()
        response.__enter__.return_value = response
        response.read1.return_value = b"x"
        response.read.side_effect = AssertionError("buffered read can block on slow trickle")
        with mock.patch.object(fallback.urllib.request, "urlopen", return_value=response), \
                mock.patch.object(fallback.time, "monotonic", side_effect=[0, 0, 181]):
            with self.assertRaisesRegex(RuntimeError, "deadline"):
                fallback.download_archive(self.url, self.root / "partial", attempts=1)
        response.read.assert_not_called()
        self.assertEqual(response.read1.call_count, 1)

    def test_small_compressed_archive_cannot_expand_without_bound(self):
        inflated = self.canonical_archive({"payload": ("100644", b"A" * (20 * 1024 * 1024))})
        self.assertLess(len(inflated), fallback.MAX_ARCHIVE_BYTES)
        with mock.patch.object(fallback, "canonicalize_tar_gz",
                               side_effect=AssertionError("unbounded canonicalization")):
            with self.assertRaisesRegex(RuntimeError, "expanded"):
                fallback.prefetch_libusb(self.lock, self.downloads, policy=self.policy,
                    downloader=lambda url, path: path.write_bytes(inflated),
                    fetch=mock.Mock(side_effect=AssertionError("unreviewed fallback")))
        self.assertEqual(list(self.downloads.iterdir()), [])

    def test_bad_blob_symlink_path_or_text_fails_before_publication(self):
        mutations = (
            (lambda files: files.update({"README": ("100644", b"wrong")}), "canonical SHA"),
            (lambda files: files.update({"README.md": ("120000", b"../../outside")}), "symlink"),
            (lambda files: files.update({"INSTALL_WIN.txt": ("100644", b"broken\rtext")}), "canonical SHA"),
            (lambda files: files.update({"bin/tool": ("160000", b"unexpected submodule")}), "mode"),
            (lambda files: files.update({"../outside": files.pop("bin/tool")}), "path"),
        )
        for mutate, failure in mutations:
            with self.subTest(mutate=mutate):
                files = dict(self.files)
                mutate(files)
                with self.assertRaisesRegex(RuntimeError, failure):
                    self.fallback_from_503(files=files)
                self.assertFalse((self.downloads / self.policy["archive_file"]).exists())
                self.assertEqual(list(self.downloads.iterdir()), [])

    def test_git_fetch_has_one_deadline_across_all_operations(self):
        with mock.patch.object(fallback, "_git", return_value=b"" ) as git, \
                mock.patch.object(fallback.time, "monotonic", side_effect=[0, 0, 301, 301]):
            with self.assertRaisesRegex(RuntimeError, "deadline"):
                fallback.fetch_git_tree(self.policy)
        self.assertEqual(git.call_count, 1)
        self.assertEqual(git.call_args.args[1], ["init", "-q"])

    def test_terminal_git_failure_names_the_official_source(self):
        with mock.patch.object(fallback, "_git", side_effect=[b"", RuntimeError("fetch failed"),
                                                            RuntimeError("fetch failed")]), \
                mock.patch.object(fallback.time, "monotonic", return_value=0), \
                mock.patch.object(fallback.time, "sleep") as sleep:
            with self.assertRaisesRegex(RuntimeError, self.policy["repository_url"]):
                fallback.fetch_git_tree(self.policy)
        sleep.assert_called_once_with(10)

    def test_invalid_git_blob_identity_never_reaches_archive_writer(self):
        tree = b"100644 blob " + b"f" * 40 + b"\tREADME\0"
        with mock.patch.object(fallback, "_git", side_effect=[b"", b"", self.commit.encode(), tree, b"wrong"]), \
                mock.patch.object(fallback.time, "monotonic", return_value=0):
            with self.assertRaisesRegex(RuntimeError, "tree identity"):
                fallback.fetch_git_tree({**self.policy, "entries": 1})

    def test_lock_commit_origin_and_sha_must_match_reviewed_source(self):
        for field, value in (("commit", "b" * 40),
                             ("repository_url", "https://unreviewed.example/libusb"),
                             ("archive_sha256", "0" * 64)):
            with self.subTest(field=field):
                original = self.record[field]
                self.record[field] = value
                self.write_lock()
                downloader = mock.Mock(side_effect=AssertionError("unreviewed network"))
                with self.assertRaisesRegex(RuntimeError, "unreviewed locked"):
                    fallback.prefetch_libusb(self.lock, self.downloads,
                                             policy=self.policy, downloader=downloader)
                downloader.assert_not_called()
                self.record[field] = original


if __name__ == "__main__":
    unittest.main()
