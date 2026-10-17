"""The manual source probe authenticates metadata without acquiring any payload."""

from __future__ import annotations

import io
import json
import os
import pathlib
import subprocess
import sys
import unittest
from unittest import mock

from test_ci_environment_source_cache import SourceCacheFixture

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import ci_environment_source_cache as cache
import lint_ci_quality as quality
import probe_ci_environment_source as probe


class SourceMetadataProbeTest(SourceCacheFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        environment = mock.patch.dict(os.environ, {
            "GITHUB_ACTIONS": "true",
            "GITHUB_EVENT_NAME": "workflow_dispatch",
            "GITHUB_REPOSITORY": cache.REPOSITORY,
            "GITHUB_REF": "refs/heads/worktree-master-ci-fail",
            "GITHUB_SHA": self.current,
            "GITHUB_RUN_ID": "789",
            "GITHUB_RUN_ATTEMPT": "1",
            "KLOGG_PROBE_DEFAULT_BRANCH": "master",
            "GH_TOKEN": "VERY_SECRET_FIXTURE_TOKEN",
        }, clear=True)
        environment.start()
        self.addCleanup(environment.stop)

    def invoke(self, *, metadata=None):
        arguments = ["--repo-root", str(self.repo), "--expected-source-sha", self.current,
                     "--prior-sha", self.prior, "--run-id", str(self.run_id),
                     "--run-attempt", str(self.attempt), "--artifact-id", str(self.artifact_id)]
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(cache, "_metadata", side_effect=metadata or self.metadata), \
                mock.patch.object(cache, "_git", side_effect=self.git), \
                mock.patch.object(cache, "download_artifact_zip",
                                  side_effect=AssertionError("probe acquired a ZIP")) as download, \
                mock.patch.object(cache, "import_source_cache",
                                  side_effect=AssertionError("probe imported source")) as acquire, \
                mock.patch.object(cache, "_offline_validator",
                                  side_effect=AssertionError("probe verified payload")) as validate, \
                mock.patch.object(sys, "stdout", stdout), mock.patch.object(sys, "stderr", stderr):
            result = probe.main(arguments)
        download.assert_not_called()
        acquire.assert_not_called()
        validate.assert_not_called()
        return result, stdout.getvalue(), stderr.getvalue()

    def test_metadata_eligibility_reuses_original_provenance_without_payload_authority(self):
        result, stdout, stderr = self.invoke()
        self.assertEqual(result, 0)
        self.assertEqual(stderr, "")
        report = json.loads(stdout)
        self.assertEqual(report["status"], "metadata-eligible")
        self.assertEqual(report["scope"], "source-artifact-metadata-only")
        self.assertIs(report["qualified"], False)
        self.assertIs(report["archive_downloaded"], False)
        self.assertEqual(report["metadata_requests"], 4)
        self.assertEqual(len(self.metadata_calls), 4)
        self.assertEqual(report["probe"], {"source_sha": self.current,
                                         "run_id": 789, "run_attempt": 1})
        self.assertEqual(report["transport"]["prior_sha"], self.prior)
        self.assertEqual(report["transport"]["run_attempt"], self.attempt)
        self.assertEqual(report["transport"]["artifact_id"], self.artifact_id)
        self.assertEqual(report["transport"]["digest"], self.zip_digest)
        self.assertNotIn("VERY_SECRET", stdout)
        self.assertEqual(probe.MAX_METADATA_REQUESTS, 4)

    def test_invalid_dispatch_is_rejected_before_git_or_metadata(self):
        changes = (
            ("GITHUB_ACTIONS", "false"),
            ("GITHUB_EVENT_NAME", "pull_request"),
            ("GITHUB_EVENT_NAME", "push"),
            ("GITHUB_EVENT_NAME", "schedule"),
            ("GITHUB_REPOSITORY", "elsewhere/klogg"),
            ("GITHUB_REF", "refs/heads/master"),
            ("GITHUB_REF", "refs/tags/test"),
            ("GITHUB_SHA", "c" * 40),
            ("GITHUB_RUN_ATTEMPT", "2"),
            ("GITHUB_RUN_ID", "not-an-id"),
            ("KLOGG_PROBE_DEFAULT_BRANCH", ""),
            ("GH_TOKEN", ""),
        )
        for key, value in changes:
            with self.subTest(field=key, value_length=len(value)), \
                    mock.patch.dict(os.environ, {key: value}):
                result, stdout, stderr = self.invoke()
                self.assertNotEqual(result, 0)
                self.assertEqual(self.metadata_calls, [])
                self.assertEqual(self.git_calls, [])
                self.assertNotIn("VERY_SECRET", stdout + stderr)

    def test_original_producer_validation_remains_mandatory(self):
        self.jobs["total_count"] += 1
        result, stdout, stderr = self.invoke()
        self.assertNotEqual(result, 0)
        self.assertEqual(stdout, "")
        self.assertIn("producer job list is incomplete", stderr)
        self.assertEqual(len(self.metadata_calls), 4)

    def test_api_failure_is_typed_private_and_never_retried(self):
        original_metadata = cache._metadata
        error = subprocess.CalledProcessError(
            1, ["gh", "api", "VERY_SECRET_COMMAND"], output=b"VERY_SECRET_BODY",
            stderr=b"gh: VERY_SECRET_MESSAGE (HTTP 403)\n")
        with mock.patch.object(cache.transport.subprocess, "run", side_effect=error) as request:
            result, stdout, stderr = self.invoke(metadata=original_metadata)
        self.assertNotEqual(result, 0)
        self.assertEqual(stdout, "")
        self.assertIn("endpoint=artifact", stderr)
        self.assertIn("artifact=456", stderr)
        self.assertIn("failure=exit", stderr)
        self.assertIn("http=403", stderr)
        self.assertNotIn("VERY_SECRET", stderr)
        request.assert_called_once()

    def test_malformed_cli_arguments_do_not_echo_user_values(self):
        stderr = io.StringIO()
        with mock.patch.object(sys, "stderr", stderr), \
                self.assertRaises(SystemExit) as caught:
            probe.main(["--run-id", "VERY_SECRET_INPUT"])
        self.assertEqual(caught.exception.code, 2)
        self.assertNotIn("VERY_SECRET", stderr.getvalue())
        self.assertEqual(self.metadata_calls, [])
        self.assertEqual(self.git_calls, [])

    def test_future_authentication_cannot_issue_a_fifth_request(self):
        def expanded_authentication(*args, metadata, **kwargs):
            for _ in range(5):
                metadata("repos/ZEACENT/klogg/actions/artifacts/456")
            return {}

        with mock.patch.object(cache, "authenticate_artifact", side_effect=expanded_authentication):
            result, stdout, stderr = self.invoke()
        self.assertNotEqual(result, 0)
        self.assertEqual(stdout, "")
        self.assertIn("metadata request limit", stderr)
        self.assertEqual(len(self.metadata_calls), 4)


class SourceMetadataWorkflowTest(unittest.TestCase):
    def assert_probe_contract(self, text):
        blocks = quality.workflow_job_blocks(text)
        self.assertEqual(set(blocks), {"platform-fragile", "source-metadata-probe"})
        job = blocks["source-metadata-probe"]
        self.assertTrue(quality.workflow_job_direct_fields_are_unique(job))
        direct = {entry.group("key") for line in job
                  if (entry := quality.KEY_VALUE_RE.match(line)) is not None
                  and len(entry.group("indent")) == 4}
        self.assertEqual(direct, {"name", "needs", "if", "runs-on", "permissions",
                                  "timeout-minutes", "steps"})
        self.assertEqual(quality.workflow_job_needs(text)["source-metadata-probe"],
                         {"platform-fragile"})
        condition = quality.workflow_job_direct_value(job, "if")
        self.assertIsNotNone(condition)
        self.assertEqual(" ".join(condition.split()), " && ".join((
            "github.event_name == 'workflow_dispatch'",
            "github.repository == 'ZEACENT/klogg'",
            "inputs.source_metadata_probe",
            "inputs.expected_source_sha == github.sha",
            "github.run_attempt == '1'",
            "startsWith(github.ref, 'refs/heads/')",
            "github.ref != format('refs/heads/{0}', github.event.repository.default_branch)",
        )))
        self.assertEqual(quality.workflow_job_direct_value(job, "runs-on"), "ubuntu-24.04")
        self.assertEqual(quality.workflow_job_direct_value(job, "timeout-minutes"), "5")
        permissions = quality.workflow_mapping_block(job, "permissions", 4)
        self.assertIsNotNone(permissions)
        self.assertEqual({key: value for key, (value, _) in permissions.items()},
                         {"contents": "read", "actions": "read"})

        steps = [quality.workflow_step_fields(step)
                 for step in quality.workflow_step_blocks(job)]
        self.assertEqual(len(steps), 2)
        ordinary_checkout = quality.workflow_step_fields(
            quality.workflow_step_blocks(blocks["platform-fragile"])[0])[0]["uses"]
        self.assertEqual(steps[0], ({"uses": ordinary_checkout, "with": ""}, {"with": {
            "persist-credentials": "false", "fetch-depth": "0", "ref": "${{ github.sha }}",
        }}))
        fields, children = steps[1]
        self.assertEqual(set(fields), {"name", "env", "run"})
        self.assertEqual(fields["run"].strip(), "\n".join((
            "python3 scripts/probe_ci_environment_source.py \\",
            '--expected-source-sha "$EXPECTED_SOURCE_SHA" \\',
            '--prior-sha "$PRIOR_SOURCE_SHA" \\',
            '--run-id "$PRIOR_SOURCE_RUN_ID" \\',
            '--run-attempt "$PRIOR_SOURCE_RUN_ATTEMPT" \\',
            '--artifact-id "$PRIOR_SOURCE_ARTIFACT_ID"',
        )))
        self.assertEqual(children, {"env": {
            "GH_TOKEN": "${{ github.token }}",
            "KLOGG_PROBE_DEFAULT_BRANCH": "${{ github.event.repository.default_branch }}",
            "EXPECTED_SOURCE_SHA": "${{ inputs.expected_source_sha }}",
            "PRIOR_SOURCE_SHA": "${{ inputs.prior_source_sha }}",
            "PRIOR_SOURCE_RUN_ID": "${{ inputs.prior_source_run_id }}",
            "PRIOR_SOURCE_RUN_ATTEMPT": "${{ inputs.prior_source_run_attempt }}",
            "PRIOR_SOURCE_ARTIFACT_ID": "${{ inputs.prior_source_artifact_id }}",
        }})

        triggers = quality.workflow_trigger_mapping(text)
        self.assertIsNotNone(triggers)
        self.assertEqual(set(triggers), {"pull_request", "push", "workflow_dispatch"})
        inputs = quality.workflow_mapping_block(triggers["workflow_dispatch"][1], "inputs", 4)
        self.assertIsNotNone(inputs)
        expected_inputs = {"source_metadata_probe", "expected_source_sha", "prior_source_sha",
                           "prior_source_run_id", "prior_source_run_attempt",
                           "prior_source_artifact_id"}
        self.assertEqual(set(inputs), expected_inputs)
        for name, (_, block) in inputs.items():
            fields = quality.workflow_mapping_block(block, name, 6)
            self.assertIsNotNone(fields)
            expected_type = "boolean" if name == "source_metadata_probe" else "string"
            self.assertEqual(fields["type"][0], expected_type)
            self.assertEqual(fields["required"][0], "false")
            self.assertEqual(fields["default"][0], "false" if expected_type == "boolean" else "")

    def test_registered_manual_route_is_default_off_read_only_and_exact_source(self):
        self.assert_probe_contract((ROOT / ".github/workflows/lint.yml").read_text())

    def test_real_route_guard_credential_and_acquisition_mutations_are_rejected(self):
        text = (ROOT / ".github/workflows/lint.yml").read_text()
        self.assert_probe_contract(text)
        mutations = (
            ("github.event_name == 'workflow_dispatch'", "github.event_name == 'pull_request'"),
            ("inputs.source_metadata_probe &&", "true &&"),
            ("inputs.expected_source_sha == github.sha", "true"),
            ("github.run_attempt == '1'", "github.run_attempt == '2'"),
            ("github.ref != format(", "github.ref == format("),
            ("needs: [platform-fragile]", "needs: []"),
            ("      actions: read", "      actions: write"),
            ("      actions: read", "      # actions: read\n      actions: write"),
            ("      actions: read", "      actions: read\n      actions: read"),
            ("        type: boolean", "        type: string"),
            ("        default: false", "        default: true"),
            ("    timeout-minutes: 5", "    timeout-minutes: 5\n    continue-on-error: true"),
            ("GH_TOKEN: ${{ github.token }}", "GH_TOKEN: ${{ secrets.KLOGG_GITHUB_TOKEN }}"),
            ("python3 scripts/probe_ci_environment_source.py",
             "python3 scripts/ci_environment_source_cache.py"),
            ("          fetch-depth: 0", "          fetch-depth: 1"),
            ("          ref: ${{ github.sha }}", "          ref: master"),
        )
        for old, new in mutations:
            with self.subTest(mutation=old):
                self.assertIn(old, text)
                with self.assertRaises(AssertionError):
                    self.assert_probe_contract(text.replace(old, new, 1))


if __name__ == "__main__":
    unittest.main()
