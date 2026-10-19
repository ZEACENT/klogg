import copy
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[2]
LOCAL_CALLS = (".github/workflows/ci-ios-host-evidence.yml", ".github/workflows/ci-environments.yml",
               ".github/workflows/ci-dependencies.yml")
sys.path.insert(0, str(ROOT / "scripts"))
try:
    import ci_compiler_object_transport as transport
except ModuleNotFoundError:
    transport = None


class CompilerObjectTransportTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(transport, "cross-ref compiler object artifact transport is not implemented")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        for name in transport.POLICY_FILES:
            path = self.repo / name
            path.parent.mkdir(parents=True, exist_ok=True)
            text = "reviewed producer policy\n"
            if name == transport.WORKFLOW:
                text = "jobs:\n" + "".join(f"  local{index}:\n    uses: ./{workflow}\n"
                                            for index, workflow in enumerate(LOCAL_CALLS))
            path.write_text(text)
        self.descriptor = {
            "schema_version": 1, "kind": "ccache", "label": "native-adb-macos-x86_64",
            "compatibility": {"core": "source-recipe-v1", "compiler": "clang-pinned", "flags": []},
            "policy_files": [], "producer_job": "ADB native macOS",
            "required_steps": ["Build and verify native helper"],
            "export_step": "Build and verify native helper", "budget_bytes": 1024 * 1024,
        }
        self.context = transport.context(self.descriptor, self.repo)
        self.origin = {"repository_id": 12, "run_id": 201, "run_attempt": 1,
                       "event": "pull_request", "head_sha": "a" * 40,
                       "checkout_sha": "b" * 40, "tree": "c" * 40}
        self.cache = self.root / "cache"
        (self.cache / "a" / "b").mkdir(parents=True)
        (self.cache / "a" / "b" / "entryR").write_bytes(b"compiled objects")
        self.bundle = self.root / "bundle"
        transport.pack(self.cache, self.bundle, self.context, self.origin)
        self.zipped = self.root / "artifact.zip"
        with zipfile.ZipFile(self.zipped, "w") as archive:
            for file in sorted(self.bundle.iterdir()):
                archive.write(file, file.name)
        self.artifact = {
            "id": 101, "name": transport.artifact_name(self.context, 1),
            "expired": False, "created_at": "2026-10-18T10:05:00Z",
            "size_in_bytes": self.zipped.stat().st_size,
            "digest": "sha256:" + hashlib.sha256(self.zipped.read_bytes()).hexdigest(),
            "workflow_run": {"id": 201, "repository_id": 12, "head_repository_id": 12,
                             "head_sha": "a" * 40, "head_branch": "old-feature"},
        }
        repository = {"id": 12, "full_name": transport.REPOSITORY}
        self.run = {"id": 201, "run_attempt": 1, "head_sha": "a" * 40,
                    "head_branch": "old-feature", "event": "pull_request",
                    "repository": repository, "head_repository": repository,
                    "path": transport.WORKFLOW, "status": "completed", "conclusion": "success",
                    "referenced_workflows": [{"path": transport.REPOSITORY + "/" + name + "@" + "b" * 40,
                                               "sha": "b" * 40, "ref": "refs/pull/80/merge"}
                                              for name in LOCAL_CALLS]}
        self.attempt = {**self.run, "run_started_at": "2026-10-18T10:00:00Z"}
        self.job = {"id": 301, "run_id": 201, "run_attempt": 1, "head_sha": "a" * 40,
                    "name": self.descriptor["producer_job"], "status": "completed", "conclusion": "success",
                    "started_at": "2026-10-18T10:01:00Z", "completed_at": "2026-10-18T10:06:00Z",
                    "steps": [{"name": "Build and verify native helper", "status": "completed", "conclusion": "success"}]}
        self.calls = []
        self.downloads = []
        self.now = transport.timestamp("2026-10-18T11:00:00Z")

    def api(self, endpoint):
        self.calls.append(endpoint)
        if "/contents/" in endpoint:
            from urllib.parse import unquote
            name = unquote(endpoint.split("/contents/", 1)[1].split("?", 1)[0])
            return {"type": "file", "path": name, "sha": self.context["policy_files"][name]}
        if endpoint == "repos/" + transport.REPOSITORY + "/git/commits/" + "b" * 40:
            return {"sha": "b" * 40, "tree": {"sha": "c" * 40},
                    "parents": [{"sha": "d" * 40}, {"sha": "a" * 40}]}
        if endpoint.endswith("artifacts/101"):
            return copy.deepcopy(self.artifact)
        if "/artifacts?" in endpoint:
            return {"total_count": 1, "artifacts": [copy.deepcopy(self.artifact)]}
        if "/attempts/1/jobs?" in endpoint:
            return {"total_count": 1, "jobs": [copy.deepcopy(self.job)]}
        if endpoint.endswith("/attempts/1"):
            return copy.deepcopy(self.attempt)
        if endpoint.endswith("/runs/201"):
            return copy.deepcopy(self.run)
        self.fail("unexpected endpoint " + endpoint)

    def download(self, identifier, path, *, limit, timeout):
        self.downloads.append(identifier)
        path.write_bytes(self.zipped.read_bytes())

    def restore(self, **kwargs):
        return transport.restore(self.context, self.root / "restored", repository_id=12,
                                 current_run=999, metadata=kwargs.pop("metadata", self.api),
                                 download=self.download, now=self.now, **kwargs)

    def application_artifact(self, cache=None):
        self.descriptor["compatibility"]["component"] = "application"
        self.context = transport.context(self.descriptor, self.repo)
        self.bundle = self.root / "application-bundle"
        transport.pack(cache or self.cache, self.bundle, self.context, self.origin)
        with zipfile.ZipFile(self.zipped, "w") as archive:
            for file in sorted(self.bundle.iterdir()):
                archive.write(file, file.name)
        self.artifact.update(name=transport.artifact_name(self.context, 1),
                             size_in_bytes=self.zipped.stat().st_size,
                             digest="sha256:" + hashlib.sha256(self.zipped.read_bytes()).hexdigest())

    def test_backend_hint_requires_current_profile_and_application_tree(self):
        self.application_artifact()
        transport.mark_backend(self.cache, self.context, self.origin)
        self.assertTrue(transport.backend_matches(self.cache, self.context, 12, "c" * 40))
        self.assertFalse(transport.backend_matches(self.cache, self.context, 12, "e" * 40))
        self.assertFalse(transport.backend_matches(self.cache, self.context, 99, "c" * 40))
        changed = copy.deepcopy(self.descriptor)
        changed["compatibility"]["core"] = "changed-dependency"
        self.assertFalse(transport.backend_matches(self.cache, transport.context(changed, self.repo), 12, "c" * 40))
        (self.cache / transport.BACKEND_MARKER).write_text("{invalid")
        self.assertFalse(transport.backend_matches(self.cache, self.context, 12, "c" * 40))

    def test_backend_marker_cli_needs_no_archive_output(self):
        descriptor_path = self.root / "descriptor.json"
        descriptor_path.write_text(json.dumps(self.descriptor))
        output = self.root / "action-output"
        with mock.patch.object(transport, "identity", return_value=self.origin):
            with mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(output)}):
                self.assertEqual(transport.main(["mark-backend", "--descriptor", str(descriptor_path),
                                                 "--cache", str(self.cache), "--repo-root", str(self.repo)]), 0)
        self.assertTrue((self.cache / transport.BACKEND_MARKER).is_file())
        self.assertIn("marked=true", output.read_text())

    def test_retained_backend_cli_does_not_emit_discovered_artifact_as_import(self):
        descriptor_path = self.root / "descriptor.json"
        descriptor_path.write_text(json.dumps(self.descriptor))
        output, report = self.root / "action-output", self.root / "restore-report.json"
        retained = {"restored": False, "export_needed": True, "backend_retained": True,
                    "available_artifact_id": 101, "transport": "actions-cache"}
        with mock.patch.object(transport, "identity", return_value=self.origin):
            with mock.patch.object(transport, "restore", return_value=retained):
                with mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(output)}):
                    transport.main(["restore", "--descriptor", str(descriptor_path), "--cache", str(self.cache),
                                    "--repo-root", str(self.repo), "--output", str(report)])
        self.assertIn("artifact-id=", output.read_text().splitlines())
        self.assertNotIn("artifact-id=101", output.read_text())
        self.assertEqual(json.loads(report.read_text())["available_artifact_id"], 101)

    def test_native_backend_hint_ignores_unrelated_application_tree(self):
        transport.mark_backend(self.cache, self.context, self.origin)
        self.assertTrue(transport.backend_matches(self.cache, self.context, 12, "e" * 40))

    def test_backend_hint_is_excluded_from_shared_snapshot_and_rejected_on_import(self):
        transport.mark_backend(self.cache, self.context, self.origin)
        bundle = self.root / "marker-free"
        transport.pack(self.cache, bundle, self.context, self.origin)
        with tarfile.open(bundle / "objects.tar") as archive:
            self.assertNotIn(transport.BACKEND_MARKER, archive.getnames())
        with self.assertRaises(ValueError):
            transport.safe_name(transport.BACKEND_MARKER)

    def test_changed_application_tree_requires_export_even_with_young_shared_snapshot(self):
        self.application_artifact()
        result = self.restore(current_tree="e" * 40)
        self.assertTrue(result["restored"])
        self.assertTrue(result["export_needed"])

    def test_matching_application_tree_suppresses_redundant_young_export(self):
        self.application_artifact()
        result = self.restore(current_tree="c" * 40)
        self.assertFalse(result["export_needed"])

    def test_current_tree_artifact_is_preferred_over_newer_other_source_snapshot(self):
        self.application_artifact()
        newer = copy.deepcopy(self.artifact)
        newer.update(id=102, created_at="2026-10-18T10:06:00Z")
        newer["workflow_run"]["id"] = 202
        def metadata(endpoint):
            if "/artifacts?" in endpoint:
                return {"total_count": 2, "artifacts": [newer, self.artifact]}
            if endpoint.endswith("artifacts/102"):
                return newer
            return self.api(endpoint)
        terminal = {"aggregate_conclusion": "success", "producer_attempt_conclusion": "success",
                    "producer_job_conclusion": "success"}
        def producer(artifact, *args):
            origin = self.origin if artifact["id"] == 101 else {**self.origin, "run_id": 202, "tree": "e" * 40}
            return origin, terminal
        with mock.patch.object(transport, "_producer", side_effect=producer):
            result = self.restore(current_tree="c" * 40, metadata=metadata)
        self.assertEqual(result["artifact_id"], 101)
        self.assertEqual(self.downloads, [101])

    @unittest.skipUnless(shutil.which("ccache") and shutil.which("clang"), "ccache/clang unavailable")
    def test_real_repeat_pr_retains_changed_source_hit_over_older_shared_snapshot(self):
        cache = self.root / "real-cache"
        config = self.root / "trusted-ccache.conf"
        config.write_text("compiler_check = content\nmax_size = 16M\n")
        source, output = self.root / "probe.c", self.root / "probe.o"
        env = {key: value for key, value in os.environ.items() if not key.startswith("CCACHE_")}
        env.update(CCACHE_DIR=str(cache), CCACHE_CONFIGPATH=str(config), CCACHE_BASEDIR=str(self.root))
        command = ["ccache", shutil.which("clang"), "-c", str(source), "-o", str(output)]
        def run(arguments):
            return subprocess.run(arguments, env=env, check=True, capture_output=True, text=True, timeout=30).stdout
        source.write_text("int answer(void) { return 1; }\n")
        run(command)
        output.unlink()
        self.descriptor["budget_bytes"] = 16_000_000
        self.application_artifact(cache)
        source.write_text("int answer(void) { return 2; }\n")
        run(command)
        output.unlink()
        current = {**self.origin, "run_id": 999, "tree": "e" * 40}
        transport.mark_backend(cache, self.context, current)
        trusted = config.read_bytes()
        for repetition in range(2):
            result = transport.restore(self.context, self.root / f"unused-import-{repetition}",
                                       repository_id=12, current_run=999, current_tree=current["tree"],
                                       backend_cache=cache, metadata=self.api, download=self.download, now=self.now)
            self.assertTrue(result["backend_retained"])
            self.assertFalse(result["restored"])
            self.assertNotIn("artifact_id", result, "discovery must not relabel retained backend as an import")
            self.assertEqual(result["available_artifact_id"], 101)
            self.assertEqual(result["transport"], "actions-cache")
            self.assertTrue(result["export_needed"], "changed-source B must be eligible to replace shared A")
            run(["ccache", "--zero-stats"])
            run(command)
            output.unlink()
            stats = {key: int(value) for key, value in (line.split() for line in run(["ccache", "--print-stats"]).splitlines())}
            self.assertEqual(stats.get("cache_miss", 0), 0)
            self.assertEqual(stats.get("direct_cache_hit", 0) + stats.get("preprocessed_cache_hit", 0), 1)
        self.assertEqual(self.downloads, [])
        self.assertEqual(config.read_bytes(), trusted)

    def test_cross_branch_and_squash_master_replay_is_bound_to_inputs_not_application_sha(self):
        result = self.restore()
        self.assertTrue(result["restored"])
        self.assertEqual(result["artifact_id"], 101)
        self.assertFalse(result["export_needed"])
        self.assertEqual((self.root / "restored/a/b/entryR").read_bytes(), b"compiled objects")
        self.assertEqual(self.downloads, [101])

    def test_input_and_policy_changes_change_identity(self):
        changed = copy.deepcopy(self.descriptor)
        changed["compatibility"]["flags"] = ["-fsanitize=address"]
        self.assertNotEqual(self.context["key"], transport.context(changed, self.repo)["key"])
        (self.repo / transport.POLICY_FILES[0]).write_text("new policy")
        self.assertNotEqual(self.context["key"], transport.context(self.descriptor, self.repo)["key"])

    def test_crlf_checkout_matches_lf_git_policy_blobs_and_restores(self):
        committed = copy.deepcopy(self.context)
        for name in transport.POLICY_FILES:
            path = self.repo / name
            path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        self.context = transport.context(self.descriptor, self.repo)
        self.assertEqual(self.context, committed)
        def lf_server(endpoint):
            result = self.api(endpoint)
            if "/contents/" in endpoint:
                result["sha"] = committed["policy_files"][result["path"]]
            return result
        self.assertTrue(self.restore(metadata=lf_server)["restored"])

    def test_policy_normalization_does_not_hide_code_changes_or_arbitrary_encodings(self):
        path = self.repo / transport.POLICY_FILES[0]
        path.write_bytes(b"changed runtime policy\r\n")
        self.assertNotEqual(self.context["key"], transport.context(self.descriptor, self.repo)["key"])
        for data in (b"invalid UTF-8: \xff\n", b"bare\rreturn\n", b"nul\x00byte\n"):
            with self.subTest(data=data):
                path.write_bytes(data)
                with self.assertRaisesRegex(ValueError, "policy"):
                    transport.context(self.descriptor, self.repo)

    def test_failed_unrelated_workflow_does_not_discard_successful_validated_lane(self):
        self.run["conclusion"] = self.attempt["conclusion"] = "failure"
        self.assertTrue(self.restore()["restored"])

    def test_failed_job_step_cancelled_or_running_context_never_downloads(self):
        mutations = [(self.job, "conclusion", "failure"),
                     (self.job, "conclusion", "cancelled"),
                     (self.job, "status", "in_progress"),
                     (self.job["steps"][0], "conclusion", "skipped"),
                     (self.run, "status", "in_progress"),
                     (self.attempt, "status", "in_progress")]
        for target, field, bad in mutations:
            with self.subTest(field=field, bad=bad):
                old = target[field]
                target[field] = bad
                self.assertFalse(self.restore()["restored"])
                self.assertEqual(self.downloads, [])
                target[field] = old

    def test_cancelled_aggregate_preserves_successful_validated_lane_and_reports_conclusion(self):
        self.run["conclusion"] = self.attempt["conclusion"] = "cancelled"
        result = self.restore()
        self.assertTrue(result["restored"])
        self.assertEqual(result["aggregate_conclusion"], "cancelled")
        self.assertEqual(result["producer_attempt_conclusion"], "cancelled")
        self.assertEqual(result["producer_job_conclusion"], "success")

    def test_newer_failed_producer_attempt_disqualifies_stale_successful_snapshot(self):
        self.run.update(run_attempt=2, conclusion="failure")
        latest_job = {**self.job, "run_attempt": 2, "conclusion": "failure"}
        def latest(endpoint):
            if endpoint.endswith("/attempts/2"):
                return {**self.run, "run_started_at": "2026-10-18T10:10:00Z"}
            if "/attempts/2/jobs?" in endpoint:
                return {"total_count": 1, "jobs": [latest_job]}
            return self.api(endpoint)
        self.assertFalse(self.restore(metadata=latest)["restored"])
        self.assertEqual(self.downloads, [])

    def test_unrelated_latest_attempt_does_not_hide_intermediate_producer_failure(self):
        self.run.update(run_attempt=3, conclusion="cancelled")
        def latest(endpoint):
            for number in (2, 3):
                if endpoint.endswith(f"/attempts/{number}"):
                    return {**self.run, "run_attempt": number, "run_started_at": "2026-10-18T10:10:00Z"}
                if f"/attempts/{number}/jobs?" in endpoint:
                    jobs = [] if number == 3 else [{**self.job, "run_attempt": 2, "conclusion": "failure"}]
                    return {"total_count": len(jobs), "jobs": jobs}
            return self.api(endpoint)
        self.assertFalse(self.restore(metadata=latest)["restored"])
        self.assertEqual(self.downloads, [])

    def test_unrelated_cancelled_rerun_keeps_earlier_successful_lane_snapshot(self):
        self.run.update(run_attempt=2, conclusion="cancelled")
        def latest(endpoint):
            if endpoint.endswith("/attempts/2"):
                return {**self.run, "run_started_at": "2026-10-18T10:10:00Z"}
            if "/attempts/2/jobs?" in endpoint:
                return {"total_count": 0, "jobs": []}
            return self.api(endpoint)
        result = self.restore(metadata=latest)
        self.assertTrue(result["restored"])
        self.assertEqual(result["producer"]["run_attempt"], 1)
        self.assertEqual(result["aggregate_conclusion"], "cancelled")

    def test_fork_origin_and_unknown_event_are_not_admitted(self):
        self.artifact["workflow_run"]["head_repository_id"] = 99
        self.assertFalse(self.restore()["restored"])
        self.artifact["workflow_run"]["head_repository_id"] = 12
        self.run["event"] = "workflow_dispatch"
        self.assertFalse(self.restore()["restored"])
        self.assertEqual(self.downloads, [])

    def test_base_only_policy_change_uses_authenticated_compiled_merge_not_pr_head(self):
        def policy_at_revision(endpoint):
            result = self.api(endpoint)
            if "/contents/" in endpoint and not endpoint.endswith("ref=" + "b" * 40):
                result["sha"] = "e" * 40  # PR head lacks the base branch's policy update.
            return result
        result = self.restore(metadata=policy_at_revision)
        self.assertTrue(result["restored"])
        self.assertEqual(result["producer"]["head_sha"], "a" * 40)
        self.assertEqual(result["producer"]["checkout_sha"], "b" * 40)
        self.assertEqual(result["checkout_proof"], "actions-local-reusable-workflow")

    def test_fabricated_manifest_checkout_or_tree_is_rejected(self):
        original = json.loads((self.bundle / "manifest.json").read_text())
        for field in ("checkout_sha", "tree"):
            with self.subTest(field=field):
                manifest = copy.deepcopy(original)
                manifest["origin"][field] = "e" * 40
                (self.bundle / "manifest.json").write_text(json.dumps(manifest))
                with zipfile.ZipFile(self.zipped, "w") as archive:
                    for file in sorted(self.bundle.iterdir()):
                        archive.write(file, file.name)
                self.artifact.update(size_in_bytes=self.zipped.stat().st_size,
                                     digest="sha256:" + hashlib.sha256(self.zipped.read_bytes()).hexdigest())
                with self.assertRaisesRegex(ValueError, "producer mismatch"):
                    self.restore()
                self.assertFalse((self.root / "restored").exists())

    def test_missing_checkout_proof_is_a_miss_not_manifest_trust(self):
        self.run["referenced_workflows"] = []
        self.attempt["referenced_workflows"] = []
        self.assertFalse(self.restore()["restored"])
        self.assertEqual(self.downloads, [])

    def test_original_attempt_checkout_proof_is_not_replaced_by_latest_rerun_refs(self):
        self.run.update(run_attempt=2, conclusion="cancelled")
        self.run["referenced_workflows"] = [{"path": transport.REPOSITORY + "/" + name + "@" + "e" * 40,
                                            "sha": "e" * 40, "ref": "refs/pull/80/merge"} for name in LOCAL_CALLS]
        def latest(endpoint):
            if endpoint.endswith("/attempts/2"):
                return {**self.run, "run_started_at": "2026-10-18T10:10:00Z"}
            if "/attempts/2/jobs?" in endpoint:
                return {"total_count": 0, "jobs": []}
            return self.api(endpoint)
        result = self.restore(metadata=latest)
        self.assertTrue(result["restored"])
        self.assertEqual(result["producer"]["checkout_sha"], "b" * 40)
        self.assertTrue(any("/git/commits/" + "b" * 40 in call for call in self.calls))
        self.assertFalse(any("/git/commits/" + "e" * 40 in call for call in self.calls))

    def test_one_recognized_optional_local_call_is_sufficient_checkout_evidence(self):
        self.attempt["referenced_workflows"] = self.attempt["referenced_workflows"][:1]
        self.assertTrue(self.restore()["restored"])

    def test_older_attempt_without_its_own_checkout_proof_cannot_borrow_latest_refs(self):
        self.run.update(run_attempt=2, conclusion="cancelled")
        self.attempt.pop("referenced_workflows")
        def latest(endpoint):
            if endpoint.endswith("/attempts/2"):
                return {**self.run, "run_started_at": "2026-10-18T10:10:00Z"}
            if "/attempts/2/jobs?" in endpoint:
                return {"total_count": 0, "jobs": []}
            return self.api(endpoint)
        self.assertFalse(self.restore(metadata=latest)["restored"])
        self.assertEqual(self.downloads, [])

    def test_conflicting_checkout_references_and_wrong_merge_parent_fail_closed(self):
        references = copy.deepcopy(self.attempt["referenced_workflows"])
        references[0].update(sha="e" * 40, path=transport.REPOSITORY + "/" + LOCAL_CALLS[0] + "@" + "e" * 40)
        self.attempt["referenced_workflows"] = references
        with self.assertRaisesRegex(ValueError, "checkout provenance"):
            self.restore()
        self.attempt["referenced_workflows"] = self.run["referenced_workflows"]
        def wrong_parent(endpoint):
            value = self.api(endpoint)
            if "/git/commits/" in endpoint:
                value["parents"][1]["sha"] = "e" * 40
            return value
        with self.assertRaisesRegex(ValueError, "producer PR head"):
            self.restore(metadata=wrong_parent)
        self.assertEqual(self.downloads, [])

    def test_local_call_topology_ignores_comments_external_refs_and_script_text(self):
        local = LOCAL_CALLS[0]
        fake = ("jobs:\n  fake:\n    # uses: ./" + local + "\n"
                "    uses: " + transport.REPOSITORY + "/" + local + "@main\n"
                "    steps:\n      - run: |\n        uses: ./" + local + "\n")
        self.assertEqual(transport.local_workflow_calls(fake.encode()), [])
        self.assertEqual(transport.local_workflow_calls(("jobs:\n  real:\n    uses: './" + local + "'\n").encode()), [local])

    def test_push_producer_uses_authenticated_push_commit_without_reusable_refs(self):
        self.origin.update(event="push", head_sha="b" * 40)
        self.run.update(event="push", head_sha="b" * 40, head_branch="master", referenced_workflows=[])
        self.attempt.update(event="push", head_sha="b" * 40, head_branch="master", referenced_workflows=[])
        self.job["head_sha"] = "b" * 40
        self.artifact["workflow_run"].update(head_sha="b" * 40, head_branch="master")
        manifest = json.loads((self.bundle / "manifest.json").read_text())
        manifest["origin"] = self.origin
        (self.bundle / "manifest.json").write_text(json.dumps(manifest))
        with zipfile.ZipFile(self.zipped, "w") as archive:
            for file in sorted(self.bundle.iterdir()):
                archive.write(file, file.name)
        self.artifact.update(size_in_bytes=self.zipped.stat().st_size,
                             digest="sha256:" + hashlib.sha256(self.zipped.read_bytes()).hexdigest())
        result = self.restore()
        self.assertTrue(result["restored"])
        self.assertEqual(result["checkout_proof"], "actions-push-head")

    def test_unrelated_reusable_references_cannot_authorize_checkout(self):
        refs = [{"path": "outsider/repo/.github/workflows/build.yml@" + "b" * 40,
                 "sha": "b" * 40, "ref": "refs/pull/80/merge"}]
        self.run["referenced_workflows"] = self.attempt["referenced_workflows"] = refs
        self.assertFalse(self.restore()["restored"])
        self.assertEqual(self.downloads, [])

    def test_policy_bytes_are_checked_against_server_commit_not_manifest_claim(self):
        def changed_policy(endpoint):
            result = self.api(endpoint)
            if "/contents/" in endpoint:
                result["sha"] = "d" * 40
            return result
        with self.assertRaisesRegex(ValueError, "policy"):
            self.restore(metadata=changed_policy)
        self.assertEqual(self.downloads, [])

    def test_successful_new_attempt_cannot_authorize_failed_old_attempt(self):
        self.run["run_attempt"] = 2
        self.job["conclusion"] = "failure"
        self.assertFalse(self.restore()["restored"])
        self.assertEqual(self.downloads, [])

    def test_selected_digest_and_provenance_substitution_fail_closed(self):
        self.artifact["digest"] = "sha256:" + "0" * 64
        with self.assertRaisesRegex(ValueError, "digest"):
            self.restore()
        self.assertFalse((self.root / "restored").exists())

    def test_archive_configuration_is_not_imported(self):
        (self.cache / "ccache.conf").write_text("prefix = arbitrary-program\n")
        transport.pack(self.cache, self.root / "clean-bundle", self.context, self.origin)
        with tarfile.open(self.root / "clean-bundle/objects.tar") as archive:
            self.assertNotIn("ccache.conf", archive.getnames())

    def test_legacy_cache_config_is_omitted_and_external_config_survives_replay(self):
        # Legacy --max-size writes leave cache-local configuration behind. It is
        # metadata, not compiler-object data, and must never enter the envelope.
        (self.cache / "ccache.conf").write_text("max_size = 250M\ncompression = true\nprefix_command = must-not-run\n")
        external = self.root / "trusted-config/ccache.conf"
        external.parent.mkdir()
        trusted = b"max_size = 500M\ncompiler_check = content\nprefix_command =\n"
        external.write_bytes(trusted)
        bundle = self.root / "legacy-bundle"
        manifest = transport.pack(self.cache, bundle, self.context, self.origin)
        self.assertEqual(manifest["object_count"], 1)
        with tarfile.open(bundle / "objects.tar") as archive:
            self.assertEqual(archive.getnames(), ["a/b/entryR"])
        with zipfile.ZipFile(self.zipped, "w") as archive:
            for file in sorted(bundle.iterdir()):
                archive.write(file, file.name)
        self.artifact.update(size_in_bytes=self.zipped.stat().st_size,
                             digest="sha256:" + hashlib.sha256(self.zipped.read_bytes()).hexdigest())
        with mock.patch.dict(os.environ, {"CCACHE_CONFIGPATH": str(external)}):
            with mock.patch.object(transport.subprocess, "run", side_effect=AssertionError("object transport must not execute cache configuration")):
                self.assertTrue(self.restore()["restored"])
            self.assertEqual(os.environ["CCACHE_CONFIGPATH"], str(external))
        self.assertEqual(external.read_bytes(), trusted)
        self.assertFalse((self.root / "restored/ccache.conf").exists())
        self.assertEqual((self.root / "restored/a/b/entryR").read_bytes(), b"compiled objects")

    def test_symlinks_and_oversized_cache_fail_before_publication(self):
        (self.cache / "link").symlink_to(self.cache / "a/b/entryR")
        with self.assertRaisesRegex(ValueError, "symlink"):
            transport.pack(self.cache, self.root / "unsafe", self.context, self.origin)
        self.assertFalse((self.root / "unsafe").exists())
        (self.cache / "link").unlink()
        (self.cache / "large").write_bytes(b"x" * (1024 * 1024))
        with self.assertRaisesRegex(ValueError, "budget"):
            transport.pack(self.cache, self.root / "large-bundle", self.context, self.origin)

    def test_tar_traversal_and_case_aliases_are_rejected(self):
        for names in (("../escape",), ("A/file", "a/file"), ("C:stream",), ("ccache.conf",)):
            with self.subTest(names=names):
                raw = io.BytesIO()
                with tarfile.open(fileobj=raw, mode="w") as archive:
                    for name in names:
                        info = tarfile.TarInfo(name)
                        info.size = 1
                        archive.addfile(info, io.BytesIO(b"x"))
                archive_path = self.root / "unsafe.tar"
                archive_path.write_bytes(raw.getvalue())
                with self.assertRaises(ValueError):
                    transport.extract_objects(archive_path, self.root / "unsafe-extracted", self.context)
                self.assertFalse((self.root / "unsafe-extracted").exists())

    def test_candidate_name_binds_attempt_without_artifact_overwrite(self):
        self.assertNotEqual(transport.artifact_name(self.context, 1), transport.artifact_name(self.context, 2))
        self.assertNotIn(self.origin["checkout_sha"], transport.artifact_name(self.context, 1))

    def test_policy_parent_symlink_cannot_escape_repository_root(self):
        policies = self.repo / "scripts"
        outside = self.root / "outside-policy"
        policies.rename(outside)
        policies.symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "policy escapes repository"):
            transport.context(self.descriptor, self.repo)

    def test_existing_destination_and_unknown_descriptor_fields_fail_closed(self):
        (self.root / "restored").mkdir()
        with self.assertRaisesRegex(ValueError, "destination"):
            self.restore()
        changed = {**self.descriptor, "trust_arbitrary_pr": True}
        with self.assertRaisesRegex(ValueError, "descriptor"):
            transport.context(changed, self.repo)

    def test_existing_windows_allowances_are_supported_without_budget_increase(self):
        for budget in (512 * 1024 * 1024, 1024 * 1024 * 1024, 2 * 1024 * 1024 * 1024):
            with self.subTest(budget=budget):
                descriptor = {**self.descriptor, "kind": "sccache", "budget_bytes": budget}
                self.assertEqual(transport.context(descriptor, self.repo)["budget_bytes"], budget)
        with self.assertRaisesRegex(ValueError, "budget"):
            transport.context({**self.descriptor, "budget_bytes": 2 * 1024 * 1024 * 1024 + 1}, self.repo)

    def test_budget_change_is_a_distinct_bounded_snapshot_identity(self):
        changed = {**self.descriptor, "budget_bytes": 2 * 1024 * 1024}
        self.assertNotEqual(self.context["key"], transport.context(changed, self.repo)["key"])

    def test_case_aliased_parent_directories_are_rejected_before_install(self):
        archive_path = self.root / "aliases.tar"
        with tarfile.open(archive_path, "w") as archive:
            for name in ("A/one", "a/two"):
                info = tarfile.TarInfo(name)
                info.mode, info.size = 0o600, 1
                archive.addfile(info, io.BytesIO(b"x"))
        with self.assertRaisesRegex(ValueError, "alias"):
            transport.extract_objects(archive_path, self.root / "aliases", self.context)
        self.assertFalse((self.root / "aliases").exists())

    def test_current_identity_accepts_own_pr_and_master_but_not_forks_or_dispatch(self):
        repository = {"id": 12, "full_name": transport.REPOSITORY}
        event = {"repository": repository, "number": 80, "pull_request": {
            "number": 80, "head": {"sha": "a" * 40, "repo": repository}}}
        event_path = self.root / "event.json"
        env = {"GITHUB_EVENT_PATH": str(event_path), "GITHUB_REPOSITORY": transport.REPOSITORY,
               "GITHUB_REPOSITORY_ID": "12", "GITHUB_SHA": "b" * 40,
               "GITHUB_EVENT_NAME": "pull_request", "GITHUB_REF": "refs/pull/80/merge",
               "GITHUB_RUN_ID": "999", "GITHUB_RUN_ATTEMPT": "1",
               "GITHUB_WORKFLOW_REF": transport.REPOSITORY + "/" + transport.WORKFLOW + "@refs/pull/80/merge"}
        def git(root, *args):
            return "b" * 40 if args[-1] == "HEAD" else "c" * 40
        event_path.write_text(json.dumps(event))
        self.assertEqual(transport.identity(env, self.repo, git=git)["head_sha"], "a" * 40)
        event["pull_request"]["head"]["repo"] = {"id": 99, "full_name": "fork/repo"}
        event_path.write_text(json.dumps(event))
        with self.assertRaisesRegex(ValueError, "fork"):
            transport.identity(env, self.repo, git=git)
        event = {"repository": repository, "ref": "refs/heads/master", "after": "b" * 40}
        event_path.write_text(json.dumps(event))
        env.update(GITHUB_EVENT_NAME="push", GITHUB_REF="refs/heads/master")
        self.assertEqual(transport.identity(env, self.repo, git=git)["event"], "push")
        env["GITHUB_EVENT_NAME"] = "workflow_dispatch"
        with self.assertRaisesRegex(ValueError, "ordinary"):
            transport.identity(env, self.repo, git=git)

    def overlapping_artifact_pages(self, repeated=None):
        # Two uploads arrive after offset page 1: its final unrelated entry and
        # compatible artifact move onto page 2. Neither is a new artifact ID.
        first = [{"id": number, "name": "unrelated"} for number in range(1000, 1099)] + [self.artifact]
        repeated = self.artifact if repeated is None else repeated
        pages = []
        def metadata(endpoint):
            if "/artifacts?" in endpoint:
                page = int(endpoint.rsplit("=", 1)[1])
                pages.append(page)
                if page == 1:
                    return {"total_count": 101, "artifacts": copy.deepcopy(first)}
                self.assertEqual(page, 2)
                return {"total_count": 103, "artifacts": [copy.deepcopy(first[-2]), copy.deepcopy(repeated),
                                                           {"id": 999, "name": "unrelated-older"}]}
            return self.api(endpoint)
        return metadata, pages

    def test_artifact_listing_page_churn_deduplicates_identical_ids_before_candidate_admission(self):
        metadata, pages = self.overlapping_artifact_pages()
        with mock.patch.object(transport, "_producer", wraps=transport._producer) as producer:
            result = self.restore(metadata=metadata)
        self.assertTrue(result["restored"])
        self.assertEqual(result["artifact_id"], 101)
        self.assertEqual(pages, [1, 2])
        self.assertEqual(producer.call_count, 1)
        self.assertEqual(self.downloads, [101])

    def test_artifact_listing_page_churn_ignores_only_nonidentity_metadata_changes(self):
        repeated = {**self.artifact, "updated_at": "2026-10-18T10:06:00Z"}
        metadata, _ = self.overlapping_artifact_pages(repeated)
        self.assertTrue(self.restore(metadata=metadata)["restored"])
        self.assertEqual(self.downloads, [101])

    def test_artifact_listing_repeated_id_with_conflicting_identity_fails_closed(self):
        changes = ({"name": "different-artifact"}, {"size_in_bytes": self.artifact["size_in_bytes"] + 1},
                   {"digest": "sha256:" + "e" * 64}, {"created_at": "2026-10-18T10:04:00Z"},
                   {"expired": True}, {"workflow_run": {**self.artifact["workflow_run"], "head_sha": "e" * 40}})
        for change in changes:
            with self.subTest(change=change):
                metadata, _ = self.overlapping_artifact_pages({**self.artifact, **change})
                with self.assertRaisesRegex(ValueError, "(?:conflicting|duplicate).*artifact"):
                    self.restore(metadata=metadata)
                self.assertEqual(self.downloads, [])

    def test_artifact_listing_invalid_entries_and_same_page_duplicates_remain_rejected(self):
        invalid = (None, [], {}, {"id": None}, {"id": 0}, {"id": -1}, {"id": True},
                   {"id": "101"}, {"id": 101.0})
        for item in invalid:
            with self.subTest(item=item):
                with self.assertRaisesRegex(ValueError, "invalid artifact listing"):
                    self.restore(metadata=lambda endpoint: {"total_count": 1, "artifacts": [item]})
        with self.assertRaisesRegex(ValueError, "duplicate.*artifact"):
            self.restore(metadata=lambda endpoint: {"total_count": 2, "artifacts": [self.artifact, self.artifact]})
        self.assertEqual(self.downloads, [])

    def test_artifact_listing_identity_comparison_preserves_field_presence_and_types(self):
        self.assertNotEqual(transport.artifact_identity({"id": 101, "size_in_bytes": 1}),
                            transport.artifact_identity({"id": 101, "size_in_bytes": True}))
        self.assertNotEqual(transport.artifact_identity({"id": 101, "digest": None}),
                            transport.artifact_identity({"id": 101}))

    def test_artifact_listing_overlap_does_not_extend_page_or_download_limits(self):
        pages = []
        items = [{"id": number, "name": "unrelated"} for number in range(1000, 1100)]
        def metadata(endpoint):
            pages.append(endpoint)
            return {"total_count": 10000, "artifacts": copy.deepcopy(items)}
        result = self.restore(metadata=metadata)
        self.assertFalse(result["restored"])
        self.assertTrue(result["discovery_exhausted"])
        self.assertEqual(len(pages), transport.MAX_PAGES)
        self.assertEqual(self.downloads, [])

    def test_discovery_paginates_past_unrelated_newer_artifacts_and_is_bounded(self):
        def pages(endpoint):
            if "/artifacts?" in endpoint:
                if endpoint.endswith("page=1"):
                    return {"total_count": 101, "artifacts": [
                        {"id": number, "name": "unrelated"} for number in range(1000, 1100)]}
                self.assertTrue(endpoint.endswith("page=2"))
                return {"total_count": 101, "artifacts": [self.artifact]}
            return self.api(endpoint)
        self.assertTrue(self.restore(metadata=pages)["restored"])
        count = []
        def exhausted(endpoint):
            count.append(endpoint)
            page = int(endpoint.rsplit("=", 1)[1])
            return {"total_count": 10000, "artifacts": [
                {"id": page * 1000 + number, "name": "unrelated"} for number in range(100)]}
        result = transport.restore(self.context, self.root / "second", repository_id=12,
                                   current_run=999, metadata=exhausted, now=self.now)
        self.assertFalse(result["restored"])
        self.assertTrue(result["discovery_exhausted"])
        self.assertEqual(len(count), transport.MAX_PAGES)

    def test_windows_adapter_streams_with_same_byte_budget_and_reaps_process(self):
        for payload, budget, succeeds in ((b"ZIP bytes", 20, True), (b"oversized", 2, False), (b"", 2, False)):
            with self.subTest(payload=payload):
                process = mock.Mock(stdout=io.BytesIO(payload))
                process.wait.return_value = 0
                process.poll.return_value = None
                destination = self.root / ("download-" + str(budget) + str(len(payload)))
                with mock.patch.object(transport.subprocess, "Popen", return_value=process), mock.patch.dict(os.environ, {"GH_TOKEN": "token"}):
                    if succeeds:
                        transport.download_artifact_windows(101, destination, limit=budget, timeout=5)
                        self.assertEqual(destination.read_bytes(), payload)
                    else:
                        with self.assertRaises(ValueError):
                            transport.download_artifact_windows(101, destination, limit=budget, timeout=5)
                process.wait.assert_called()
                self.assertTrue(process.stdout.closed)

    def test_windows_adapter_reports_reader_failure_without_waiting_for_deadline(self):
        process = mock.Mock()
        process.stdout.read.side_effect = OSError("broken pipe")
        process.poll.return_value = None
        process.wait.return_value = 1
        with mock.patch.object(transport.subprocess, "Popen", return_value=process), mock.patch.dict(os.environ, {"GH_TOKEN": "token"}):
            with self.assertRaisesRegex(ValueError, "reader"):
                transport.download_artifact_windows(101, self.root / "broken-download", limit=100, timeout=1)
        process.stdout.close.assert_called_once()

    def test_manifest_origin_mismatch_is_not_a_fallback_miss(self):
        manifest_path = self.bundle / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["origin"]["run_attempt"] = 2
        manifest_path.write_text(json.dumps(manifest))
        with zipfile.ZipFile(self.zipped, "w") as archive:
            for file in sorted(self.bundle.iterdir()):
                archive.write(file, file.name)
        self.artifact.update(size_in_bytes=self.zipped.stat().st_size,
                             digest="sha256:" + hashlib.sha256(self.zipped.read_bytes()).hexdigest())
        with self.assertRaisesRegex(ValueError, "producer mismatch"):
            self.restore()
        self.assertFalse((self.root / "restored").exists())

    def test_export_action_is_bounded_immutable_and_avoids_redundant_upload(self):
        text = (ROOT / ".github/actions/export-compiler-object-artifact/action.yml").read_text()
        self.assertIn("retention-days: 2", text)
        self.assertIn("overwrite: false", text)
        self.assertIn("inputs.export-needed == 'true'", text)
        self.assertIn("success() && steps.pack.outputs.packed == 'true'", text)
        self.assertNotIn("secrets.", text)
        self.assertNotIn("upload-release-artifact", text)

    def test_atomic_install_restores_old_cache_if_final_rename_fails(self):
        accepted = self.root / "accepted"
        accepted.mkdir()
        (accepted / "new").write_text("new")
        original_rename = Path.rename
        def rename(path, target):
            if path == accepted:
                raise OSError("injected final rename failure")
            return original_rename(path, target)
        with mock.patch.object(Path, "rename", rename), self.assertRaises(OSError):
            transport.install(accepted, self.cache)
        self.assertEqual((self.cache / "a/b/entryR").read_bytes(), b"compiled objects")
        self.assertTrue((accepted / "new").exists())

    def test_missing_artifact_is_observable_and_authorizes_bounded_export(self):
        result = self.restore(metadata=lambda endpoint: {"total_count": 0, "artifacts": []})
        self.assertFalse(result["restored"])
        self.assertTrue(result["export_needed"])
        self.assertEqual(self.downloads, [])


if __name__ == "__main__":
    unittest.main()
