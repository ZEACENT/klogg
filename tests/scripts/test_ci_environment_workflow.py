"""Producer DAG and ordinary-check isolation contracts; no remote runs."""
from __future__ import annotations

import ast
import copy
import importlib.util
import json
import os
import pathlib
import tempfile
import textwrap
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).parents[2]
SPEC = importlib.util.spec_from_file_location("environment_workflow_quality", ROOT / "scripts/lint_ci_quality.py")
QUALITY = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(QUALITY)
CI_BUILD = ROOT / ".github/workflows/ci-build.yml"
PRODUCER = ROOT / ".github/workflows/ci-environments.yml"
PREFIX = "${{ github.event_name == 'workflow_dispatch' && inputs.environment-mode != 'off' && '[environment-mode skipped] ' || '' }}"
ORDINARY = "(github.event_name != 'workflow_dispatch' || inputs.environment-mode == 'off')"


def mutate_job(text, job, old, new):
    block = "\n".join(QUALITY.workflow_job_blocks(text)[job])
    if old not in block:
        raise AssertionError("mutation did not match job " + job)
    return text.replace(block, block.replace(old, new, 1), 1)


def python_payload(text, job, name):
    for step in QUALITY.workflow_step_blocks(QUALITY.workflow_job_blocks(text)[job]):
        if QUALITY.workflow_step_fields(step)[0].get("name") != name:
            continue
        begin = next(index for index, line in enumerate(step) if line.strip().startswith("python3 - <<'PY'"))
        end = next(index for index in range(begin + 1, len(step)) if step[index].strip() == "PY")
        return textwrap.dedent("\n".join(step[begin + 1:end]))
    raise AssertionError("missing executable Python payload")


def event_expression(expression, event, mode, skipped=False):
    text = expression[3:] if expression.startswith("${{") else expression
    text = text[:-2] if text.endswith("}}") else text
    text = text.strip()
    text = text.replace("github.event_name", repr(event)).replace("inputs.environment-mode", repr(mode))
    text = text.replace("!contains(github.event.head_commit.message, '[skip ci]')", repr(not skipped))
    text = text.replace("always()", "True").replace("&&", "and").replace("||", "or")
    parsed = ast.parse(text, mode="eval")
    allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.Compare, ast.Eq, ast.NotEq, ast.Constant)
    if not all(isinstance(node, allowed) for node in ast.walk(parsed)):
        raise AssertionError("unmodeled event expression")
    return eval(compile(parsed, "<event projection>", "eval"), {"__builtins__": {}})


class EnvironmentBootstrapTest(unittest.TestCase):
    def test_registered_dispatch_exposes_explicit_environment_mode_and_source_pins(self):
        text = CI_BUILD.read_text()
        self.assertIn("      environment-mode:\n", text)
        self.assertIn("      expected-source-sha:\n", text)
        self.assertIn("      analysis-base-sha:\n", text)
        blocks = QUALITY.workflow_job_blocks(text)
        self.assertEqual(QUALITY.workflow_job_direct_value(blocks.get("EnvironmentProducer", []), "uses"),
                         "./.github/workflows/ci-environments.yml")

    def test_skipped_producer_mode_jobs_cannot_emit_ordinary_required_check_names(self):
        blocks = QUALITY.workflow_job_blocks(CI_BUILD.read_text())
        for job in QUALITY.CI_BUILD_REQUIRED_JOBS:
            if job in ("EnvironmentModePreflight", "EnvironmentProducer"):
                continue
            with self.subTest(job=job):
                name = QUALITY.workflow_job_direct_value(blocks[job], "name")
                self.assertTrue(name.startswith(PREFIX), name)

    def test_lint_rejects_skipped_normal_gate_name_not_only_gate_execution_condition(self):
        text = CI_BUILD.read_text()
        if PREFIX in text:
            mutated = text.replace(PREFIX + "ci-gate", "ci-gate", 1)
        else:
            mutated = text.replace("    if: always()\n", "    if: ${{ always() && " + ORDINARY + " }}\n", 1)
        self.assertNotEqual(text, mutated)
        self.assertIn("CI producer mode must distinguish every skipped ordinary check name: ci-gate",
                      QUALITY.ci_build_workflow_issues(mutated))

    def test_normal_event_names_and_gate_behavior_are_unchanged_but_producer_names_are_distinct(self):
        blocks = QUALITY.workflow_job_blocks(CI_BUILD.read_text())
        cases = (("push", "off", True), ("pull_request", "off", True),
                 ("workflow_dispatch", "off", True), ("workflow_dispatch", "qualify", False),
                 ("workflow_dispatch", "publish", False), ("push", "publish", True))
        for event, mode, ordinary in cases:
            with self.subTest(event=event, mode=mode):
                for job in QUALITY.CI_BUILD_REQUIRED_JOBS:
                    fields = blocks[job]
                    name = QUALITY.workflow_job_direct_value(fields, "name")
                    self.assertTrue(name.startswith(PREFIX))
                    actual = event_expression(PREFIX, event, mode) + name[len(PREFIX):]
                    if ordinary:
                        self.assertEqual(actual, name[len(PREFIX):])
                    else:
                        self.assertTrue(actual.startswith("[environment-mode skipped] "))
                        self.assertNotEqual(actual, name[len(PREFIX):])
                    if job == "DispatchContinuous":
                        self.assertIn("github.event_name == 'push'", QUALITY.workflow_job_direct_value(fields, "if"))
                        continue
                    condition = QUALITY.workflow_job_direct_value(fields, "if")
                    self.assertEqual(bool(event_expression(condition, event, mode)), ordinary)
                    self.assertEqual(bool(event_expression(condition, event, mode, skipped=True)), ordinary and job == "ci-gate")

    def test_every_ordinary_name_shadow_mutation_is_rejected(self):
        text = CI_BUILD.read_text()
        for job in QUALITY.CI_BUILD_REQUIRED_JOBS:
            with self.subTest(job=job):
                mutated = mutate_job(text, job, PREFIX, "")
                self.assertIn("CI producer mode must distinguish every skipped ordinary check name: " + job,
                              QUALITY.ci_build_environment_mode_issues(mutated))

    def test_caller_permissions_secrets_and_wrong_ref_cannot_bypass_reusable_boundary(self):
        text = CI_BUILD.read_text()
        mutations = (
            ("uses: ./.github/workflows/ci-environments.yml", "uses: ZEACENT/klogg/.github/workflows/ci-environments.yml@master"),
            ("    permissions:\n", "    secrets: inherit\n    permissions:\n"),
            ("      contents: read", "      contents: write"),
            ("      actions: read", "      actions: write"),
            ("needs: [EnvironmentModePreflight]", "needs: []"),
        )
        for old, new in mutations:
            with self.subTest(old=old):
                mutated = mutate_job(text, "EnvironmentProducer", old, new)
                self.assertIn("CI environment caller must use the exact source-local reusable workflow and narrow permission ceiling",
                              QUALITY.ci_build_environment_mode_issues(mutated))

    def test_dispatch_preflight_rejects_release_mix_missing_or_stale_source_and_bad_base(self):
        payload = python_payload(CI_BUILD.read_text(), "EnvironmentModePreflight", "Validate isolated producer mode and exact source")
        valid = {"KLOGG_ENVIRONMENT_MODE": "qualify", "KLOGG_QUALIFICATION_MODE": "validation",
                 "KLOGG_EXPECTED_SOURCE_SHA": "1" * 40, "KLOGG_ANALYSIS_BASE_SHA": "2" * 40,
                 "GITHUB_SHA": "1" * 40, "GITHUB_REPOSITORY": "ZEACENT/klogg", "GITHUB_REF": "refs/heads/feature"}
        with mock.patch.dict(os.environ, valid):
            exec(compile(payload, "<dispatch preflight>", "exec"), {})
        for key, value in (("KLOGG_QUALIFICATION_MODE", "release"), ("KLOGG_EXPECTED_SOURCE_SHA", ""),
                           ("KLOGG_EXPECTED_SOURCE_SHA", "3" * 40), ("KLOGG_ANALYSIS_BASE_SHA", ""),
                           ("KLOGG_ANALYSIS_BASE_SHA", "1" * 40), ("KLOGG_ENVIRONMENT_MODE", "anything"),
                           ("GITHUB_REPOSITORY", "fork/klogg"), ("GITHUB_REF", "refs/tags/release")):
            with self.subTest(key=key, value=value), mock.patch.dict(os.environ, {**valid, key: value}):
                with self.assertRaises(SystemExit):
                    exec(compile(payload, "<dispatch preflight>", "exec"), {})


class EnvironmentProducerLintTest(unittest.TestCase):
    def test_executable_builder_cannot_receive_publisher_write_scopes(self):
        unsafe = """on:
  workflow_call:
jobs:
  BuildFocal:
    permissions:
      contents: read
      actions: read
      packages: write
    steps:
      - run: scripts/build_ci_environment.sh --family focal-qt5-gcc13
"""
        self.assertIn("Environment executable build/qualification jobs must be read-only: BuildFocal",
                      QUALITY.ci_environment_workflow_issues(unsafe))

    def test_qualifier_artifact_downloads_cannot_substitute_a_latest_name(self):
        unsafe = """on:
  workflow_call:
jobs:
  Source:
    runs-on: ubuntu-24.04
  BuildJammy:
    runs-on: ubuntu-24.04
  CpmSources:
    runs-on: ubuntu-24.04
  QualifyAsan:
    permissions:
      contents: read
      actions: read
    needs: [Source, BuildJammy, CpmSources]
    steps:
      - uses: actions/download-artifact@3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c
        with:
          name: latest-candidate
"""
        self.assertIn("Environment artifact downloads require exact same-run IDs: QualifyAsan",
                      QUALITY.ci_environment_workflow_issues(unsafe))

    def test_actual_workflow_has_exact_six_candidates_ten_profiles_and_is_lint_clean(self):
        text = PRODUCER.read_text()
        self.assertEqual(QUALITY.ci_environment_workflow_issues(text), [])
        catalog = json.loads((ROOT / "ci/environments/recipes.json").read_text())
        self.assertEqual(set(QUALITY.CI_ENVIRONMENT_BUILDERS.values()), set(catalog["families"]))
        expected = {(family, profile) for family, item in catalog["families"].items() for profile in item["profiles"]}
        actual = {(family, profile) for family, profile, _, _ in QUALITY.CI_ENVIRONMENT_PROFILES.values()}
        self.assertEqual(actual, expected)
        self.assertEqual((len(QUALITY.CI_ENVIRONMENT_BUILDERS), len(actual)), (6, 10))

    def test_real_workflow_permission_matrix_and_event_mutations_fail_closed(self):
        text = PRODUCER.read_text()
        mutations = (
            ("BuildJammy", "      actions: read", "      actions: read\n      id-token: write"),
            ("QualifyAsan", "      actions: read", "      actions: read\n      packages: write"),
            ("QualifyCodeql", "      actions: read", "      actions: read\n      security-events: write"),
            ("Publisher", "environment: ci-environment-publish", "environment: unprotected"),
            ("Publisher", "inputs.mode == 'publish'", "inputs.mode != 'off'"),
            ("UploadCodeqlSarif", "    if: ${{ inputs.mode == 'publish' }}", "    if: ${{ always() }}"),
            ("PublicVerification", "      actions: read", "      actions: read\n      packages: write"),
            ("BuildFocal", "    runs-on: ubuntu-24.04", "    strategy:\n      matrix:\n        family: [one, two]\n    runs-on: ubuntu-24.04"),
            ("QualifyTsan", "    timeout-minutes: 180", "    continue-on-error: true\n    timeout-minutes: 180"),
        )
        for job, old, new in mutations:
            with self.subTest(job=job, old=old):
                self.assertTrue(QUALITY.ci_environment_workflow_issues(mutate_job(text, job, old, new)))

    def test_artifact_identity_source_family_and_mobile_ancestry_mutations_are_rejected(self):
        text = PRODUCER.read_text()
        mutations = (
            ("QualifyAsan", "needs.BuildJammy.outputs.candidate-artifact-id", "needs.BuildFocal.outputs.candidate-artifact-id"),
            ("QualifyAsan", "needs: [Source, BuildJammy, CpmSources]", "needs: [Source, BuildJammy, CpmSources, LinuxFixture]"),
            ("QualifyCoverage", "KLOGG_PROFILE: coverage", "KLOGG_PROFILE: static"),
            ("BuildJammy", "KLOGG_FAMILY: jammy-qt5", "KLOGG_FAMILY: noble-qt6"),
            ("QualifyAppImage", "          artifact-ids: ${{ env.KLOGG_CANDIDATE_ARTIFACT_ID }}", "          name: latest-candidate"),
            ("QualifyAppImage", "          artifact-ids: ${{ env.KLOGG_CANDIDATE_ARTIFACT_ID }}", "          artifact-ids: ${{ env.KLOGG_CANDIDATE_ARTIFACT_ID }}\n          run-id: 42"),
            ("BuildFocal", "archive-sha256: ${{ steps.identity.outputs.archive-sha256 }}", "archive-sha256: fabricated"),
            ("QualifyStatic", "path: ${{ runner.temp }}/analysis-result/codeql.sarif", "path: ${{ runner.temp }}/analysis-result/"),
        )
        for job, old, new in mutations:
            with self.subTest(job=job, old=old):
                self.assertTrue(QUALITY.ci_environment_workflow_issues(mutate_job(text, job, old, new)))

    def test_real_profile_command_cannot_be_replaced_by_comment_string_or_optional_step(self):
        text = PRODUCER.read_text()
        command = 'python3 scripts/qualify_ci_analysis.py --role "$KLOGG_PROFILE"'
        for replacement in ('# ' + command, 'printf "%s\\n" "scripts/qualify_ci_analysis.py"',
                            'python3 scripts/not-qualify-ci-analysis.py --role "$KLOGG_PROFILE"'):
            with self.subTest(replacement=replacement):
                mutated = mutate_job(text, "QualifyStatic", command, replacement)
                self.assertTrue(any("execute real profile" in issue for issue in QUALITY.ci_environment_workflow_issues(mutated)))
        mutated = mutate_job(text, "QualifyAppImage", "      - uses: ./.github/actions/linux-validate\n",
                             "      - uses: ./.github/actions/linux-validate\n        if: ${{ false }}\n")
        self.assertTrue(any("execute real profile" in issue for issue in QUALITY.ci_environment_workflow_issues(mutated)))

    def test_qualification_gate_and_publisher_cannot_drop_profiles_or_upgrade_qualify_receipts(self):
        text = PRODUCER.read_text()
        mutations = (
            ("QualificationGate", ", QualifyCoverage, QualifyCodeql, UploadCodeqlSarif]", ", QualifyCodeql, UploadCodeqlSarif]"),
            ("QualificationGate", ' --operation "$KLOGG_ENVIRONMENT_MODE"', ' --operation publish'),
            ("QualificationGate", ' --sarif-result "$KLOGG_SARIF_RESULT"', ' --sarif-result success'),
            ("QualificationGate", "    if: ${{ always() }}", "    if: ${{ success() }}"),
            ("Publisher", "subject-name: verification.json", "subject-name: other.json"),
            ("Publisher", "subject-digest: ${{ steps.publish.outputs.focal_image_digest }}", "subject-digest: ${{ steps.publish.outputs.jammy_image_digest }}"),
            ("Publisher", "          push-to-registry: false", "          push-to-registry: true"),
            ("Publisher", "ci_environment_pipeline.py finalize-publication", "ci_environment_pipeline.py verify-publication"),
            ("PublicVerification", "    needs: [Publisher]", "    needs: [QualificationGate]"),
            ("PublicVerification", "    permissions:\n", "    continue-on-error: true\n    permissions:\n"),
        )
        for job, old, new in mutations:
            with self.subTest(job=job, old=old):
                self.assertTrue(QUALITY.ci_environment_workflow_issues(mutate_job(text, job, old, new)))

    def test_malformed_duplicate_fields_unknown_anchors_and_extra_jobs_are_rejected(self):
        text = PRODUCER.read_text()
        for mutated in (
            text.replace("steps: *candidate_steps", "steps: *missing_steps", 1),
            text.replace("  BuildJammy:\n", "  BuildJammy:\n    permissions: write-all\n", 1),
            text.replace("jobs:\n", "jobs:\n  UnexpectedPublisher:\n    runs-on: ubuntu-24.04\n", 1),
            text.replace("  workflow_call:\n", "  workflow_dispatch:\n", 1),
            text.replace("on:\n", "on:\n  schedule:\n    - cron: '0 1 * * *'\n", 1),
        ):
            with self.subTest(mutation=mutated[:80]):
                self.assertTrue(QUALITY.ci_environment_workflow_issues(mutated))

    def test_publisher_subjects_are_twelve_distinct_exact_bindings_and_public_check_is_later(self):
        text = PRODUCER.read_text()
        steps = QUALITY.workflow_job_steps(text)
        attestations = [QUALITY.workflow_step_fields(step) for step in steps["Publisher"]
                        if QUALITY.workflow_step_fields(step)[0].get("uses", "").startswith("actions/attest-build-provenance@")]
        self.assertEqual(len(attestations), 12)
        self.assertEqual(sum(children["with"]["subject-name"] == "verification.json" for _, children in attestations), 6)
        self.assertEqual(sum(children["with"]["subject-name"] == "ghcr.io/zeacent/klogg-ci-env" for _, children in attestations), 6)
        needs = QUALITY.workflow_job_needs(text)
        self.assertEqual(needs["PublicVerification"], {"Publisher"})
        self.assertNotIn("verify-publication", "\n".join(line for step in steps["Publisher"] for line in step))
        for job, _, _, _, package in ((job, *values) for job, values in QUALITY.CI_ENVIRONMENT_PROFILES.items()):
            if not package:
                self.assertNotIn("LinuxFixture", QUALITY.workflow_job_ancestors(needs, job))


class ExecutableQualificationGateTest(unittest.TestCase):
    def setUp(self):
        self.payload = python_payload(PRODUCER.read_text(), "QualificationGate", "Require every exact builder and qualification result")
        self.needs = {}
        for index, job in enumerate(QUALITY.CI_ENVIRONMENT_BUILDERS, 1):
            self.needs[job] = {"result": "success", "outputs": {"candidate-artifact-id": str(index), "archive-sha256": "a" * 64}}
        for index, job in enumerate(QUALITY.CI_ENVIRONMENT_PROFILES, 20):
            self.needs[job] = {"result": "success", "outputs": {"receipt-artifact-id": str(index), "archive-sha256": "b" * 64, "receipt-sha256": "c" * 64}}
        self.needs["UploadCodeqlSarif"] = {"result": "success", "outputs": {}}

    def execute(self, mode, needs):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            output = root / "github-output"
            environment = {"RUNNER_TEMP": str(root), "GITHUB_OUTPUT": str(output),
                           "KLOGG_NEEDS_JSON": json.dumps(needs), "KLOGG_ENVIRONMENT_MODE": mode}
            with mock.patch.dict(os.environ, environment):
                exec(compile(self.payload, "<qualification gate>", "exec"), {})
            return json.loads((root / "artifact-identities.json").read_text()), json.loads((root / "job-results.json").read_text()), output.read_text()

    def test_exact_publish_and_qualify_results_preserve_external_artifact_ids(self):
        for mode, sarif in (("publish", "success"), ("qualify", "skipped")):
            with self.subTest(mode=mode):
                needs = copy.deepcopy(self.needs)
                needs["UploadCodeqlSarif"]["result"] = sarif
                identities, results, output = self.execute(mode, needs)
                self.assertEqual(len(identities["candidates"]), 6)
                self.assertEqual(sum(len(values) for values in identities["profiles"].values()), 10)
                self.assertEqual(len(results), 16)
                self.assertEqual(identities["candidates"]["focal-qt5-gcc13"]["artifact_id"], 1)
                self.assertEqual(output, "sarif-result=" + sarif + "\n")

    def test_each_missing_failed_cancelled_or_skipped_required_lane_rejects_qualification(self):
        for job in set(self.needs) - {"UploadCodeqlSarif"}:
            for result in ("failure", "cancelled", "skipped", None):
                with self.subTest(job=job, result=result):
                    needs = copy.deepcopy(self.needs)
                    if result is None:
                        del needs[job]
                    else:
                        needs[job]["result"] = result
                    with self.assertRaises(SystemExit):
                        self.execute("publish", needs)

    def test_sarif_status_cannot_upgrade_qualify_to_publication(self):
        for mode, sarif in (("publish", "skipped"), ("publish", "failure"), ("qualify", "success"), ("unexpected", "success")):
            with self.subTest(mode=mode, sarif=sarif):
                needs = copy.deepcopy(self.needs)
                needs["UploadCodeqlSarif"]["result"] = sarif
                with self.assertRaises(SystemExit):
                    self.execute(mode, needs)


if __name__ == "__main__":
    unittest.main()
