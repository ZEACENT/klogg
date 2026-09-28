"""A prior-run ADB artifact transports bytes, never qualification authority."""

from __future__ import annotations

import copy
import hashlib
import io
import json
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest
import warnings
import zipfile
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_environment_source_cache as cache


class SourceCacheTransportTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name).resolve()
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.current = "b" * 40
        self.prior = "a" * 40
        self.run_id = 123
        self.attempt = 2
        self.artifact_id = 456
        self.files = {"a.tar.gz": b"locked source a", "b.tar.gz": b"locked source b"}
        self.lock = self.root / "lock.json"
        self.lock.write_text(json.dumps({"sources": [
            {"id": name.split(".")[0], "archive_file": name,
             "archive_sha256": hashlib.sha256(data).hexdigest()}
            for name, data in self.files.items()
        ]}), encoding="utf-8")
        self.zip_bytes = self.make_zip({**self.files, "adb-helper-prefetch-manifest.json": b"{}"})
        self.zip_digest = hashlib.sha256(self.zip_bytes).hexdigest()
        self.artifact = {
            "id": self.artifact_id, "name": "adb-helper-source-cache", "expired": False,
            "size_in_bytes": len(self.zip_bytes), "digest": "sha256:" + self.zip_digest,
            "created_at": "2026-09-28T06:25:00Z",
            "workflow_run": {"id": self.run_id, "head_sha": self.prior,
                             "head_branch": "worktree-master-ci-fail", "repository_id": 11,
                             "head_repository_id": 11},
        }
        self.run = {
            "id": self.run_id, "event": "pull_request", "head_sha": self.prior,
            "head_branch": "worktree-master-ci-fail", "run_attempt": self.attempt,
            "path": ".github/workflows/ci-build.yml",
            "repository": {"id": 11, "full_name": "ZEACENT/klogg"},
            "head_repository": {"id": 11, "full_name": "ZEACENT/klogg"},
        }
        self.attempt_run = {
            "id": self.run_id, "run_attempt": self.attempt, "head_sha": self.prior,
            "run_started_at": "2026-09-28T06:23:00Z",
            "updated_at": "2026-09-28T06:27:00Z",
        }
        self.jobs = {"total_count": 1, "jobs": [{
            "name": "Prefetch locked ADB helper source closure", "status": "completed",
            "conclusion": "success", "run_attempt": self.attempt,
            "started_at": "2026-09-28T06:24:00Z", "completed_at": "2026-09-28T06:26:00Z",
        }]}
        self.metadata_calls = []
        self.git_calls = []

    @staticmethod
    def make_zip(files, *, mode=None):
        result = io.BytesIO()
        with zipfile.ZipFile(result, "w") as archive:
            for name, data in files.items():
                info = zipfile.ZipInfo(name)
                info.external_attr = ((stat.S_IFREG | 0o644) if mode is None else mode) << 16
                archive.writestr(info, data)
        return result.getvalue()

    def metadata(self, path):
        self.metadata_calls.append(path)
        if path.endswith("/artifacts/456"):
            return self.artifact
        if path.endswith("/runs/123"):
            return self.run
        if path.endswith("/runs/123/attempts/2"):
            return self.attempt_run
        if path.endswith("/runs/123/attempts/2/jobs?per_page=100"):
            return self.jobs
        raise AssertionError("unexpected metadata request: " + path)

    def git(self, command, *, cwd):
        self.git_calls.append(command)
        self.assertEqual(pathlib.Path(cwd), self.repo)
        if command == ["git", "rev-parse", "HEAD"]:
            return self.current + "\n"
        if command == ["git", "merge-base", "--is-ancestor", self.prior, self.current]:
            return ""
        raise AssertionError("unexpected git operation: " + repr(command))

    def authenticate(self):
        return cache.authenticate_artifact(
            self.repo, self.current, self.prior, self.run_id, self.attempt,
            self.artifact_id, metadata=self.metadata, git_runner=self.git,
        )

    def test_explicit_ancestor_artifact_authenticates_only_its_transport(self):
        result = self.authenticate()
        self.assertEqual(result["digest"], self.zip_digest)
        self.assertEqual(result["size"], len(self.zip_bytes))
        self.assertEqual(len(self.metadata_calls), 4)
        self.assertIn(["git", "merge-base", "--is-ancestor", self.prior, self.current],
                      self.git_calls)

    def test_copied_successful_job_cannot_relabel_artifact_as_later_attempt(self):
        self.attempt_run = {**self.attempt_run,
                            "run_started_at": "2026-09-28T06:30:00Z",
                            "updated_at": "2026-09-28T06:40:00Z"}
        with self.assertRaisesRegex(cache.SourceCacheError, "attempt"):
            self.authenticate()

    def test_later_run_rerun_does_not_invalidate_earlier_successful_prefetch(self):
        self.run = {**self.run, "run_attempt": self.attempt + 1}
        result = self.authenticate()
        self.assertEqual(result["run_attempt"], self.attempt)
        self.assertEqual(result["digest"], self.zip_digest)

    def test_prior_artifact_branch_must_match_current_dispatch_branch(self):
        with mock.patch.dict(cache.os.environ, {"GITHUB_REF": "refs/heads/another-branch"}):
            with self.assertRaises(cache.SourceCacheError):
                self.authenticate()

    def test_substituted_or_unreviewed_producer_metadata_fails_closed(self):
        changes = (
            ("artifact id", "artifact", "id", 457),
            ("artifact name", "artifact", "name", "another-artifact"),
            ("expired", "artifact", "expired", True),
            ("artifact digest", "artifact", "digest", "sha256:bad"),
            ("oversized zip", "artifact", "size_in_bytes", cache.MAX_CACHE_BYTES + 1),
            ("wrong run", "artifact", "workflow_run", {**self.artifact["workflow_run"], "id": 124}),
            ("wrong head", "run", "head_sha", "c" * 40),
            ("fork", "run", "head_repository", {"id": 12, "full_name": "elsewhere/klogg"}),
            ("wrong branch", "run", "head_branch", "other"),
            ("wrong event", "run", "event", "workflow_dispatch"),
            ("wrong workflow", "run", "path", ".github/workflows/other.yml"),
            ("wrong attempt", "run", "run_attempt", 1),
            ("failed prefetch", "jobs", "jobs", [{**self.jobs["jobs"][0], "conclusion": "failure"}]),
            ("other attempt job", "jobs", "jobs", [{**self.jobs["jobs"][0], "run_attempt": 1}]),
            ("artifact outside job", "artifact", "created_at", "2026-09-28T06:27:00Z"),
        )
        for label, source, field, value in changes:
            with self.subTest(label=label):
                original = getattr(self, source)
                setattr(self, source, {**original, field: value})
                try:
                    with self.assertRaises(cache.SourceCacheError):
                        self.authenticate()
                finally:
                    setattr(self, source, original)

    def test_non_ancestor_or_different_checked_out_source_fails(self):
        for source in ("ancestor", "checkout"):
            with self.subTest(source=source):
                def git(command, *, cwd):
                    if source == "checkout" and command[-2:] == ["rev-parse", "HEAD"]:
                        return "c" * 40 + "\n"
                    if source == "ancestor" and command[-3:] == ["--is-ancestor", self.prior, self.current]:
                        raise subprocess.CalledProcessError(1, command)
                    return self.git(command, cwd=cwd)
                with self.assertRaises(cache.SourceCacheError):
                    cache.authenticate_artifact(
                        self.repo, self.current, self.prior, self.run_id, self.attempt,
                        self.artifact_id, metadata=self.metadata, git_runner=git,
                    )

    def test_exact_zip_is_extracted_without_trusting_its_manifest(self):
        archive = self.root / "source.zip"
        archive.write_bytes(self.zip_bytes)
        output = self.root / "import"
        cache.extract_verified_zip(archive, output, self.lock, self.zip_digest)
        self.assertEqual({p.name for p in output.iterdir()}, set(self.files))
        for name, data in self.files.items():
            self.assertEqual((output / name).read_bytes(), data)

    def test_transport_digest_members_and_raw_lock_hash_fail_before_publication(self):
        mutations = (
            ("digest", self.zip_bytes, "0" * 64),
            ("missing", self.make_zip({"a.tar.gz": self.files["a.tar.gz"]}), None),
            ("extra", self.make_zip({**self.files, "bad": b"bad"}), None),
            ("unsafe", self.make_zip({**self.files, "../bad": b"bad"}), None),
            ("symlink", self.make_zip({**self.files, "adb-helper-prefetch-manifest.json": b"{}"},
                                      mode=stat.S_IFLNK | 0o777), None),
            ("wrong raw hash", self.make_zip({**self.files, "b.tar.gz": b"wrong",
                                               "adb-helper-prefetch-manifest.json": b"{}"}), None),
        )
        for label, payload, override in mutations:
            with self.subTest(label=label):
                archive = self.root / "source.zip"
                archive.write_bytes(payload)
                output = self.root / "import"
                with self.assertRaises(cache.SourceCacheError):
                    cache.extract_verified_zip(archive, output, self.lock,
                        override or hashlib.sha256(payload).hexdigest())
                self.assertFalse(output.exists())

    def test_optional_transport_rejects_missing_generated_manifest_as_corruption(self):
        payload = self.make_zip(self.files)
        archive = self.root / "missing-manifest.zip"
        archive.write_bytes(payload)
        output = self.root / "import"
        with self.assertRaises(cache.SourceCacheError) as raised:
            cache.extract_verified_zip(archive, output, self.lock,
                                       hashlib.sha256(payload).hexdigest(),
                                       allow_incompatible=True)
        self.assertNotIsInstance(raised.exception, cache.IncompatibleSourceCacheError)
        self.assertFalse(output.exists())

    def test_optional_transport_distinguishes_old_lock_from_malformed_zip(self):
        archive = self.root / "source.zip"
        archive.write_bytes(self.zip_bytes)
        changed = json.loads(self.lock.read_text(encoding="utf-8"))
        changed["sources"][1]["archive_sha256"] = "0" * 64
        self.lock.write_text(json.dumps(changed), encoding="utf-8")
        output = self.root / "import"
        with self.assertRaises(cache.IncompatibleSourceCacheError):
            cache.extract_verified_zip(archive, output, self.lock, self.zip_digest,
                                       allow_incompatible=True)
        self.assertFalse(output.exists())
        changed["sources"].append({"id": "c", "archive_file": "c.tar.gz",
                                   "archive_sha256": hashlib.sha256(b"new").hexdigest()})
        self.lock.write_text(json.dumps(changed), encoding="utf-8")
        with self.assertRaises(cache.IncompatibleSourceCacheError):
            cache.extract_verified_zip(archive, output, self.lock, self.zip_digest,
                                       allow_incompatible=True)
        self.assertFalse(output.exists())
        unsafe = self.make_zip({**self.files, "../outside": b"unsafe"})
        archive.write_bytes(unsafe)
        with self.assertRaises(cache.SourceCacheError) as raised:
            cache.extract_verified_zip(archive, output, self.lock,
                                       hashlib.sha256(unsafe).hexdigest(),
                                       allow_incompatible=True)
        self.assertNotIsInstance(raised.exception, cache.IncompatibleSourceCacheError)
        self.assertFalse(output.exists())

    def test_duplicate_and_oversized_members_fail_before_publication(self):
        archive = self.root / "source.zip"
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(archive, "w") as zipped:
                for name in ("a.tar.gz", "a.tar.gz", "b.tar.gz", "adb-helper-prefetch-manifest.json"):
                    zipped.writestr(name, self.files.get(name, b"{}"))
        output = self.root / "import"
        with self.assertRaises(cache.SourceCacheError):
            cache.extract_verified_zip(archive, output, self.lock,
                                       hashlib.sha256(archive.read_bytes()).hexdigest())
        self.assertFalse(output.exists())
        archive.write_bytes(self.zip_bytes)
        with self.assertRaises(cache.SourceCacheError):
            cache.extract_verified_zip(archive, output, self.lock, self.zip_digest,
                                       max_cache_bytes=5)
        self.assertFalse(output.exists())

    def test_zip_size_must_match_authenticated_artifact_metadata(self):
        self.artifact = {**self.artifact, "size_in_bytes": len(self.zip_bytes) - 1}
        output = self.root / "import"
        def download(artifact_id, archive, *, limit, timeout):
            archive.write_bytes(self.zip_bytes)
        with self.assertRaisesRegex(cache.SourceCacheError, "size"):
            cache.import_source_cache(self.lock, self.repo, self.current, self.prior,
                self.run_id, self.attempt, self.artifact_id, output,
                metadata=self.metadata, git_runner=self.git,
                download=download, validator=mock.Mock())
        self.assertFalse(output.exists())

    def test_streaming_zip_download_enforces_actual_byte_limit(self):
        real_popen = subprocess.Popen
        def producer(command, **kwargs):
            self.assertEqual(command[-1],
                "repos/ZEACENT/klogg/actions/artifacts/456/zip")
            return real_popen([sys.executable, "-c", "import os; os.write(1, b'x' * 64)"], **kwargs)
        with mock.patch.dict(cache.os.environ, {"GH_TOKEN": "synthetic"}), \
                mock.patch.object(cache.subprocess, "Popen", side_effect=producer):
            archive = self.root / "download.zip"
            cache.download_artifact_zip(self.artifact_id, archive, limit=64, timeout=5)
            self.assertEqual(archive.read_bytes(), b"x" * 64)
            with self.assertRaisesRegex(cache.SourceCacheError, "byte limit"):
                cache.download_artifact_zip(self.artifact_id, self.root / "oversized.zip",
                                            limit=10, timeout=5)

    def test_real_original_offline_validator_keeps_published_cache_archives_only(self):
        output = self.root / "real-validator-import"
        def download(artifact_id, archive, *, limit, timeout):
            archive.write_bytes(self.zip_bytes)
        cache.import_source_cache(self.lock, self.repo, self.current, self.prior,
                                  self.run_id, self.attempt, self.artifact_id, output,
                                  metadata=self.metadata, git_runner=self.git,
                                  download=download)
        self.assertEqual({path.name for path in output.iterdir()}, set(self.files))

    def test_complete_import_validates_offline_and_never_downloads_sources(self):
        output = self.root / "import"
        calls = []
        def download(artifact_id, archive, *, limit, timeout):
            calls.append(("download", artifact_id, limit, timeout))
            archive.write_bytes(self.zip_bytes)
        def validator(lock, directory):
            calls.append(("offline", lock, directory))
            self.assertEqual({p.name for p in directory.iterdir()}, set(self.files))
            (directory / "adb-helper-prefetch-manifest.json").write_text("generated", encoding="utf-8")
        cache.import_source_cache(self.lock, self.repo, self.current, self.prior,
                                  self.run_id, self.attempt, self.artifact_id, output,
                                  metadata=self.metadata, git_runner=self.git,
                                  download=download, validator=validator)
        self.assertEqual([call[0] for call in calls], ["download", "offline"])
        self.assertTrue(output.is_dir())
        self.assertEqual({p.name for p in output.iterdir()}, set(self.files))


if __name__ == "__main__":
    unittest.main()
