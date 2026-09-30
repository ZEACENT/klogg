"""Ordinary CI discovers authenticated source bytes without inheriting prior qualification."""

from __future__ import annotations

import hashlib
import io
import json
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_adb_source_transport as transport
import ci_environment_source_cache as cache


class OrdinarySourceTransportTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.repo = self.repo.resolve()
        self.lock = self.root / "lock.json"
        self.data = b"locked archive"
        self.lock.write_text(json.dumps({"sources": [{"id": "src", "archive_file": "src.tar.gz",
            "archive_sha256": hashlib.sha256(self.data).hexdigest()}]}), encoding="utf-8")
        self.merge = "a" * 40
        self.head = "b" * 40
        self.prior = "c" * 40
        self.current_run = 99
        self.branch = "feature-source"
        self.event_file = self.root / "event.json"
        self.env = {"GITHUB_REPOSITORY": cache.REPOSITORY, "GH_TOKEN": "test-token",
                    "GITHUB_EVENT_NAME": "pull_request", "GITHUB_SHA": self.merge,
                    "GITHUB_RUN_ID": str(self.current_run), "GITHUB_EVENT_PATH": str(self.event_file),
                    "GITHUB_HEAD_REF": self.branch}
        self.write_event()
        self.zip_bytes = self.zip_content(self.data)
        self.artifacts = [self.artifact(101, 201, self.prior)]
        self.downloaded = []
        self.queries = []
        self.git_queries = []

    def write_event(self, *, fork=False, head=None):
        canonical = {"id": 11, "full_name": cache.REPOSITORY}
        self.event_file.write_text(json.dumps({"repository": canonical, "pull_request": {
            "head": {"sha": head or self.head, "ref": self.branch,
                     "repo": {"id": 12, "full_name": "someone/klogg"} if fork else canonical}}}),
            encoding="utf-8")

    @staticmethod
    def zip_content(data):
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as archive:
            for name, content in {"src.tar.gz": data,
                                  "adb-helper-prefetch-manifest.json": b"{}"}.items():
                info = zipfile.ZipInfo(name)
                info.external_attr = (stat.S_IFREG | 0o644) << 16
                archive.writestr(info, content)
        return output.getvalue()

    def artifact(self, artifact_id, run_id, sha, *, branch=None, expired=False, created=None):
        return {"id": artifact_id, "name": "adb-helper-source-cache", "expired": expired,
                "size_in_bytes": len(self.zip_bytes) if hasattr(self, "zip_bytes") else 1,
                "digest": "sha256:" + hashlib.sha256(self.zip_bytes).hexdigest() if hasattr(self, "zip_bytes") else "sha256:" + "0" * 64,
                "created_at": created or "2026-09-28T06:25:00Z",
                "workflow_run": {"id": run_id, "head_sha": sha,
                    "head_branch": branch or self.branch, "repository_id": 11,
                    "head_repository_id": 11}}

    def metadata(self, path):
        self.queries.append(path)
        prefix = "repos/" + cache.REPOSITORY + "/actions/"
        self.assertTrue(path.startswith(prefix), path)
        endpoint = path[len(prefix):]
        if endpoint.startswith("artifacts?name="):
            return {"total_count": len(self.artifacts), "artifacts": self.artifacts}
        if endpoint.startswith("artifacts/"):
            artifact_id = int(endpoint.split("/")[1])
            return next(artifact for artifact in self.artifacts if artifact["id"] == artifact_id)
        run_id = int(endpoint.split("/")[1])
        artifact = next(artifact for artifact in self.artifacts
                        if artifact["workflow_run"]["id"] == run_id)
        sha = artifact["workflow_run"]["head_sha"]
        branch = artifact["workflow_run"]["head_branch"]
        if endpoint.endswith("/jobs?per_page=100"):
            return {"total_count": 1, "jobs": [{"name": cache.PREFETCH_JOB,
                "status": "completed", "conclusion": "success", "run_attempt": 1,
                "started_at": "2026-09-28T06:24:00Z",
                "completed_at": "2026-09-28T06:26:00Z"}]}
        if "/attempts/" in endpoint:
            return {"id": run_id, "run_attempt": 1, "head_sha": sha,
                    "run_started_at": "2026-09-28T06:23:00Z"}
        return {"id": run_id, "event": "pull_request", "head_sha": sha,
                "head_branch": branch, "run_attempt": 1, "path": cache.WORKFLOW,
                "repository": {"id": 11, "full_name": cache.REPOSITORY},
                "head_repository": {"id": 11, "full_name": cache.REPOSITORY}}

    def git(self, command, *, cwd):
        self.git_queries.append(command)
        self.assertEqual(cwd, self.repo)
        if command == ["git", "rev-parse", "HEAD"]:
            return self.merge + "\n"
        if command[:3] == ["git", "merge-base", "--is-ancestor"]:
            if command[3] in {self.head, self.prior} and command[4] in {self.head, self.merge}:
                return ""
            raise cache.SourceCacheError("not an ancestor")
        raise AssertionError(command)

    def download(self, artifact_id, archive, *, limit, timeout):
        self.downloaded.append(artifact_id)
        archive.write_bytes(self.zip_bytes)

    def validate(self, lock, directory):
        self.assertEqual((directory / "src.tar.gz").read_bytes(), self.data)
        (directory / "adb-helper-prefetch-manifest.json").write_text("generated", encoding="utf-8")

    def discover(self):
        return transport.discover(self.lock, self.repo, self.root / "download",
                                  environ=self.env, metadata=self.metadata,
                                  git_runner=self.git, download=self.download,
                                  validator=self.validate)

    def test_pr_merge_uses_canonical_event_head_to_rank_same_branch_ancestor(self):
        self.assertEqual(self.discover(), 0)
        self.assertEqual(self.downloaded, [101])
        self.assertEqual(sorted(path.name for path in (self.root / "download").iterdir()),
                         ["src.tar.gz"])
        self.assertIn(["git", "merge-base", "--is-ancestor", self.prior, self.head],
                      self.git_queries)

    def test_master_squash_accepts_canonical_cross_branch_pr_bytes(self):
        self.env.update({"GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/master",
                         "GITHUB_SHA": self.merge})
        self.env.pop("GITHUB_HEAD_REF")
        self.event_file.write_text(json.dumps({"repository": {"id": 11,
            "full_name": cache.REPOSITORY}, "after": self.merge,
            "ref": "refs/heads/master"}), encoding="utf-8")
        self.assertEqual(self.discover(), 0)
        self.assertNotIn(["git", "merge-base", "--is-ancestor", self.prior, self.merge],
                         self.git_queries)

    def test_master_push_accepts_canonical_prior_push_bytes(self):
        self.env.update({"GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/master",
                         "GITHUB_SHA": self.merge})
        self.env.pop("GITHUB_HEAD_REF")
        self.event_file.write_text(json.dumps({"repository": {"id": 11,
            "full_name": cache.REPOSITORY}, "after": self.merge,
            "ref": "refs/heads/master"}), encoding="utf-8")
        original = self.metadata
        def metadata(path):
            result = original(path)
            if path.endswith("/runs/201"):
                return {**result, "event": "push"}
            return result
        self.metadata = metadata
        self.assertEqual(self.discover(), 0)
        self.assertEqual(self.downloaded, [101])

    def test_branch_validation_matches_git_for_representative_safe_and_unsafe_refs(self):
        branches = ("feature+cache@v2", "topic/option+debug", "worktree-master-ci-fail",
                    "feature@id", "unrelated..invalid", "foo.lock", "foo/.bar",
                    "foo/bar.lock", "foo?bar", "foo:bar", "-bad",
                    "foo\\bar", "trailing.", "two//slashes")
        self.assertFalse(transport._valid_branch("@"))  # Git expands @ to HEAD.
        for branch in branches:
            with self.subTest(branch=branch):
                expected = subprocess.run(["git", "check-ref-format", "--branch", branch],
                                          capture_output=True, check=False).returncode == 0
                self.assertEqual(transport._valid_branch(branch), expected)

    def test_git_legal_plus_and_at_branch_can_reuse_prior_pr(self):
        self.branch = "feature+cache@v2"
        self.env["GITHUB_HEAD_REF"] = self.branch
        self.write_event()
        self.artifacts[0]["workflow_run"]["head_branch"] = self.branch
        self.assertEqual(self.discover(), 0)
        self.assertEqual(self.downloaded, [101])

    def test_master_skips_unrelated_malformed_branch_and_imports_valid_artifact(self):
        self.env.update({"GITHUB_EVENT_NAME": "push", "GITHUB_REF": "refs/heads/master",
                         "GITHUB_SHA": self.merge})
        self.env.pop("GITHUB_HEAD_REF")
        self.event_file.write_text(json.dumps({"repository": {"id": 11,
            "full_name": cache.REPOSITORY}, "after": self.merge,
            "ref": "refs/heads/master"}), encoding="utf-8")
        self.artifacts.insert(0, self.artifact(102, 202, self.prior,
                                               branch="unrelated..invalid"))
        self.assertEqual(self.discover(), 0)
        self.assertEqual(self.downloaded, [101])

    def test_slow_incompatible_zip_does_not_consume_discovery_budget(self):
        first_zip = self.zip_content(b"older lock content")
        current_zip = self.zip_bytes
        self.zip_bytes = first_zip
        self.artifacts[0]["size_in_bytes"] = len(first_zip)
        self.artifacts[0]["digest"] = "sha256:" + hashlib.sha256(first_zip).hexdigest()
        second = self.artifact(100, 202, self.prior)
        second["size_in_bytes"] = len(current_zip)
        second["digest"] = "sha256:" + hashlib.sha256(current_zip).hexdigest()
        self.artifacts.append(second)
        clock = [0.0]
        def download(artifact_id, archive, *, limit, timeout):
            self.downloaded.append(artifact_id)
            archive.write_bytes(first_zip if artifact_id == 101 else current_zip)
            if artifact_id == 101:
                clock[0] += 181
        self.download = download
        with mock.patch.object(transport.time, "monotonic", side_effect=lambda: clock[0]):
            self.assertEqual(self.discover(), 0)
        self.assertEqual(self.downloaded, [101, 100])
        self.assertEqual((self.root / "download" / "src.tar.gz").read_bytes(), self.data)

    def test_transport_total_deadline_still_rejects_overlong_selected_zip(self):
        clock = [0.0]
        def download(artifact_id, archive, *, limit, timeout):
            archive.write_bytes(self.zip_bytes)
            clock[0] += transport.MAX_TOTAL_SECONDS + 1
        self.download = download
        with mock.patch.object(transport.time, "monotonic", side_effect=lambda: clock[0]):
            with self.assertRaisesRegex(cache.SourceCacheError, "total deadline"):
                self.discover()
        self.assertFalse((self.root / "download").exists())

    def test_fork_has_no_trusted_artifact_and_does_not_query_api(self):
        self.write_event(fork=True)
        self.assertEqual(self.discover(), 2)
        self.assertEqual(self.queries, [])

    def test_corrupt_selected_zip_fails_without_fallback(self):
        self.zip_bytes = b"not a zip"
        self.artifacts[0]["size_in_bytes"] = len(self.zip_bytes)
        self.artifacts[0]["digest"] = "sha256:" + hashlib.sha256(self.zip_bytes).hexdigest()
        with self.assertRaises(cache.SourceCacheError):
            self.discover()
        self.assertFalse((self.root / "download").exists())

    def test_changed_lock_incompatible_first_candidate_falls_back_to_second(self):
        first = self.zip_bytes
        changed = b"new locked archive"
        self.data = changed
        self.lock.write_text(json.dumps({"sources": [{"id": "src", "archive_file": "src.tar.gz",
            "archive_sha256": hashlib.sha256(changed).hexdigest()}]}), encoding="utf-8")
        second = self.zip_content(changed)
        self.artifacts.append(self.artifact(100, 202, self.prior))
        self.artifacts[1]["size_in_bytes"] = len(second)
        self.artifacts[1]["digest"] = "sha256:" + hashlib.sha256(second).hexdigest()
        def download(artifact_id, archive, *, limit, timeout):
            self.downloaded.append(artifact_id)
            archive.write_bytes(first if artifact_id == 101 else second)
        self.download = download
        self.assertEqual(self.discover(), 0)
        self.assertEqual(self.downloaded, [101, 100])

    def test_expired_and_current_run_artifacts_are_ignored(self):
        self.artifacts[0]["expired"] = True
        self.artifacts.append(self.artifact(102, self.current_run, self.head))
        self.assertEqual(self.discover(), 2)
        self.assertEqual(self.downloaded, [])

    def test_name_filtered_page_limits_candidates_and_downloads(self):
        self.artifacts = [self.artifact(100 + n, 200 + n, self.prior)
                          for n in range(20)]
        self.zip_bytes = self.zip_content(b"incompatible")
        for artifact in self.artifacts:
            artifact["size_in_bytes"] = len(self.zip_bytes)
            artifact["digest"] = "sha256:" + hashlib.sha256(self.zip_bytes).hexdigest()
        self.assertEqual(self.discover(), 2)
        self.assertEqual(len(self.downloaded), 2)
        self.assertIn("per_page=20", self.queries[0])

    def test_nonancestor_prior_pr_is_not_selected_even_when_merge_contains_other_history(self):
        self.artifacts[0]["workflow_run"]["head_sha"] = "d" * 40
        original = self.git
        def git(command, *, cwd):
            if command == ["git", "merge-base", "--is-ancestor", "d" * 40, self.head]:
                raise subprocess.CalledProcessError(1, command)
            return original(command, cwd=cwd)
        self.git = git
        self.assertEqual(self.discover(), 2)
        self.assertEqual(self.downloaded, [])

    def test_selected_attempt_alias_from_prior_rerun_fails_closed(self):
        original = self.metadata
        def metadata(path):
            result = original(path)
            if path.endswith("/attempts/1"):
                return {**result, "run_started_at": "2026-09-28T06:25:00Z"}
            return result
        self.metadata = metadata
        self.assertEqual(self.discover(), 2)
        self.assertEqual(self.downloaded, [])

    def test_artifact_without_successful_attempt_is_not_downloaded(self):
        original = self.metadata
        def metadata(path):
            result = original(path)
            if path.endswith("/jobs?per_page=100"):
                return {"total_count": 1, "jobs": [{**result["jobs"][0], "conclusion": "failure"}]}
            return result
        self.metadata = metadata
        self.assertEqual(self.discover(), 2)
        self.assertEqual(self.downloaded, [])

    def test_malformed_selected_artifact_provenance_fails_closed(self):
        original = self.metadata
        def metadata(path):
            result = original(path)
            if path.endswith("/runs/201"):
                return {**result, "head_repository": {"id": 12, "full_name": "fork/klogg"}}
            return result
        self.metadata = metadata
        with self.assertRaises(cache.SourceCacheError):
            self.discover()
        self.assertEqual(self.downloaded, [])

    def test_canonical_event_head_mismatch_is_fatal(self):
        self.write_event(head="e" * 40)
        with self.assertRaises(cache.SourceCacheError):
            self.discover()
        self.assertEqual(self.queries, [])

    def test_git_nonancestor_is_skippable_but_missing_history_is_fatal(self):
        command = ["git", "merge-base", "--is-ancestor", self.prior, self.head]
        with mock.patch.object(transport.subprocess, "run",
                               side_effect=subprocess.CalledProcessError(1, command)):
            self.assertFalse(transport._ancestor(self.prior, self.head, self.repo,
                                                 transport._git))
        with mock.patch.object(transport.subprocess, "run",
                               side_effect=subprocess.CalledProcessError(128, command)):
            with self.assertRaises(cache.SourceCacheError):
                transport._ancestor(self.prior, self.head, self.repo, transport._git)

    def test_discovery_deadline_is_fatal_before_any_zip_download(self):
        with mock.patch.object(transport.time, "monotonic", side_effect=[0, 181]):
            with self.assertRaisesRegex(cache.SourceCacheError, "deadline"):
                self.discover()
        self.assertEqual(self.downloaded, [])

    def test_unsafe_selected_zip_is_fatal_not_incompatible(self):
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as archive:
            archive.writestr("../src.tar.gz", self.data)
        self.zip_bytes = output.getvalue()
        self.artifacts[0]["size_in_bytes"] = len(self.zip_bytes)
        self.artifacts[0]["digest"] = "sha256:" + hashlib.sha256(self.zip_bytes).hexdigest()
        with self.assertRaises(cache.SourceCacheError):
            self.discover()
        self.assertEqual(self.downloaded, [101])
        self.assertFalse((self.root / "download").exists())

    def test_cli_returns_discovery_status_and_one_for_provenance_error(self):
        arguments = ["ci_adb_source_transport.py", "--lock", str(self.lock),
                     "--repo-root", str(self.repo), "--download-root", str(self.root / "download")]
        with mock.patch.object(sys, "argv", arguments), mock.patch.object(transport, "discover", return_value=2):
            self.assertEqual(transport.main(), 2)
        with mock.patch.object(sys, "argv", arguments), mock.patch.object(transport, "discover",
                               side_effect=cache.SourceCacheError("invalid origin")):
            self.assertEqual(transport.main(), 1)

    def test_absence_returns_two_without_creating_download_root(self):
        self.artifacts = []
        self.assertEqual(self.discover(), 2)
        self.assertFalse((self.root / "download").exists())

    def test_metadata_api_failure_is_fatal(self):
        def fail(_):
            raise cache.SourceCacheError("API failure")
        self.metadata = fail
        with self.assertRaisesRegex(cache.SourceCacheError, "API failure"):
            self.discover()


if __name__ == "__main__":
    unittest.main()
