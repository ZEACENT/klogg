"""Closed-PR cache cleanup needs current, complete evidence and explicit consent."""

from __future__ import annotations

import copy
import io
import json
import pathlib
import subprocess
import sys
import unittest
from unittest import mock
from urllib.parse import parse_qs, urlsplit

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import pr_cache_cleanup as cleanup
import lint_ci_quality as lint

CONFIRMATION = "delete-closed-pr-caches"
ENVIRONMENT = {
    "GITHUB_EVENT_NAME": "workflow_dispatch",
    "GITHUB_REF": "refs/heads/master",
    "GITHUB_REPOSITORY": "ZEACENT/klogg",
}


def cache(identifier=1, ref="refs/pull/12/merge", key="intel-qt6-ccache-pr-v1-12-2"):
    return {"id": identifier, "ref": ref, "key": key, "version": "a" * 64,
            "size_in_bytes": 100, "created_at": "2026-10-01T00:00:00Z",
            "last_accessed_at": "2026-10-02T00:00:00Z"}


def pull(number=12, state="closed", merged=True):
    return {"number": number, "state": state, "merged": merged,
            "head": {"sha": "b" * 40, "ref": "feature"},
            "merge_commit_sha": "c" * 40}


def run(identifier=1, status="in_progress", **changes):
    return {"id": identifier, "status": status, "head_branch": "feature",
            "head_sha": "b" * 40, "pull_requests": [{"number": 12}], **changes}


class FakeAPI:
    repository = "ZEACENT/klogg"

    def __init__(self):
        self.caches = [cache()]
        self.pulls = {12: pull()}
        self.runs = []
        self.deleted = []
        self.calls = []
        self.before_get = lambda endpoint: None

    def get_json(self, endpoint):
        self.calls.append(endpoint)
        self.before_get(endpoint)
        path = urlsplit(endpoint).path
        query = parse_qs(urlsplit(endpoint).query)
        if path.startswith("/pulls/"):
            return copy.deepcopy(self.pulls[int(path.rsplit("/", 1)[1])])
        if path == "/actions/caches":
            field = "actions_caches"
            records = self.caches
            if "ref" in query:
                records = [item for item in records if item["ref"] == query["ref"][0]]
        elif path == "/actions/runs":
            field = "workflow_runs"
            records = [item for item in self.runs if item["status"] == query["status"][0]]
        else:
            raise AssertionError("unexpected API endpoint " + endpoint)
        page = int(query.get("page", ["1"])[0])
        return copy.deepcopy({"total_count": len(records), field: records[(page - 1) * 100:page * 100]})

    def delete_cache(self, identifier):
        self.deleted.append(identifier)


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.api = FakeAPI()

    def apply(self):
        return cleanup.cleanup(self.api, apply=True, confirmation=CONFIRMATION,
                               environment=ENVIRONMENT)

    def test_default_is_read_only_and_only_closed_exact_pr_refs_are_candidates(self):
        self.api.caches = [cache(), cache(2, "refs/pull/13/head"),
                           cache(3, "refs/pull/14/merge"), cache(4, "refs/heads/master"),
                           cache(5, "refs/heads/feature"), cache(6, "refs/pull/12/merge-extra"),
                           cache(7, "refs/pull/012/merge"), cache(8, "refs/tags/refs/pull/12/head")]
        self.api.pulls.update({13: pull(13, merged=False), 14: pull(14, "open", False)})
        result = cleanup.cleanup(self.api)
        self.assertEqual(result["mode"], "dry-run")
        self.assertEqual([item["cache"]["id"] for item in result["candidates"]], [1, 2])
        self.assertEqual(len(result["protected"]), 6)
        self.assertEqual(self.api.deleted, [])
        self.assertFalse(any("/pulls/012" in path for path in self.api.calls))

    def test_apply_deletes_exact_ids_after_fresh_metadata_checks(self):
        result = self.apply()
        self.assertEqual(result["deleted"], [1])
        self.assertEqual(self.api.deleted, [1])
        self.assertGreaterEqual(self.api.calls.count("/pulls/12"), 2)
        self.assertTrue(any("ref=refs%2Fpull%2F12%2Fmerge" in path for path in self.api.calls))

    def test_confirmation_and_dispatch_on_master_are_mandatory_before_any_api_call(self):
        cases = [(False, CONFIRMATION, ENVIRONMENT),
                 (True, "yes", ENVIRONMENT), (True, "", ENVIRONMENT),
                 (True, CONFIRMATION, {})]
        cases += [(True, CONFIRMATION, {**ENVIRONMENT, field: value}) for field, value in (
            ("GITHUB_EVENT_NAME", "pull_request"), ("GITHUB_EVENT_NAME", "pull_request_target"),
            ("GITHUB_EVENT_NAME", "push"), ("GITHUB_EVENT_NAME", "schedule"),
            ("GITHUB_REF", "refs/heads/feature"), ("GITHUB_REPOSITORY", "fork/klogg"))]
        for apply, confirmation, environment in cases:
            with self.subTest(apply=apply, environment=environment, confirmation=confirmation):
                with self.assertRaises(cleanup.CleanupError):
                    cleanup.cleanup(self.api, apply=apply, confirmation=confirmation,
                                    environment=environment)
                self.assertEqual(self.api.calls, [])
                self.assertEqual(self.api.deleted, [])

    def test_all_active_statuses_protect_pr_number_ref_branch_and_head(self):
        for status in ("queued", "in_progress", "waiting", "requested", "pending"):
            matches = [run(status=status, head_branch="other", head_sha="d" * 40),
                       run(status=status, pull_requests=[], head_branch="other"),
                       run(status=status, pull_requests=[], head_branch="other", head_sha="c" * 40),
                       run(status=status, pull_requests=[], head_sha="d" * 40),
                       run(status=status, pull_requests=[], head_branch="refs/pull/12/merge", head_sha="d" * 40),
                       run(status=status, pull_requests=[], head_branch="12/head", head_sha="d" * 40)]
            for active in matches:
                with self.subTest(status=status, active=active):
                    self.api.runs = [active]
                    result = self.apply()
                    self.assertEqual(result["candidates"], [])
                    self.assertEqual(self.api.deleted, [])

    def test_completed_failed_runs_and_unrelated_active_runs_do_not_block(self):
        self.api.runs = [run(status="completed", conclusion="failure"),
                         run(2, head_branch="other", head_sha="d" * 40, pull_requests=[])]
        self.assertEqual(self.apply()["deleted"], [1])

    def test_api_failures_in_any_evidence_abort_without_deletion(self):
        for endpoint in ("/actions/caches", "/actions/runs", "/pulls/"):
            with self.subTest(endpoint=endpoint):
                def fail(path):
                    if path.startswith(endpoint):
                        raise cleanup.CleanupError("API unavailable")
                self.api.before_get = fail
                with self.assertRaisesRegex(cleanup.CleanupError, "API unavailable"):
                    self.apply()
                self.assertEqual(self.api.deleted, [])

    def test_malformed_cache_pr_or_active_run_fails_closed(self):
        cases = [("caches", "ref", None), ("caches", "id", True),
                 ("caches", "size_in_bytes", -1), ("caches", "version", ""),
                 ("caches", "created_at", "not-a-date"), ("caches", "last_accessed_at", None),
                 ("pulls", "state", "unknown"), ("pulls", "number", 99),
                 ("pulls", "head", {"sha": "bad", "ref": "feature"}),
                 ("pulls", "merged", "true"), ("pulls", "merge_commit_sha", "bad"),
                 ("runs", "pull_requests", None), ("runs", "head_sha", "bad"),
                 ("runs", "id", True), ("runs", "head_branch", None)]
        for source, field, value in cases:
            with self.subTest(source=source, field=field):
                self.api = FakeAPI()
                self.api.runs = [run()]
                record = self.api.pulls[12] if source == "pulls" else getattr(self.api, source)[0]
                record[field] = value
                with self.assertRaises(cleanup.CleanupError):
                    self.apply()
                self.assertEqual(self.api.deleted, [])

    def test_pagination_includes_late_caches_and_active_runs(self):
        self.api.caches = [cache(index, "refs/heads/master") for index in range(1, 102)] + [cache(102)]
        result = cleanup.cleanup(self.api)
        self.assertEqual([item["cache"]["id"] for item in result["candidates"]], [102])
        self.api.runs = [run(index, head_branch="other", head_sha="d" * 40, pull_requests=[])
                         for index in range(1, 102)] + [run(102)]
        self.assertEqual(self.apply()["candidates"], [])
        self.assertEqual(self.api.deleted, [])

    def test_pagination_malformed_truncated_drifting_duplicate_and_over_bound_fail_closed(self):
        cases = [{"total_count": 101, "actions_caches": [cache()]},
                 {"total_count": True, "actions_caches": []},
                 {"total_count": 10001, "actions_caches": []},
                 {"total_count": 1, "actions_caches": "spoof"},
                 {"total_count": 2, "actions_caches": [cache(), cache()]}, {}]
        for response in cases:
            with self.subTest(response=response):
                with mock.patch.object(self.api, "get_json", return_value=response):
                    with self.assertRaises(cleanup.CleanupError):
                        self.apply()
                self.assertEqual(self.api.deleted, [])
        first = {"total_count": 101, "actions_caches": [cache(i) for i in range(1, 101)]}
        for second in ({"total_count": 102, "actions_caches": [cache(101), cache(102)]},
                       {"total_count": 101, "actions_caches": []},
                       {"total_count": 101, "actions_caches": [cache(1)]}):
            with self.subTest(second=second):
                with mock.patch.object(self.api, "get_json", side_effect=[first, second]):
                    with self.assertRaises(cleanup.CleanupError):
                        self.apply()
                self.assertEqual(self.api.deleted, [])

    def test_recheck_aborts_reopened_changed_head_or_new_active_run(self):
        for change in ("reopen", "head", "active", "api-error"):
            with self.subTest(change=change):
                self.api = FakeAPI()
                checks = 0
                def mutate(path):
                    nonlocal checks
                    if path == "/pulls/12":
                        checks += 1
                        if checks == 2:
                            if change == "reopen":
                                self.api.pulls[12]["state"] = "open"
                            elif change == "head":
                                self.api.pulls[12]["head"]["sha"] = "e" * 40
                            elif change == "active":
                                self.api.runs = [run()]
                            else:
                                raise cleanup.CleanupError("API unavailable")
                self.api.before_get = mutate
                with self.assertRaises(cleanup.CleanupError):
                    self.apply()
                self.assertEqual(self.api.deleted, [])

    def test_cache_disappearance_replacement_ref_or_last_access_change_aborts(self):
        for change in ("missing", "replacement", "ref", "last-access", "version"):
            with self.subTest(change=change):
                self.api = FakeAPI()
                def mutate(path):
                    if path.startswith("/actions/caches?") and "ref=" in path:
                        if change == "missing":
                            self.api.caches = []
                        elif change == "replacement":
                            self.api.caches[0]["id"] = 2
                        elif change == "ref":
                            self.api.caches[0]["ref"] = "refs/heads/master"
                        elif change == "last-access":
                            self.api.caches[0]["last_accessed_at"] = "2026-10-03T00:00:00Z"
                        else:
                            self.api.caches[0]["version"] = "e" * 64
                self.api.before_get = mutate
                with self.assertRaises(cleanup.CleanupError):
                    self.apply()
                self.assertEqual(self.api.deleted, [])

    def test_each_delete_gets_independent_recheck_and_stops_after_first_failure(self):
        self.api.caches.append(cache(2))
        original_delete = self.api.delete_cache
        def delete(identifier):
            original_delete(identifier)
            self.api.runs = [run()]
        self.api.delete_cache = delete
        with self.assertRaises(cleanup.CleanupError):
            self.apply()
        self.assertEqual(self.api.deleted, [1])

    def test_filtered_run_limit_or_unexpected_status_is_not_absence_of_active_runs(self):
        get_json = self.api.get_json
        for response in ({"total_count": 1000, "workflow_runs": []},
                         {"total_count": 1, "workflow_runs": [run(status="unknown")]},
                         {"total_count": 1, "workflow_runs": [run(pull_requests=[{"number": True}])]}):
            with self.subTest(response=response):
                def metadata(path):
                    return response if path.startswith("/actions/runs") else get_json(path)
                with mock.patch.object(self.api, "get_json", side_effect=metadata):
                    with self.assertRaises(cleanup.CleanupError):
                        self.apply()
                self.assertEqual(self.api.deleted, [])

    def test_delete_failure_aborts_without_trying_later_ids(self):
        self.api.caches.append(cache(2))
        with mock.patch.object(self.api, "delete_cache", side_effect=cleanup.CleanupError("HTTP 403")) as delete:
            with self.assertRaisesRegex(cleanup.CleanupError, "already deleted IDs: \\[\\]"):
                self.apply()
            delete.assert_called_once_with(1)

    def test_cli_default_prints_read_only_plan_and_errors_do_not_call_api(self):
        with mock.patch.object(cleanup, "GitHubAPI", return_value=self.api):
            with mock.patch("sys.stdout", new_callable=io.StringIO) as output:
                self.assertEqual(cleanup.main(["--repo", self.api.repository]), 0)
                self.assertEqual(json.loads(output.getvalue())["mode"], "dry-run")
            self.api.calls.clear()
            with mock.patch("sys.stderr", new_callable=io.StringIO) as output:
                self.assertEqual(cleanup.main(["--repo", self.api.repository, "--apply"]), 1)
                self.assertIn("exact manual confirmation", output.getvalue())
            self.assertEqual(self.api.calls, [])
            self.assertEqual(self.api.deleted, [])

    def test_platform_usage_separates_protected_master_from_eligible_pr_bytes(self):
        self.api.caches = [cache(), cache(2, "refs/heads/master", "intel-qt6-ccache-v2-3"),
                           cache(3, "refs/heads/master", "ubuntu-24.04-deb-ccache-v2-3"),
                           cache(4, "refs/pull/12/head", "windows-sccache-v1-" + "a" * 32 + "-pr-12-20261017"),
                           cache(5, "refs/heads/master", "CPM-123")]
        usage = cleanup.cleanup(self.api)["usage_by_platform"]
        self.assertEqual(usage["macos"], {"total_bytes": 200, "master_bytes": 100, "eligible_bytes": 100})
        self.assertEqual(usage["linux"]["master_bytes"], 100)
        self.assertEqual(usage["windows"]["eligible_bytes"], 100)
        self.assertEqual(usage["shared_or_unknown"]["master_bytes"], 100)
        self.assertEqual(self.api.deleted, [])


class CleanupTransportTests(unittest.TestCase):
    def test_get_is_explicit_and_delete_is_by_id_never_key_or_ref(self):
        with mock.patch.object(cleanup.subprocess, "run") as execute:
            execute.return_value = subprocess.CompletedProcess([], 0, '{"total_count":0}', '')
            api = cleanup.GitHubAPI("ZEACENT/klogg")
            self.assertEqual(api.get_json("/actions/caches"), {"total_count": 0})
            self.assertIn("GET", execute.call_args.args[0])
            api.delete_cache(12)
            self.assertIn("DELETE", execute.call_args.args[0])
            self.assertIn("/repos/ZEACENT/klogg/actions/caches/12", execute.call_args.args[0])

    def test_api_command_failure_duplicate_json_keys_and_nonobjects_fail_closed(self):
        for stdout in ('{"id":1,"id":2}', '[]', 'not json'):
            with mock.patch.object(cleanup.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, stdout, '')):
                with self.assertRaises(cleanup.CleanupError):
                    cleanup.GitHubAPI("ZEACENT/klogg").get_json("/actions/caches")
        with mock.patch.object(cleanup.subprocess, "run", side_effect=subprocess.CalledProcessError(403, [])):
            with self.assertRaises(cleanup.CleanupError):
                cleanup.GitHubAPI("ZEACENT/klogg").get_json("/actions/caches")

    def test_malformed_repository_endpoint_or_delete_identifier_never_runs_gh(self):
        with mock.patch.object(cleanup.subprocess, "run") as execute:
            for repository in ("", "owner/repo/extra", "--hostname/evil", "owner/repo?x=1"):
                with self.assertRaises(cleanup.CleanupError):
                    cleanup.GitHubAPI(repository)
            api = cleanup.GitHubAPI("ZEACENT/klogg")
            for endpoint in ("//evil.test", "https://evil.test", "/../actions/caches"):
                with self.assertRaises(cleanup.CleanupError):
                    api.get_json(endpoint)
            for identifier in (True, -1, "1?key=foo"):
                with self.assertRaises(cleanup.CleanupError):
                    api.delete_cache(identifier)
            execute.assert_not_called()


class CleanupWorkflowTests(unittest.TestCase):
    def assert_safe_workflow(self, text):
        triggers = lint.workflow_trigger_mapping(text)
        self.assertIsNotNone(triggers)
        self.assertEqual(set(triggers), {"workflow_dispatch"})
        self.assertEqual(triggers["workflow_dispatch"][0], "")
        root_permissions = [lint.strip_yaml_comment(line) for line in text.splitlines()
                            if line.startswith("permissions:")]
        self.assertEqual(root_permissions, ["permissions: {}"])
        jobs = lint.workflow_job_blocks(text)
        self.assertEqual(set(jobs), {"audit", "apply"})
        for name, block in jobs.items():
            condition = "github.event_name == 'workflow_dispatch' && github.ref == 'refs/heads/master'"
            if name == "apply":
                condition += " && inputs.apply && inputs.confirmation == 'delete-closed-pr-caches' && false"
            self.assertEqual(lint.workflow_job_direct_value(block, "if"), "${{ " + condition + " }}")
            permissions = lint.workflow_mapping_block(block, "permissions", 4)
            self.assertIsNotNone(permissions)
            self.assertEqual({key: value for key, (value, _) in permissions.items()},
                             {"actions": "write" if name == "apply" else "read",
                              "contents": "read", "pull-requests": "read"})
            steps = lint.workflow_step_blocks(block)
            checkouts = [step for step in steps if "actions/checkout@" in "\n".join(step)]
            self.assertEqual(len(checkouts), 1)
            checkout = lint.workflow_mapping_block(checkouts[0], "with", 8)
            self.assertIsNotNone(checkout)
            self.assertEqual(checkout["ref"][0], "${{ github.sha }}")
            self.assertEqual(checkout["persist-credentials"][0], "false")
        audit_run = "\n".join(lint.workflow_run_blocks("\n".join(jobs["audit"])))
        self.assertNotIn("--apply", audit_run)
        self.assertIn('python3 scripts/pr_cache_cleanup.py --repo "$GITHUB_REPOSITORY"', audit_run)
        apply_run = "\n".join(lint.workflow_run_blocks("\n".join(jobs["apply"])))
        self.assertIn('--apply --confirm "$CLEANUP_CONFIRMATION"', apply_run)
        self.assertEqual(lint.workflow_job_direct_value(jobs["apply"], "environment"), "pr-cache-cleanup")
        self.assertEqual(lint.workflow_job_direct_value(jobs["apply"], "needs"), "audit")
        inputs = lint.workflow_mapping_block(text.splitlines(), "inputs", 4)
        self.assertIsNotNone(inputs)
        apply_input = lint.workflow_mapping_block(inputs["apply"][1], "apply", 6)
        self.assertIsNotNone(apply_input)
        self.assertEqual(apply_input["type"][0], "boolean")
        self.assertEqual(apply_input["default"][0], "false")

    def workflow_text(self):
        path = ROOT / ".github/workflows/ci-cache-cleanup.yml"
        self.assertTrue(path.is_file(), "manual cleanup workflow is missing")
        return path.read_text()

    def test_manual_only_least_privilege_workflow_uses_trusted_dispatch_sha(self):
        self.assert_safe_workflow(self.workflow_text())

    def test_real_workflow_mutations_fail_closed_including_comments_and_expression_spoofs(self):
        text = self.workflow_text()
        mutations = [
            ("default: false", "default: true"),
            ("permissions: {}", "permissions: write-all"),
            ("actions: read", "actions: write"),
            ("ref: ${{ github.sha }}", "ref: ${{ github.event.pull_request.head.sha }}"),
            ("persist-credentials: false", "persist-credentials: true"),
            ("environment: pr-cache-cleanup", "# environment: pr-cache-cleanup"),
            (" && inputs.apply", " || inputs.apply"),
            (" && inputs.confirmation == 'delete-closed-pr-caches'", ""),
            (" && false", ""),
            (" && false", " && true"),
            (" && false", " || false"),
            ("github.ref == 'refs/heads/master'", "github.ref == 'refs/heads/master' || true"),
            ("workflow_dispatch:", "workflow_dispatch: [malformed]"),
            ("on:\n", "on:\non:\n"),
            ("if: ${{ github.event_name", "if: ${{ true }} # ${{ github.event_name"),
        ]
        mutations += [("  workflow_dispatch:", "  " + event + ":\n  workflow_dispatch:")
                      for event in ("pull_request", "pull_request_target", "push", "schedule")]
        for original, replacement in mutations:
            with self.subTest(replacement=replacement):
                self.assertIn(original, text)
                with self.assertRaises(AssertionError):
                    self.assert_safe_workflow(text.replace(original, replacement))


if __name__ == "__main__":
    unittest.main()
