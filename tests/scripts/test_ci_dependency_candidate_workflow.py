"""Fail-closed contract for native-core candidate production in CI Build."""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import re
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).parents[2]
SPEC = importlib.util.spec_from_file_location("lint_ci_quality", ROOT / "scripts/lint_ci_quality.py")
LINT = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(LINT)
WORKFLOW = ROOT / ".github/workflows/ci-build.yml"
UPLOAD = "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a"
CONDITION = "${{ github.event_name == 'workflow_dispatch' && (inputs.dependency-mode == 'qualify' || inputs.dependency-mode == 'publish') }}"
NATIVE_BUILDER_IF = "${{ ((github.event_name != 'workflow_dispatch' || (inputs.environment-mode == 'off' && inputs.dependency-mode == 'off')) || (github.event_name == 'workflow_dispatch' && (inputs.dependency-mode == 'qualify' || inputs.dependency-mode == 'publish'))) && !contains(github.event.head_commit.message, '[skip ci]') }}"
TARGETS = {
    "BuildAdbLinuxX64": ("adb-linux-x86_64", "ubuntu-24.04", "adb"),
    "BuildAdbLinuxArm64": ("adb-linux-arm64", "ubuntu-24.04-arm", "adb"),
    "BuildAdbWindowsX64": ("adb-windows-x86_64", "windows-2022", "adb"),
    "BuildAdbMacX64": ("adb-macos-x86_64", "macos-15-intel", "adb"),
    "BuildAdbMacArm64": ("adb-macos-arm64", "macos-15", "adb"),
    "BuildIosNativeX64": ("ios-x86_64", "macos-15-intel", "ios"),
    "BuildIosNativeArm64": ("ios-arm64", "macos-15", "ios"),
}


def candidate_issues(text: str) -> list[str]:
    """Inspect resolved anchors, direct job outputs, and exact candidate boundaries."""
    issues = []
    blocks = LINT.workflow_job_blocks(text)
    try:
        steps = LINT.workflow_job_steps(text)
    except ValueError as error:
        return [str(error)]
    catalog = json.loads((ROOT / "ci/dependencies/catalog.json").read_text())["targets"]
    if set(catalog) != {target for target, _, _ in TARGETS.values()}:
        issues.append("catalog target set differs from seven explicit builders")
    for job, (target_id, runner, family) in TARGETS.items():
        block = blocks.get(job, [])
        job_steps = [LINT.workflow_step_fields(step) for step in steps.get(job, [])]
        failure = lambda reason: issues.append(f"{job}: {reason}")
        if LINT.workflow_job_direct_value(block, "runs-on") != runner:
            failure("runner mismatch")
        if LINT.workflow_job_direct_value(block, "if") != NATIVE_BUILDER_IF:
            failure("ordinary and isolated native dispatch projections changed")
        expected_outputs = {
            "artifact_id": "${{ steps.upload_candidate.outputs.artifact-id }}",
            "candidate_sha256": "${{ steps.candidate.outputs.candidate-sha256 }}",
            "archive_sha256": "${{ steps.candidate.outputs.archive-sha256 }}",
            "full_artifact_id": ("${{ steps.upload_full.outputs.artifact-id }}" if family == "adb"
                                 else "${{ steps.resolve_full_artifact.outputs.artifact-id }}"),
        }
        mapping = LINT.workflow_mapping_block(block, "outputs", 4)
        if mapping is None or {key: value for key, (value, _) in mapping.items()} != expected_outputs:
            failure("missing or substituted immutable job outputs")
        generated = [(index, fields) for index, (fields, _) in enumerate(job_steps) if fields.get("id") == "candidate"]
        uploaded = [(index, fields, children.get("with", {})) for index, (fields, children) in enumerate(job_steps)
                    if fields.get("id") == "upload_candidate"]
        if len(generated) != 1 or len(uploaded) != 1:
            failure("exactly one generator and upload required")
            continue
        index, generate = generated[0]
        upload_index, upload, options = uploaded[0]
        prefix = "adb-helper" if family == "adb" else "ios-native"
        target_expression = "adb-${KLOGG_ADB_HELPER_TARGET}" if family == "adb" else "ios-${KLOGG_IOS_ARCHITECTURE}"
        archive_expression = f"{prefix}-${{KLOGG_ADB_HELPER_TARGET}}.tar.gz" if family == "adb" else f"{prefix}-${{KLOGG_IOS_ARCHITECTURE}}.tar.gz"
        input_root = "$RUNNER_TEMP/adb-helper-artifact" if family == "adb" else "$RUNNER_TEMP/ios-native-stack"
        source_root = "$RUNNER_TEMP/adb-helper-package-support" if family == "adb" else "$RUNNER_TEMP/ios-native-stack"
        script = generate.get("run", "")
        commands = LINT.environment_shell_commands(steps[job][index])
        expected_command = [
            "python3", "scripts/ci_dependency_producer.py",
            "--target-id", target_expression, "--artifact-root", input_root,
            "--source-assets-root", source_root,
            "--archive", f"$candidate_dir/{archive_expression}",
            "--candidate-receipt", "$candidate_dir/candidate.json",
        ]
        required = (
            "set -euo pipefail", "python3 scripts/ci_dependency_producer.py",
            f'--target-id "{target_expression}"', f'--artifact-root "{input_root}"',
            f'--source-assets-root "{source_root}"',
            f'--archive "$candidate_dir/{archive_expression}"',
            '--candidate-receipt "$candidate_dir/candidate.json"',
            'mkdir -p "$candidate_dir"', 'hashlib.sha256(',
            'print(f"candidate-sha256={hashlib.sha256(receipt.read_bytes()).hexdigest()}")',
            'print(f"archive-sha256={hashlib.sha256(archive.read_bytes()).hexdigest()}")',
            'GITHUB_OUTPUT',
        )
        if (generate.get("name") != "Produce offline native-core candidate"
                or generate.get("if") != CONDITION or generate.get("shell") != "bash"
                or generate.get("continue-on-error") not in (None, "false")
                or any(marker not in script for marker in required)
                or commands.count(expected_command) != 1
                or re.search(r"(?m)^\s*#.*ci_dependency_producer.py", script)):
            failure("candidate generator must run offline with exact target and legal roots")
        expected_archive = catalog[target_id]["archive_name"]
        if f"{prefix}-{target_id.split('-', 1)[1]}.tar.gz" != expected_archive:
            failure("catalog archive mismatch")
        env_mapping = LINT.workflow_mapping_block(block, "env", 4)
        env = {} if env_mapping is None else {key: value for key, (value, _) in env_mapping.items()}
        target_var = "KLOGG_ADB_HELPER_TARGET" if family == "adb" else "KLOGG_IOS_ARCHITECTURE"
        if env.get(target_var) != target_id.split("-", 1)[1]:
            failure("job target differs from catalog")
        github_target = f"${{{{ env.{target_var} }}}}"
        if (upload_index <= index or upload.get("if") != CONDITION or upload.get("uses") != UPLOAD
                or upload.get("continue-on-error") not in (None, "false")
                or options != {
                    "name": f"native-core-candidate-{family}-{github_target}-${{{{ github.run_id }}}}-${{{{ github.run_attempt }}}}",
                    "path": f"${{{{ runner.temp }}}}/native-core-candidate/{prefix}-{github_target}.tar.gz\n${{{{ runner.temp }}}}/native-core-candidate/candidate.json",
                    "if-no-files-found": "error", "compression-level": "0",
                }):
            failure("candidate upload must contain only two exact same-attempt files")
        required_prior = ("./.github/actions/build-adb-helper",) if family == "adb" else ()
        prior_steps = [fields for fields, _ in job_steps[:index]]
        if family == "adb":
            if not any(fields.get("uses") in required_prior for fields in prior_steps):
                failure("ADB full smoke/legal verification must precede candidate")
            if not any(fields.get("name") == "Package ADB helper without losing file modes" for fields in prior_steps):
                failure("legacy ADB full archive must precede candidate")
        elif not any(fields.get("name") == "Bind iOS native artifact with SHA256SUMS" for fields in prior_steps):
            failure("iOS full build/legal verification must precede candidate")
        if not any(fields.get("uses") == UPLOAD and fields.get("if") is None and
                   fields.get("id") != "upload_candidate" for fields in prior_steps):
            failure("existing full artifact upload must precede candidate")
    return issues


ATTEST = "actions/attest-build-provenance@4d101475d8b20a2381f78447822ac1eab6504dd8"
NON_PR = "${{ github.event_name != 'pull_request' }}"


def full_artifact_issues(text: str) -> list[str]:
    """Original transport identity and signed tar bytes must precede candidates."""
    blocks = LINT.workflow_job_blocks(text)
    try:
        jobs = LINT.workflow_job_steps(text)
    except ValueError as error:
        return [str(error)]
    issues = []
    for job, (target_id, _, family) in TARGETS.items():
        block = blocks.get(job, [])
        steps = [LINT.workflow_step_fields(step) for step in jobs.get(job, [])]
        fields = [entry for entry, _ in steps]
        expected_name = "adb-helper-${{ env.KLOGG_ADB_HELPER_TARGET }}" if family == "adb" else "${{ env.KLOGG_IOS_ARTIFACT }}"
        expected_archive = ("${{ runner.temp }}/adb-helper-${{ env.KLOGG_ADB_HELPER_TARGET }}.tar.gz"
                            if family == "adb" else "${{ runner.temp }}/${{ env.KLOGG_IOS_ARTIFACT }}.tar.gz")
        expected_full_id = ("${{ steps.upload_full.outputs.artifact-id }}" if family == "adb"
                            else "${{ steps.resolve_full_artifact.outputs.artifact-id }}")
        mapping = LINT.workflow_mapping_block(block, "outputs", 4)
        outputs = {} if mapping is None else {key: value for key, (value, _) in mapping.items()}
        if outputs.get("full_artifact_id") != expected_full_id or set(outputs) != {
                "artifact_id", "candidate_sha256", "archive_sha256", "full_artifact_id"}:
            issues.append(job + ": original full artifact ID must be independent of candidate")
        def match(name):
            return [(index, field, children) for index, (field, children) in enumerate(steps)
                    if field.get("name") == name]
        uploads = [(index, field, child) for index, (field, child) in enumerate(steps)
                   if field.get("id") == ("upload_full" if family == "adb" else "upload_ios_stack")]
        candidates = [(index, field, child) for index, (field, child) in enumerate(steps)
                      if field.get("id") == "candidate"]
        archive_steps = match("Package ADB helper without losing file modes" if family == "adb"
                              else "Bind iOS native artifact with SHA256SUMS")
        original_attests = match("Attest target-bound ADB helper build provenance" if family == "adb"
                                 else "Attest target-bound iOS native stack provenance")
        signed_tars = match("Attest exact full native tar provenance")
        if not all(len(group) == 1 for group in (uploads, candidates, archive_steps, original_attests, signed_tars)):
            issues.append(job + ": missing unique full upload, SHA signing, tar signing, or candidate")
            continue
        upload_index, upload, upload_child = uploads[0]
        attest_index, attest, attest_child = signed_tars[0]
        sha_index, sha_attest, sha_child = original_attests[0]
        if not (archive_steps[0][0] < upload_index < sha_index < attest_index < candidates[0][0]):
            issues.append(job + ": tar signing must follow packaging/full upload/SHA signing and precede candidate")
        if (upload.get("uses") != UPLOAD or upload_child.get("with") != {
                "name": expected_name, "path": expected_archive, "if-no-files-found": "error",
        } or upload.get("if") is not None or
                upload.get("continue-on-error") != (None if family == "adb" else "true")):
            issues.append(job + ": full artifact upload path or failure semantics changed")
        expected_sha = ("${{ runner.temp }}/adb-helper-artifact/SHA256SUMS" if family == "adb"
                        else "${{ runner.temp }}/ios-native-stack/SHA256SUMS")
        if (sha_attest.get("uses") != ATTEST or sha_attest.get("if") != NON_PR
                or sha_child.get("with") != {"subject-path": expected_sha}):
            issues.append(job + ": prior SHA256SUMS attestation must be preserved")
        if (attest.get("uses") != ATTEST or attest.get("if") != NON_PR
                or attest.get("continue-on-error") not in (None, "false")
                or attest_child.get("with") != {"subject-path": expected_archive}):
            issues.append(job + ": exact full tar bytes must be signed on non-PR events")
        if family == "ios":
            retries = [(index, field, child) for index, (field, child) in enumerate(steps)
                       if field.get("id") == "retry_ios_stack"]
            resolves = [(index, field, child) for index, (field, child) in enumerate(steps)
                        if field.get("id") == "resolve_full_artifact"]
            if len(retries) != 1 or len(resolves) != 1:
                issues.append(job + ": iOS retry and effective ID resolution must be explicit")
                continue
            retry_index, retry, retry_child = retries[0]
            resolve_index, resolve, resolve_child = resolves[0]
            if not upload_index < retry_index < resolve_index < attest_index:
                issues.append(job + ": iOS effective ID must be resolved after original/retry uploads")
            if (retry.get("uses") != UPLOAD or retry.get("if") != "${{ steps.upload_ios_stack.outcome == 'failure' }}"
                    or retry_child.get("with") != {
                        "name": expected_name, "path": expected_archive,
                        "if-no-files-found": "error", "overwrite": "true",
                    }):
                issues.append(job + ": original iOS retry must remain fail-closed")
            required_env = {
                "KLOGG_PRIMARY_OUTCOME": "${{ steps.upload_ios_stack.outcome }}",
                "KLOGG_PRIMARY_ID": "${{ steps.upload_ios_stack.outputs.artifact-id }}",
                "KLOGG_RETRY_OUTCOME": "${{ steps.retry_ios_stack.outcome }}",
                "KLOGG_RETRY_ID": "${{ steps.retry_ios_stack.outputs.artifact-id }}",
            }
            if (resolve.get("if") != "${{ always() && (steps.upload_ios_stack.outcome == 'success' || steps.upload_ios_stack.outcome == 'failure') }}"
                    or resolve.get("shell") != "bash" or resolve.get("continue-on-error") not in (None, "false")
                    or resolve_child.get("env") != required_env):
                issues.append(job + ": effective iOS ID must bind both actual upload outcomes")
    return issues


class CandidateWorkflowTest(unittest.TestCase):
    def test_original_full_archives_have_independent_ids_and_non_pr_byte_attestations(self):
        self.assertEqual(full_artifact_issues(WORKFLOW.read_text()), [])

    def test_full_archive_attestation_and_id_mutations_fail_closed(self):
        workflow = WORKFLOW.read_text()
        mutations = {
            "forged ADB ID": ("steps.upload_full.outputs.artifact-id", "steps.upload_candidate.outputs.artifact-id"),
            "wrong signed tar": ("subject-path: ${{ runner.temp }}/adb-helper-${{ env.KLOGG_ADB_HELPER_TARGET }}.tar.gz",
                                 "subject-path: ${{ runner.temp }}/adb-helper-artifact/SHA256SUMS"),
            "PR signing leak": ("      - name: Attest exact full native tar provenance\n        if: " + NON_PR,
                                "      - name: Attest exact full native tar provenance\n        if: ${{ github.event_name != 'push' }}"),
            "forgotten retry ID": ("        id: retry_ios_stack\n", "        # id: retry_ios_stack\n"),
            "untrusted outcome": ("${{ steps.retry_ios_stack.outcome }}", "${{ steps.upload_candidate.outcome }}"),
            "untrusted retry ID": ("${{ steps.retry_ios_stack.outputs.artifact-id }}", "${{ steps.upload_candidate.outputs.artifact-id }}"),
            "no failed branch": ("KLOGG_RETRY_OUTCOME: ${{ steps.retry_ios_stack.outcome }}", "KLOGG_RETRY_OUTCOME: success"),
            "unsigned iOS bytes": ("subject-path: ${{ runner.temp }}/${{ env.KLOGG_IOS_ARTIFACT }}.tar.gz",
                                   "subject-path: ${{ runner.temp }}/ios-native-stack/SHA256SUMS"),
            "changed original iOS upload": ("        id: upload_ios_stack\n        continue-on-error: true",
                                            "        id: upload_ios_stack\n        continue-on-error: false"),
        }
        for name, (old, new) in mutations.items():
            with self.subTest(name=name):
                self.assertIn(old, workflow)
                self.assertTrue(full_artifact_issues(workflow.replace(old, new, 1)), name)

    def test_ios_effective_id_requires_successful_actual_upload_branch(self):
        text = WORKFLOW.read_text()
        steps = LINT.workflow_job_steps(text)["BuildIosNativeX64"]
        resolver = [LINT.workflow_step_fields(step)[0] for step in steps
                    if LINT.workflow_step_fields(step)[0].get("id") == "resolve_full_artifact"]
        self.assertEqual(len(resolver), 1)
        script = resolver[0].get("run", "")
        for primary, primary_id, retry, retry_id, chosen in (
                ("success", "123", "skipped", "", "123"),
                ("failure", "", "success", "456", "456"),
                ("failure", "", "failure", "", None),
                ("failure", "", "skipped", "", None),
                ("success", "", "skipped", "", None),
                ("failure", "", "success", "invalid", None),
                ("success", "000", "skipped", "", None),
                ("success", "123", "success", "456", None),
                ("failure", "123", "success", "456", None),
                ("success", "123", "skipped", "456", None)):
            with self.subTest(primary=primary, primary_id=primary_id, retry=retry, retry_id=retry_id):
                with tempfile.TemporaryDirectory() as directory:
                    output = pathlib.Path(directory) / "github-output"
                    env = {**os.environ, "GITHUB_OUTPUT": str(output),
                           "KLOGG_PRIMARY_OUTCOME": primary, "KLOGG_PRIMARY_ID": primary_id,
                           "KLOGG_RETRY_OUTCOME": retry, "KLOGG_RETRY_ID": retry_id}
                    result = subprocess.run(["bash", "-c", script], env=env,
                                            capture_output=True, text=True, timeout=10)
                    if chosen is None:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertFalse(output.exists(), result.stdout + result.stderr)
                    else:
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertEqual(output.read_text(), "artifact-id=" + chosen + "\n")

    def test_real_seven_builders_expose_only_immutable_binary_candidates(self):
        self.assertEqual(candidate_issues(WORKFLOW.read_text()), [])

    def test_real_workflow_mutations_fail_closed(self):
        original = WORKFLOW.read_text()
        mutations = {
            "missing generator": ("      - name: Produce offline native-core candidate", "      - name: Not a candidate"),
            "PR leak": ("        if: " + CONDITION, "        if: ${{ github.event_name != 'push' }}"),
            "wrong legal root": ('--source-assets-root "$RUNNER_TEMP/adb-helper-package-support"', '--source-assets-root "$RUNNER_TEMP/adb-helper-artifact"'),
            "full closure upload": ("${{ runner.temp }}/native-core-candidate/candidate.json", "${{ runner.temp }}/adb-helper-artifact"),
            "wrong target": ('--target-id "adb-${KLOGG_ADB_HELPER_TARGET}"', '--target-id "adb-linux-x86_64"'),
            "unreviewed upload": ("        id: upload_candidate\n        if: " + CONDITION + "\n        uses: " + UPLOAD,
                                  "        id: upload_candidate\n        if: " + CONDITION + "\n        uses: actions/upload-artifact@" + "f" * 40),
            "wrong runner": ("    runs-on: ubuntu-24.04-arm\n", "    runs-on: ubuntu-24.04\n"),
            "missing hash": ("candidate-sha256=", "unbound-candidate-sha256="),
            "wrong output": ("steps.upload_candidate.outputs.artifact-id", "steps.upload_ios_stack.outputs.artifact-id"),
            "no-op comment": ("python3 scripts/ci_dependency_producer.py", "# python3 scripts/ci_dependency_producer.py"),
            "string spoof": ("          python3 scripts/ci_dependency_producer.py \\\n", "          printf '%s\\n' 'python3 scripts/ci_dependency_producer.py' \\\n"),
            "malformed candidate step": ("        id: candidate\n", "        id: candidate\n        id: fake_candidate\n"),
            "extra uploaded closure": ("${{ runner.temp }}/native-core-candidate/candidate.json\n", "${{ runner.temp }}/native-core-candidate/candidate.json\n            ${{ runner.temp }}/ios-native-stack\n"),
        }
        for label, (before, after) in mutations.items():
            with self.subTest(label=label):
                self.assertIn(before, original)
                self.assertTrue(candidate_issues(original.replace(before, after, 1)), label)

    def test_only_explicit_dispatch_candidate_modes_match(self):
        self.assertTrue(CONDITION.startswith("${{ ") and CONDITION.endswith(" }}"))
        expression = CONDITION[4:-3]
        for event, mode in (("push", "off"), ("pull_request", "off"),
                            ("workflow_dispatch", "off"), ("workflow_dispatch", "qualify"),
                            ("workflow_dispatch", "publish")):
            with self.subTest(event=event, mode=mode):
                projected = expression.replace("github.event_name", repr(event)).replace("inputs.dependency-mode", repr(mode))
                projected = projected.replace("&&", " and ").replace("||", " or ")
                self.assertEqual(eval(projected, {"__builtins__": {}}, {}),
                                 event == "workflow_dispatch" and mode in {"qualify", "publish"})


class DependencyQualificationWorkflowTest(unittest.TestCase):
    CHILD = ROOT / ".github/workflows/ci-dependencies.yml"
    ROOT_JOBS = ("SaveVersion", "PrefetchAdbHelperSources", "PrefetchIosNativeSources")
    NATIVE_JOBS = (*ROOT_JOBS, "BuildAdbHelperLegalAssets", *TARGETS)

    def test_both_dispatch_modes_run_native_builds_only_after_preflight(self):
        workflow = WORKFLOW.read_text()
        blocks = LINT.workflow_job_blocks(workflow)
        needs = LINT.workflow_job_needs(workflow)
        preflight = LINT.workflow_job_steps(workflow)["DependencyModePreflight"]
        validation = [LINT.workflow_step_fields(step)[0].get("run", "") for step in preflight
                      if LINT.workflow_step_fields(step)[0].get("name") == "Validate isolated dependency mode and exact source"]
        self.assertEqual(len(validation), 1)
        self.assertNotIn("dependency producer child is not wired", validation[0])
        self.assertNotIn("exit 1", validation[0])
        for job in self.ROOT_JOBS:
            with self.subTest(job=job):
                self.assertIn("DependencyModePreflight", needs[job])
                condition = LINT.workflow_job_direct_value(blocks[job], "if") or ""
                self.assertIn("!cancelled()", condition)
                self.assertIn("needs.DependencyModePreflight.result == 'success'", condition)
        for job in self.NATIVE_JOBS:
            with self.subTest(job=job):
                block = blocks[job]
                self.assertIn("inputs.dependency-mode == 'qualify'", LINT.workflow_job_direct_value(block, "if") or "")
                self.assertIn("inputs.dependency-mode == 'publish'", LINT.workflow_job_direct_value(block, "if") or "")
                name = LINT.workflow_job_direct_value(block, "name") or ""
                self.assertIn("inputs.environment-mode != 'off'", name)
                self.assertNotIn("inputs.dependency-mode != 'off'", name)
        self.assertEqual(needs["BuildAdbHelperLegalAssets"], {"SaveVersion", "PrefetchAdbHelperSources"})
        for job in TARGETS:
            self.assertTrue(needs[job] & {"BuildAdbHelperLegalAssets", "SaveVersion"}, job)

    def test_native_job_event_projections_preserve_ordinary_ci_and_isolate_producers(self):
        blocks = LINT.workflow_job_blocks(WORKFLOW.read_text())
        cases = (
            ("pull_request", "off", "off", "skipped", True),
            ("push", "off", "off", "skipped", True),
            ("workflow_dispatch", "off", "off", "skipped", True),
            ("workflow_dispatch", "off", "qualify", "success", True),
            ("workflow_dispatch", "off", "publish", "success", True),
            ("workflow_dispatch", "off", "qualify", "failure", False),
            ("workflow_dispatch", "qualify", "off", "skipped", False),
            ("workflow_dispatch", "qualify", "qualify", "failure", False),
        )
        for event, environment, dependency, preflight, should_run in cases:
            for job in self.NATIVE_JOBS:
                with self.subTest(event=event, environment=environment, dependency=dependency,
                                  preflight=preflight, job=job):
                    condition = LINT.workflow_job_direct_value(blocks[job], "if")[4:-3]
                    expression = (condition.replace("github.event_name", repr(event))
                                  .replace("inputs.environment-mode", repr(environment))
                                  .replace("inputs.dependency-mode", repr(dependency))
                                  .replace("needs.DependencyModePreflight.result", repr(preflight))
                                  .replace("github.event.head_commit.message", repr("normal commit"))
                                  .replace("!cancelled()", "not cancelled")
                                  .replace("!contains", "not contains")
                                  .replace("&&", " and ").replace("||", " or "))
                    projected = eval(expression, {"__builtins__": {}},
                                     {"cancelled": False, "contains": lambda text, substring: substring in text})
                    effective = projected and (job in self.ROOT_JOBS or preflight != "failure")
                    self.assertEqual(effective, should_run)
                    name = LINT.workflow_job_direct_value(blocks[job], "name") or ""
                    prefix = (name.split("}}", 1)[0][4:]
                              .replace("github.event_name", repr(event))
                              .replace("inputs.environment-mode", repr(environment))
                              .replace("&&", " and ").replace("||", " or "))
                    self.assertEqual(eval(prefix, {"__builtins__": {}}, {}),
                                     "[producer-mode skipped] " if environment != "off" else "")
        condition = LINT.workflow_job_direct_value(blocks[self.ROOT_JOBS[0]], "if")
        self.assertIn("!cancelled()", condition)
        self.assertIn("!contains(github.event.head_commit.message, '[skip ci]')", condition)

    def test_ios_host_probe_runs_only_for_dependency_dispatch_before_native_build(self):
        steps = LINT.workflow_job_steps(WORKFLOW.read_text())
        for job in ("BuildIosNativeX64", "BuildIosNativeArm64"):
            with self.subTest(job=job):
                records = [LINT.workflow_step_fields(step)[0] for step in steps[job]]
                preflight = next(index for index, row in enumerate(records)
                                 if row.get("name") == "Verify pinned iOS producer toolchain")
                probes = [(index, row) for index, row in enumerate(records)
                          if row.get("name") == "Observe unreviewed iOS host tool inputs"]
                self.assertEqual(len(probes), 1)
                index, probe = probes[0]
                source_download = next(i for i, row in enumerate(records)
                                       if row.get("uses", "").startswith("actions/download-artifact@"))
                self.assertLess(preflight, index)
                self.assertLess(index, source_download)
                self.assertEqual(probe.get("if"), CONDITION)
                self.assertEqual(probe.get("shell"), "bash")
                self.assertNotIn("continue-on-error", probe)
                self.assertIn("--probe-unreviewed-host-tools", probe.get("run", ""))
                self.assertIn("ci_dependency_toolchain.py", probe.get("run", ""))

    def test_parent_passes_exact_nine_needs_to_read_only_child(self):
        blocks = LINT.workflow_job_blocks(WORKFLOW.read_text())
        self.assertIn("DependencyGate", blocks)
        gate = blocks["DependencyGate"]
        expected = {"DependencyModePreflight", "BuildAdbHelperLegalAssets", *TARGETS}
        self.assertEqual(LINT.workflow_job_needs(WORKFLOW.read_text())["DependencyGate"], expected)
        self.assertEqual(LINT.workflow_job_direct_value(gate, "uses"), "./.github/workflows/ci-dependencies.yml")
        self.assertEqual(LINT.workflow_job_direct_value(gate, "if"), CONDITION)
        self.assertEqual({key: value for key, (value, _) in LINT.workflow_mapping_block(gate, "permissions", 4).items()},
                         {"contents": "read", "actions": "read", "attestations": "read"})
        self.assertEqual({key: value for key, (value, _) in LINT.workflow_mapping_block(gate, "with", 4).items()},
                         {"mode": "${{ inputs.dependency-mode }}", "expected-source-sha": "${{ inputs.expected-source-sha }}",
                          "needs-json": "${{ toJSON(needs) }}"})

    def test_child_filters_only_successful_eight_native_needs_before_gate(self):
        workflow = self.CHILD.read_text()
        blocks = LINT.workflow_job_blocks(workflow)
        self.assertEqual(set(blocks), {"Gate", "Publish"})
        gate = blocks["Gate"]
        self.assertEqual({key: value for key, (value, _) in LINT.workflow_mapping_block(gate, "permissions", 4).items()},
                         {"contents": "read", "actions": "read", "attestations": "read"})
        steps = [LINT.workflow_step_fields(step)[0] for step in LINT.workflow_job_steps(workflow)["Gate"]]
        shell_script = next(step["run"] for step in steps if step.get("name") == "Validate and filter parent needs")
        raw_step = workflow.split("      - name: Validate and filter parent needs\n", 1)[1].split("\n      - name:", 1)[0]
        raw_shell = textwrap.dedent(raw_step.split("        run: |\n", 1)[1])
        script = raw_shell.partition("python3 - <<'PY'\n")[2].rpartition("\nPY")[0]
        self.assertTrue(script.startswith("import json"), shell_script)
        self.assertIn("BuildAdbHelperLegalAssets", script)
        for job in TARGETS:
            self.assertIn(job, script)
        runs = "\n".join(step.get("run", "") for step in steps)
        self.assertIn("ci_dependency_gate.py", runs)
        for option in ("--mode", "--needs-json", "--output-dir", "--repo-root"):
            self.assertIn(option, runs)
        self.assertIn("write_gate_archive", runs)
        self.assertLess(next(i for i, step in enumerate(steps) if step.get("name") == "Validate and filter parent needs"),
                        next(i for i, step in enumerate(steps) if step.get("name") == "Verify independent full builds and qualify exact candidate bytes"))
        archive = next(step for step in steps if step.get("id") == "archive")
        self.assertIn("identity['sha256']", archive.get("run", ""))
        self.assertIn("identity['size']", archive.get("run", ""))
        uploaded = [(i, LINT.workflow_step_fields(step)) for i, step in enumerate(LINT.workflow_job_steps(workflow)["Gate"])
                    if LINT.workflow_step_fields(step)[0].get("id") == "upload_gate"]
        self.assertEqual(len(uploaded), 1)
        upload_index, (upload, children) = uploaded[0]
        self.assertEqual(upload.get("uses"), UPLOAD)
        self.assertEqual(children.get("with"), {
            "name": "native-gate-${{ github.run_id }}-${{ github.run_attempt }}",
            "path": "${{ runner.temp }}/native-gate.tar.gz", "if-no-files-found": "error",
            "compression-level": "0", "retention-days": "7",
        })
        self.assertGreater(upload_index, next(i for i, step in enumerate(steps) if step.get("id") == "archive"))
        self.assertIn("exit 1", "\n".join(LINT.workflow_step_fields(step)[0].get("run", "")
                                            for step in LINT.workflow_job_steps(workflow)["Publish"]))
        self.assertEqual({key: value for key, (value, _) in LINT.workflow_mapping_block(blocks["Publish"], "permissions", 4).items()},
                         {"contents": "read"})

        with tempfile.TemporaryDirectory() as directory:
            filtered = pathlib.Path(directory) / "native-gate-needs.json"
            valid = {job: {"result": "success", "outputs": {}} for job in ("DependencyModePreflight", "BuildAdbHelperLegalAssets", *TARGETS)}
            def check(payload, allowed):
                filtered.unlink(missing_ok=True)
                process = subprocess.run(["python3", "-c", script],
                                         env={**os.environ, "RUNNER_TEMP": directory,
                                              "KLOGG_PARENT_NEEDS_JSON": json.dumps(payload)},
                                         capture_output=True, text=True, timeout=10)
                self.assertEqual(process.returncode == 0, allowed, process.stderr)
                if allowed:
                    self.assertEqual(json.loads(filtered.read_text()),
                                     {job: valid[job] for job in ("BuildAdbHelperLegalAssets", *TARGETS)})
                else:
                    self.assertFalse(filtered.exists())
            check(valid, True)
            for malformed in ({**valid, "spoof": {"result": "success"}},
                              {key: value for key, value in valid.items() if key != "BuildAdbMacArm64"},
                              {**valid, "DependencyModePreflight": {"result": "failure"}},
                              {**valid, "BuildIosNativeX64": {"result": "skipped"}},
                              [valid]):
                with self.subTest(malformed=str(malformed)[:80]):
                    check(malformed, False)


if __name__ == "__main__":
    unittest.main()
