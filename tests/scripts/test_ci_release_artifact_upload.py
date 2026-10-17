"""Offline contracts and an Actions-step surrogate for macOS release uploads."""
from __future__ import annotations

import pathlib
import re
import unittest

from test_ci_environment_workflow import QUALITY as LINT, event_expression, mutate_job

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/ci-build.yml"
ACTION = ROOT / ".github/actions/upload-release-artifact/action.yml"
COMPOSITE = "./.github/actions/upload-release-artifact"
UPLOAD = "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a"
PUBLISH_IF = ("${{ env.KLOGG_PACKAGE_ENABLED == 'true' && (github.event_name == 'push' || "
              "(github.event_name == 'workflow_dispatch' && inputs.qualification-mode == 'release')) }}")
RETRY_IF = "${{ steps.upload.outcome == 'failure' }}"
INPUTS = ("name", "path", "retention-days", "compression-level")
DIAGNOSTICS_IF = "${{ failure() && steps.macos_tests.outcome == 'failure' }}"
DIAGNOSTICS_NAME = "macos-${{ env.KLOGG_LABEL }}-test-diagnostics"
DIAGNOSTICS_PATHS = ("${{ env.KLOGG_BUILD_ROOT }}/Testing/Temporary/LastTest.log\n"
                     "~/Library/Logs/DiagnosticReports/klogg_vectorscan_tests*.ips")


def action_rows(text):
    runs = LINT.workflow_mapping_block(text.splitlines(), "runs", 0)
    if not runs or runs.get("using", (None,))[0] != "composite" or "steps" not in runs:
        raise AssertionError("missing or malformed release-upload composite")
    return [LINT.workflow_step_fields(step) for step in
            LINT.workflow_step_blocks(["runs:"] + runs["steps"][1])]


def project_condition(condition, event="push", mode="validation", enabled=True,
                      primary="skipped"):
    condition = (condition.replace("env.KLOGG_PACKAGE_ENABLED", repr(str(enabled).lower()))
                 .replace("inputs.qualification-mode", repr(mode))
                 .replace("steps.upload.outcome", repr(primary)))
    return bool(event_expression(condition, event, "off"))


def simulate_uploads(action, inputs, outcomes, prior_success=True):
    """Run parsed steps against a deterministic transport, not GitHub's service.

    Model default success() gating and the distinction between outcome and
    conclusion: continue-on-error changes the latter, not the retry predicate.
    Unknown expressions/outcomes are rejected rather than treated as success.
    """
    results = iter(outcomes)
    calls = []
    primary = "skipped"
    success = prior_success
    for fields, children in action_rows(action):
        condition = fields.get("if")
        if not success or (condition and not project_condition(condition, primary=primary)):
            continue
        if fields.get("uses") != UPLOAD:
            raise AssertionError("surrogate only supports pinned artifact uploads")
        payload = {}
        for key, value in children.get("with", {}).items():
            match = re.fullmatch(r"\$\{\{ inputs\.([\w-]+) }}", value)
            payload[key] = inputs[match[1]] if match else value
        calls.append(payload)
        outcome = next(results)
        if outcome not in ("success", "failure", "cancelled", "skipped"):
            raise AssertionError("unknown transport outcome")
        if fields.get("id") == "upload":
            primary = outcome
        if outcome == "cancelled" or (outcome == "failure" and fields.get("continue-on-error") != "true"):
            success = False
    return success, calls


class ReleaseArtifactUploadTest(unittest.TestCase):
    def setUp(self):
        self.workflow = WORKFLOW.read_text()
        self.action = ACTION.read_text() if ACTION.exists() else ""

    def assert_action_contract(self, text):
        rows = action_rows(text)
        self.assertEqual(len(rows), 2, "exactly one initial upload and one retry required")
        first, retry = rows
        self.assertEqual({key: value for key, value in first[0].items() if key != "name"}, {
            "id": "upload", "continue-on-error": "true", "uses": UPLOAD, "with": ""})
        self.assertEqual({key: value for key, value in retry[0].items() if key != "name"}, {
            "if": RETRY_IF, "uses": UPLOAD, "with": ""})
        payload = {key: "${{ inputs." + key + " }}" for key in INPUTS}
        payload["if-no-files-found"] = "error"
        self.assertEqual(first[1], {"with": payload})
        self.assertEqual(retry[1], {"with": {**payload, "overwrite": "true"}})
        declarations = LINT.workflow_mapping_block(text.splitlines(), "inputs", 0)
        self.assertEqual(set(declarations or {}), set(INPUTS))
        for key in INPUTS:
            options = LINT.workflow_mapping_block(declarations[key][1], key, 2)
            self.assertIsNotNone(options)
            self.assertEqual(options.get("required", (None,))[0], "true")
            self.assertNotIn("default", options)

    def upload_rows(self, text, job):
        return [(fields, children["with"]) for fields, children in
                (LINT.workflow_step_fields(step) for step in LINT.workflow_job_steps(text)[job])
                if children.get("with", {}).get("name", "").startswith(("packages-", "symbols-"))]

    def assert_workflow_contract(self, text):
        jobs = LINT.workflow_job_blocks(text)
        for job in ("MacPackages", "MacArmPackages", "MacSanitizers"):
            self.assertIsNone(LINT.workflow_job_direct_value(jobs[job], "continue-on-error"))
            uploads = self.upload_rows(text, job)
            self.assertEqual(len(uploads), 2)
            for (fields, payload), (kind, retention) in zip(uploads, (("packages", "7"), ("symbols", "90"))):
                self.assertEqual(fields.get("uses"), COMPOSITE,
                                 "macOS release artifacts need the bounded retry action")
                self.assertEqual(fields.get("if"), PUBLISH_IF)
                self.assertNotIn("continue-on-error", fields)
                self.assertEqual(payload, {
                    "name": kind + "-${{ env.KLOGG_ARTIFACTS_ID }}-${{ env.KLOGG_CONFIG_PACKAGE_TAG }}",
                    "path": "${{ env.KLOGG_BUILD_ROOT }}/" + kind + "/*",
                    "retention-days": retention, "compression-level": "0"})

    def test_real_workflow_has_bounded_fail_closed_release_uploads(self):
        self.assert_workflow_contract(self.workflow)
        self.assert_action_contract(self.action)

    def test_publish_event_projections_preserve_cost_and_sanitizer_gates(self):
        jobs = LINT.workflow_job_blocks(self.workflow)
        for job in ("MacPackages", "MacArmPackages", "MacSanitizers"):
            env = LINT.workflow_mapping_block(jobs[job], "env", 4)
            enabled = env["KLOGG_PACKAGE_ENABLED"][0] == "true"
            self.assertEqual(enabled, job != "MacSanitizers")
            for fields, _ in self.upload_rows(self.workflow, job):
                for event, mode, expected in (
                    ("pull_request", "release", False), ("push", "validation", True),
                    ("workflow_dispatch", "release", True),
                    ("workflow_dispatch", "validation", False),
                    ("workflow_dispatch", "", False), ("workflow_dispatch", "unknown", False),
                    ("release", "release", False), ("schedule", "release", False),
                ):
                    with self.subTest(job=job, event=event, mode=mode):
                        self.assertEqual(project_condition(fields["if"], event, mode, enabled),
                                         expected and enabled)

    def test_surrogate_recovers_once_but_persistent_failure_fails(self):
        for job in ("MacPackages", "MacArmPackages"):
            for _, inputs in self.upload_rows(self.workflow, job):
                for outcomes, expected_success, attempts in (
                    (("success",), True, 1),
                    (("failure", "success"), True, 2),
                    (("failure", "failure"), False, 2),
                    (("cancelled",), False, 1),
                ):
                    with self.subTest(job=job, payload=inputs["name"], outcomes=outcomes):
                        success, calls = simulate_uploads(self.action, inputs, outcomes)
                        self.assertEqual(success, expected_success)
                        self.assertEqual(len(calls), attempts)
                        expected = {**inputs, "if-no-files-found": "error"}
                        self.assertEqual(calls[0], expected)
                        if attempts == 2:
                            self.assertEqual(calls[1], {**expected, "overwrite": "true"})
                self.assertEqual(simulate_uploads(self.action, inputs, (), prior_success=False),
                                 (False, []))

    def test_surrogate_rejects_malformed_inputs_and_unknown_transport_results(self):
        _, inputs = self.upload_rows(self.workflow, "MacPackages")[0]
        with self.assertRaisesRegex(AssertionError, "unknown transport outcome"):
            simulate_uploads(self.action, inputs, ("unknown",))
        with self.assertRaises(KeyError):
            simulate_uploads(self.action, {key: value for key, value in inputs.items() if key != "path"},
                             ("success",))
        with self.assertRaisesRegex(AssertionError, "malformed release-upload composite"):
            simulate_uploads("runs: {}", inputs, ("success",))

    def test_retry_predicate_uses_failure_outcome_only(self):
        retry = action_rows(self.action)[1][0]
        for outcome in ("success", "failure", "skipped", "cancelled", "", "unknown"):
            with self.subTest(outcome=outcome):
                self.assertEqual(project_condition(retry["if"], primary=outcome), outcome == "failure")
        with self.assertRaises(AssertionError):
            project_condition("${{ steps.other.outcome == 'failure' }}")

    def test_action_mutations_cannot_remove_or_broaden_retry_or_mask_failure(self):
        mutations = {
            "removed retry": self.action.split("    - name: Retry", 1)[0],
            "comment spoof": "\n".join("# " + line for line in self.action.splitlines()),
            "string spoof": "description: |\n" + "\n".join("  " + line for line in self.action.splitlines()),
            "malformed runs": self.action.replace("using: composite", "using: node24"),
            "wrong payload": self.action.replace("path: ${{ inputs.path }}", "path: wrong", 1),
            "wrong retry retention": self.action.replace("overwrite: true", "overwrite: true\n        retention-days: 1"),
            "missing overwrite": self.action.replace("        overwrite: true\n", ""),
            "masked final failure": self.action.replace("    - name: Retry", "    - continue-on-error: true\n      name: Retry"),
            "retry after success": self.action.replace(RETRY_IF, "${{ steps.upload.outcome == 'success' }}"),
            "retry after skipped": self.action.replace(RETRY_IF, "${{ steps.upload.outcome != 'success' }}"),
            "unconditional retry": self.action.replace(RETRY_IF, "${{ always() }}"),
            "conclusion not outcome": self.action.replace("steps.upload.outcome", "steps.upload.conclusion"),
            "third attempt": self.action + "\n    - uses: " + UPLOAD + "\n",
            "unreviewed action": self.action.replace(UPLOAD, "actions/upload-artifact@main"),
            "missing files ignored": self.action.replace("if-no-files-found: error", "if-no-files-found: warn"),
            "initial failure not tolerated": self.action.replace("continue-on-error: true", "continue-on-error: false"),
        }
        for name, mutation in mutations.items():
            with self.subTest(mutation=name):
                self.assertNotEqual(mutation, self.action, "mutation must hit the real action")
                with self.assertRaises(AssertionError):
                    self.assert_action_contract(mutation)

    def test_workflow_mutations_cannot_bypass_retry_or_publish_gating(self):
        for old, new in (
            (COMPOSITE, UPLOAD),
            (PUBLISH_IF, "${{ always() }}"),
            ("retention-days: 90", "retention-days: 7"),
            ("symbols/*", "packages/*"),
            ("uses: " + COMPOSITE, "continue-on-error: true\n        uses: " + COMPOSITE),
        ):
            with self.subTest(old=old, new=new):
                mutation = mutate_job(self.workflow, "MacPackages", old, new)
                with self.assertRaises(AssertionError):
                    self.assert_workflow_contract(mutation)
        with self.assertRaises(ValueError):
            LINT.workflow_job_steps(self.workflow.replace("steps: *mac_steps", "steps: *missing"))

    def test_release_preflight_runs_this_contract_before_expensive_builds(self):
        rows = [LINT.workflow_step_fields(step)[0] for step in
                LINT.workflow_job_steps(self.workflow)["ReleaseQualificationPreflight"]]
        checks = [fields for fields in rows
                  if fields.get("name") == "Verify macOS upload and diagnostics contracts"]
        self.assertEqual(checks, [{"name": "Verify macOS upload and diagnostics contracts",
                                   "run": "python3 tests/scripts/test_ci_release_artifact_upload.py"}])
        jobs = LINT.workflow_job_blocks(self.workflow)
        self.assertTrue(LINT.exact_platform_fragile_steps(jobs["ReleaseQualificationPreflight"],
                                                         release_qualification=True))
        for job in ("MacPackages", "MacArmPackages", "MacSanitizers"):
            self.assertIn("ReleaseQualificationPreflight", LINT.workflow_job_direct_value(jobs[job], "needs"))

    def test_preflight_lint_rejects_removed_or_masked_upload_contract(self):
        for replacement in ("", "        # run: python3 tests/scripts/test_ci_release_artifact_upload.py",
                            "        run: python3 tests/scripts/test_ci_release_artifact_upload.py || true",
                            "        run: python3 tests/scripts/test_ci_release_artifact_upload.py\n        if: ${{ false }}",
                            "        run: python3 tests/scripts/test_ci_release_artifact_upload.py\n        continue-on-error: true"):
            with self.subTest(replacement=replacement):
                text = mutate_job(self.workflow, "ReleaseQualificationPreflight",
                                  "        run: python3 tests/scripts/test_ci_release_artifact_upload.py", replacement)
                block = LINT.workflow_job_blocks(text)["ReleaseQualificationPreflight"]
                self.assertFalse(LINT.exact_platform_fragile_steps(block, release_qualification=True))

    def assert_diagnostics_contract(self, text):
        for job in ("MacPackages", "MacArmPackages", "MacSanitizers"):
            rows = [LINT.workflow_step_fields(step) for step in LINT.workflow_job_steps(text)[job]]
            test_indices = [index for index, (fields, _) in enumerate(rows)
                            if fields.get("uses") == "./.github/actions/agent-run-tests"]
            self.assertEqual(len(test_indices), 1, "tests must run once, without retry")
            index = test_indices[0]
            self.assertEqual(rows[index][0], {
                "uses": "./.github/actions/agent-run-tests", "id": "macos_tests"})
            diagnostics = [(offset, fields, children) for offset, (fields, children) in enumerate(rows)
                           if children.get("with", {}).get("name") == DIAGNOSTICS_NAME]
            self.assertEqual(len(diagnostics), 1, "test failures need one narrow diagnostics upload")
            offset, fields, children = diagnostics[0]
            self.assertEqual(offset, index + 1, "collect diagnostics before skipped package steps")
            self.assertEqual(fields, {"name": "Upload macOS test diagnostics", "if": DIAGNOSTICS_IF,
                                      "uses": UPLOAD, "with": ""})
            self.assertEqual(children, {"with": {
                "name": DIAGNOSTICS_NAME, "path": DIAGNOSTICS_PATHS,
                "if-no-files-found": "ignore", "retention-days": "7"}})
            self.assertEqual(rows[offset + 1][0].get("uses"), "./.github/actions/agent-package-mac")

    def test_macos_test_failures_preserve_narrow_diagnostics_before_packaging(self):
        self.assert_diagnostics_contract(self.workflow)

    def test_diagnostics_event_projections_require_actual_test_failure(self):
        for job in ("MacPackages", "MacArmPackages", "MacSanitizers"):
            rows = [LINT.workflow_step_fields(step) for step in LINT.workflow_job_steps(self.workflow)[job]]
            fields = next(fields for fields, children in rows
                          if children.get("with", {}).get("name") == DIAGNOSTICS_NAME)
            for event in ("pull_request", "push", "workflow_dispatch"):
                for mode in ("validation", "release"):
                    for status, outcome in (("success", "success"), ("failure", "failure"),
                                            ("failure", "skipped"), ("failure", "success"),
                                            ("cancelled", "cancelled"), ("cancelled", "failure"),
                                            ("success", "skipped")):
                        with self.subTest(job=job, event=event, mode=mode, status=status, outcome=outcome):
                            condition = (fields["if"].replace("failure()", repr(status == "failure"))
                                         .replace("steps.macos_tests.outcome", repr(outcome)))
                            self.assertEqual(project_condition(condition, event, mode),
                                             status == "failure" and outcome == "failure")

    def test_diagnostics_mutations_cannot_upload_unrelated_data_or_hide_test_failure(self):
        for old, new in (
            (DIAGNOSTICS_IF, "${{ failure() }}"),
            (DIAGNOSTICS_IF, "${{ always() }}"),
            (DIAGNOSTICS_IF, "${{ github.event_name == 'push' && failure() }}"),
            ("id: macos_tests", "id: renamed_tests"),
            ("id: macos_tests", "id: macos_tests\n        continue-on-error: true"),
            ("id: macos_tests", "id: macos_tests\n        if: ${{ github.event_name == 'push' }}"),
            ("klogg_vectorscan_tests*.ips", "*.ips"),
            ("Testing/Temporary/LastTest.log", "**/*"),
            ("if-no-files-found: ignore", "if-no-files-found: warn"),
            ("name: " + DIAGNOSTICS_NAME, "name: diagnostics"),
        ):
            with self.subTest(old=old, new=new):
                mutation = mutate_job(self.workflow, "MacPackages", old, new)
                with self.assertRaises(AssertionError):
                    self.assert_diagnostics_contract(mutation)

    def test_composite_remains_visible_to_artifact_ancestry_and_publish_lint(self):
        records = LINT.workflow_artifact_records(self.workflow)
        for job in ("MacPackages", "MacArmPackages"):
            for _, payload in self.upload_rows(self.workflow, job):
                self.assertIn(("uploads", payload["name"], PUBLISH_IF.lower()), records[job])
        for kind in ("packages", "symbols"):
            good = ("jobs:\n  MacPackages:\n    steps:\n      - uses: " + COMPOSITE +
                    "\n        if: " + PUBLISH_IF + "\n        with:\n          name: " + kind +
                    "-test\n          path: build_root/" + kind + "/*\n")
            message = "MacPackages package artifact upload must run only for publishable events"
            self.assertNotIn(message, LINT.ci_build_workflow_issues(good))
            self.assertIn(message, LINT.ci_build_workflow_issues(good.replace("        if: " + PUBLISH_IF + "\n", "")))


if __name__ == "__main__":
    unittest.main()
