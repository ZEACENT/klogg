"""Diagnostic iOS host observations must never become native qualification evidence."""

from __future__ import annotations

import importlib.util
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/ci-ios-host-evidence.yml"
PARENT = ROOT / ".github/workflows/ci-build.yml"
COLLECTOR = ROOT / "scripts/ci_ios_host_evidence.py"
LINT_SPEC = importlib.util.spec_from_file_location("lint_ci_quality", ROOT / "scripts/lint_ci_quality.py")
LINT = importlib.util.module_from_spec(LINT_SPEC)
assert LINT_SPEC.loader is not None
LINT_SPEC.loader.exec_module(LINT)


diagnostic_issues = LINT.ci_ios_host_evidence_workflow_issues


class IosHostEvidenceWorkflowTest(unittest.TestCase):
    def test_real_workflow_is_reusable_read_only_and_isolated_from_gate(self):
        self.assertTrue(WORKFLOW.is_file(), "missing isolated diagnostic workflow")
        self.assertEqual(diagnostic_issues(WORKFLOW.read_text()), [])

    def test_registered_parent_dispatches_only_the_isolated_reusable_diagnostic(self):
        text = PARENT.read_text()
        inputs = LINT.workflow_mapping_block(text.splitlines(), "inputs", 4)
        self.assertIn("dependency-mode", inputs)
        modes = inputs["dependency-mode"][1]
        self.assertIn("          - observe-ios-host", modes)
        blocks = LINT.workflow_job_blocks(text)
        self.assertIn("IosHostEvidence", blocks)
        caller = blocks["IosHostEvidence"]
        self.assertEqual(LINT.workflow_job_needs(text)["IosHostEvidence"],
                         {"DependencyModePreflight"})
        self.assertEqual(LINT.workflow_job_direct_value(caller, "uses"),
                         "./.github/workflows/ci-ios-host-evidence.yml")
        self.assertEqual(LINT.workflow_job_direct_value(caller, "if"),
                         "${{ github.event_name == 'workflow_dispatch' && inputs.dependency-mode == 'observe-ios-host' && needs.DependencyModePreflight.result == 'success' }}")
        self.assertEqual({key: value for key, (value, _) in
                          LINT.workflow_mapping_block(caller, "permissions", 4).items()},
                         {"contents": "read", "actions": "read"})
        self.assertEqual({key: value for key, (value, _) in
                          LINT.workflow_mapping_block(caller, "with", 4).items()},
                         {"expected-source-sha": "${{ inputs.expected-source-sha }}"})
        self.assertFalse({"Gate", "Publish"} & set(LINT.workflow_job_blocks(WORKFLOW.read_text())))

    def test_host_setup_failure_still_uploads_incomplete_diagnostic(self):
        text = WORKFLOW.read_text()
        for job in ("IosIntel", "IosArm"):
            steps = [LINT.workflow_step_fields(step)[0]
                     for step in LINT.workflow_job_steps(text)[job]]
            bootstrap_steps = [index for index, step in enumerate(steps)
                               if step.get("name") == "Initialize incomplete host diagnostic"]
            self.assertEqual(len(bootstrap_steps), 1, "host setup must leave a diagnostic on failure")
            bootstrap = bootstrap_steps[0]
            install = next(index for index, step in enumerate(steps)
                           if step.get("name") == "Install iOS source-build tools for diagnostic only")
            self.assertLess(bootstrap, install)
            self.assertIn("--stub", steps[bootstrap]["run"])
            self.assertEqual(steps[-1].get("if"), "${{ always() }}")

    def test_repository_lint_guards_diagnostic_workflow_mutations(self):
        original = WORKFLOW.read_text()
        self.assertEqual(LINT.ci_ios_host_evidence_workflow_issues(original), [])
        for changed in (original.replace("  contents: read\n", "  contents: write\n", 1),
                        original.replace("jobs:\n", "jobs:\n  Publish:\n    runs-on: ubuntu-24.04\n", 1),
                        original.replace("        if: ${{ always() }}", "        continue-on-error: true", 1)):
            self.assertTrue(LINT.ci_ios_host_evidence_workflow_issues(changed))

    @unittest.skipUnless(WORKFLOW.is_file(), "waiting for diagnostic workflow")
    def test_source_preflight_rejects_fork_tag_default_branch_and_changed_sha(self):
        text = WORKFLOW.read_text()
        records = [LINT.workflow_step_fields(step)[0]
                   for step in LINT.workflow_job_steps(text)["Source"]]
        self.assertEqual(sum(row.get("name") == "Validate exact diagnostic source"
                             for row in records), 1)
        raw_step = text.split("      - name: Validate exact diagnostic source\n", 1)[1].split(
            "\n      - name:", 1)[0]
        preflight_script = textwrap.dedent(raw_step.split("        run: |\n", 1)[1])
        sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        defaults = {**os.environ, "GITHUB_REPOSITORY": "ZEACENT/klogg",
                    "GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_REF": "refs/heads/worktree-master-ci-fail",
                    "GITHUB_SHA": sha, "KLOGG_EXPECTED_SOURCE_SHA": sha}
        for changes, valid in (({}, True), ({"GITHUB_REPOSITORY": "fork/klogg"}, False),
                               ({"GITHUB_REF": "refs/tags/v1"}, False),
                               ({"GITHUB_REF": "refs/heads/master"}, False),
                               ({"GITHUB_SHA": "a" * 40}, False),
                               ({"KLOGG_EXPECTED_SOURCE_SHA": "0" * 40}, False)):
            with self.subTest(changes=changes):
                result = subprocess.run(["bash", "-c", preflight_script], cwd=ROOT,
                                        env={**defaults, **changes}, capture_output=True,
                                        text=True, timeout=15)
                self.assertEqual(result.returncode == 0, valid, result.stdout + result.stderr)

    @unittest.skipUnless(WORKFLOW.is_file(), "waiting for diagnostic workflow")
    def test_workflow_mutations_cannot_add_publication_or_skip_diagnostics(self):
        original = WORKFLOW.read_text()
        examples = (
            ("manual entry", "  workflow_call:\n", "  workflow_dispatch:\n  workflow_call:\n"),
            ("write", "  contents: read\n", "  contents: write\n"),
            ("extra job", "jobs:\n", "jobs:\n  Publish:\n    runs-on: ubuntu-24.04\n"),
            ("ignored failure", "      - name: Validate exact diagnostic source\n",
             "      - name: Validate exact diagnostic source\n        continue-on-error: true\n"),
            ("comment spoof", "python3 scripts/prefetch_ios_native_sources.py",
             "# python3 scripts/prefetch_ios_native_sources.py"),
            ("unreviewed action", "      - name: Probe unreviewed host observation capability\n",
             "      - uses: example/unsafe-action@v1\n      - name: Probe unreviewed host observation capability\n"),
            ("core upload", "ios-host-evidence-", "native-core-candidate-"),
        )
        for label, before, after in examples:
            with self.subTest(label=label):
                self.assertIn(before, original)
                self.assertTrue(diagnostic_issues(original.replace(before, after, 1)))


class IosHostEvidenceCollectorTest(unittest.TestCase):
    def evaluate(self, observation: dict, marker: str):
        self.assertTrue(COLLECTOR.is_file(), "missing isolated diagnostic collector")
        sys.path.insert(0, str(ROOT / "scripts"))
        try:
            import ci_ios_host_evidence as collector
            result = collector.evaluate_observation(observation)
        finally:
            sys.path.remove(str(ROOT / "scripts"))
        self.assertIs(result.get("complete"), False)
        self.assertNotIn("qualification", result)
        self.assertNotIn("policy_sha256", result)
        self.assertIn(marker, " ".join(result.get("missing", [])).lower())

    def test_selected_tool_versions_are_not_actual_build_closure(self):
        self.evaluate({"tools": {"autoreconf": {"version": "2.73"}},
                       "homebrew": {"autoconf": "autoconf 2.73"}}, "trace")

    def test_unavailable_or_truncated_process_trace_is_incomplete(self):
        for trace in ({"available": False}, {"available": True, "truncated": True},
                      {"available": True, "error": "permission denied"}):
            with self.subTest(trace=trace):
                self.evaluate({"tools": {}, "trace": trace}, "trace")

    def test_extra_execution_without_resolved_bytes_is_incomplete(self):
        self.evaluate({"trace": {"available": True, "truncated": False,
                                  "executions": [{"argv": ["/usr/local/bin/autom4te"],
                                                  "resolved_path": None}]}}, "exec")

    def test_interpreter_or_module_without_hash_is_incomplete(self):
        for missing in ("interpreter", "module"):
            with self.subTest(missing=missing):
                observation = {"trace": {"available": True, "truncated": False},
                               "interpreters": [{"path": "/usr/bin/perl", "sha256": ""}],
                               "modules": [{"path": "/usr/local/lib/perl5/Config.pm", "sha256": ""}]}
                if missing == "interpreter":
                    observation.pop("modules")
                else:
                    observation.pop("interpreters")
                self.evaluate(observation, missing)

    def test_brew_version_without_verified_bottle_and_dependencies_is_incomplete(self):
        self.evaluate({"homebrew": {"pkgconf": "pkgconf 3.0.5"},
                       "bottles": [{"formula": "pkgconf", "sha256": None,
                                    "dependencies": []}]}, "bottle")

    def test_unresolved_host_dylib_is_incomplete(self):
        self.evaluate({"dylibs": [{"install_name": "@rpath/libfoo.dylib",
                                   "resolved_path": None}]}, "dylib")

    def test_unknown_fields_cannot_create_qualification_evidence(self):
        self.evaluate({"trace": {"available": True}, "qualified": True,
                       "native_qualified": True, "receipt_kind": "ios-native-build"}, "unknown")

    def test_formula_metadata_cannot_claim_poured_bottle_verification(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        try:
            import ci_ios_host_evidence as collector
            formula = {"name": "pkgconf", "installed": [{"version": "3.0.5"}],
                       "dependencies": ["gettext"], "build_dependencies": ["m4"],
                       "bottle": {"stable": {"files": {
                           "arm64_sonoma": {"sha256": "a" * 64}}}}}
            completed = subprocess.CompletedProcess(["brew"], 0,
                                                    json.dumps({"formulae": [formula]}), "")
            with mock.patch.object(collector.subprocess, "run", return_value=completed):
                observed = collector.observe_brew_metadata()
        finally:
            sys.path.remove(str(ROOT / "scripts"))
        pkgconf = next(item for item in observed if item["formula"] == "pkgconf")
        self.assertEqual(pkgconf["dependencies"], ["gettext", "m4"])
        self.assertEqual(pkgconf["published_bottle_sha256"], {"arm64_sonoma": "a" * 64})
        self.assertIsNone(pkgconf["sha256"])
        self.assertFalse(pkgconf["archive_verified"])

    def test_exec_trace_probe_uses_unrestricted_compiled_child(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        try:
            import ci_ios_host_evidence as collector
            commands = []

            def fake_run(command, **options):
                commands.append(command)
                if command[0] == "sudo":
                    return subprocess.CompletedProcess(command, 0, "1234\n", "")
                return subprocess.CompletedProcess(command, 0, "", "")

            with mock.patch.object(collector.subprocess, "run", side_effect=fake_run):
                self.assertTrue(collector._dtrace_capability()["dtrace_exec_probe"])
        finally:
            sys.path.remove(str(ROOT / "scripts"))
        self.assertEqual(len(commands), 2)
        self.assertEqual(commands[0][0], "clang")
        self.assertNotEqual(commands[1][commands[1].index("-c") + 1], "/usr/bin/true")

    def test_selected_tool_failure_cannot_start_unobserved_build(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        try:
            import ci_ios_host_evidence as collector
            from ci_dependency_toolchain import ToolchainError
            with tempfile.TemporaryDirectory() as directory:
                output = pathlib.Path(directory) / "evidence.json"
                environment = {"GITHUB_SHA": "a" * 40, "KLOGG_EXPECTED_SOURCE_SHA": "a" * 40,
                               "GITHUB_REPOSITORY": "ZEACENT/klogg"}
                with mock.patch.dict(os.environ, environment), \
                        mock.patch.object(collector.platform, "machine", return_value="x86_64"), \
                        mock.patch.object(collector, "observe_ios_host_tools",
                                          side_effect=ToolchainError("missing selected iOS host tool: m4")), \
                        mock.patch.object(collector, "observe_brew_metadata", return_value=[]), \
                        mock.patch.object(collector, "_dtrace_capability",
                                          return_value={"dtrace_exec_probe": True}):
                    self.assertEqual(collector.main(["--target-id", "ios-x86_64", "--output", str(output),
                                                     "--preflight"]), 1)
                self.assertIn("m4", json.loads(output.read_text())["tool_observation_error"])
        finally:
            sys.path.remove(str(ROOT / "scripts"))

    def test_probe_writes_bounded_incomplete_same_run_evidence(self):
        sys.path.insert(0, str(ROOT / "scripts"))
        try:
            import ci_ios_host_evidence as collector
            with tempfile.TemporaryDirectory() as directory:
                output = pathlib.Path(directory) / "evidence.json"
                environment = {"GITHUB_SHA": "a" * 40, "KLOGG_EXPECTED_SOURCE_SHA": "a" * 40,
                               "GITHUB_REPOSITORY": "ZEACENT/klogg", "GITHUB_REF": "refs/heads/worktree-master-ci-fail",
                               "GITHUB_RUN_ID": "123", "GITHUB_RUN_ATTEMPT": "1"}
                selected = {"tools": {}, "homebrew": {}}
                with mock.patch.dict(os.environ, environment), \
                        mock.patch.object(collector.platform, "machine", return_value="x86_64"), \
                        mock.patch.object(collector, "observe_ios_host_tools", return_value=selected), \
                        mock.patch.object(collector, "observe_brew_metadata", return_value=[]), \
                        mock.patch.object(collector, "_dtrace_capability", return_value={"dtrace_exec_probe": False}):
                    self.assertEqual(collector.main(["--target-id", "ios-x86_64", "--output", str(output),
                                                     "--preflight"]), 1)
                report = json.loads(output.read_text())
                self.assertFalse(report["complete"])
                self.assertEqual(report["source"]["sha"], "a" * 40)
                self.assertEqual(report["source"]["run_attempt"], "1")
                self.assertIn("trace", " ".join(report["missing"]))
                self.assertLess(output.stat().st_size, collector.MAX_EVIDENCE_BYTES)
        finally:
            sys.path.remove(str(ROOT / "scripts"))


if __name__ == "__main__":
    unittest.main()
