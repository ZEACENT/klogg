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
PREFIX = ("${{ github.event_name == 'workflow_dispatch' && "
          "(inputs.environment-mode != 'off' || inputs.dependency-mode != 'off') "
          "&& '[producer-mode skipped] ' || '' }}")
NATIVE_PREFIX = ("${{ github.event_name == 'workflow_dispatch' && inputs.environment-mode != 'off' "
                 "&& '[producer-mode skipped] ' || '' }}")
ORDINARY = "(github.event_name != 'workflow_dispatch' || (inputs.environment-mode == 'off' && inputs.dependency-mode == 'off'))"
PRIOR_INPUTS = ("prior-source-run-id", "prior-source-artifact-id", "prior-source-run-attempt", "prior-source-sha")
PRIOR_ENV = tuple("KLOGG_" + name.upper().replace("-", "_") for name in PRIOR_INPUTS)


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


def event_expression(expression, event, environment_mode, dependency_mode="off", skipped=False):
    text = expression[3:] if expression.startswith("${{") else expression
    text = text[:-2] if text.endswith("}}") else text
    text = text.strip()
    text = (text.replace("github.event_name", repr(event))
            .replace("inputs.environment-mode", repr(environment_mode))
            .replace("inputs.dependency-mode", repr(dependency_mode)))
    text = text.replace("!contains(github.event.head_commit.message, '[skip ci]')", repr(not skipped))
    preflight = ("success" if event == "workflow_dispatch" and dependency_mode != "off"
                 and environment_mode == "off" else "skipped")
    text = text.replace("needs.DependencyModePreflight.result", repr(preflight))
    text = text.replace("!cancelled()", "True").replace("always()", "True")
    text = text.replace("&&", "and").replace("||", "or")
    parsed = ast.parse(text, mode="eval")
    allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.Compare, ast.Eq, ast.NotEq, ast.Constant)
    if not all(isinstance(node, allowed) for node in ast.walk(parsed)):
        raise AssertionError("unmodeled event expression")
    return eval(compile(parsed, "<event projection>", "eval"), {"__builtins__": {}})


class EnvironmentBootstrapTest(unittest.TestCase):
    def test_macos_application_jobs_pin_hosts_and_xcode_before_shared_configuration(self):
        text = CI_BUILD.read_text()
        blocks = QUALITY.workflow_job_blocks(text)
        steps = QUALITY.workflow_job_steps(text)
        for job, runner, minimum in (("MacPackages", "macos-26-intel", "15.0"),
                                     ("MacArmPackages", "macos-26", "14.0")):
            with self.subTest(job=job):
                block = blocks[job]
                self.assertEqual(QUALITY.workflow_job_direct_value(block, "runs-on"), runner)
                env = QUALITY.workflow_mapping_block(block, "env", 4)
                self.assertEqual(env["KLOGG_CONFIG_OS"][0], runner)
                self.assertIn("-DKLOGG_OSX_DEPLOYMENT_TARGET=" + minimum, env["KLOGG_CONFIG_CMAKE_OPTS"][0])
        sanitizer = blocks["MacSanitizers"]
        self.assertEqual(QUALITY.workflow_job_direct_value(sanitizer, "runs-on"), "${{ matrix.config.os }}")
        sanitizer_text = "\n".join(sanitizer)
        self.assertEqual(sanitizer_text.count("          - os: macos-26-intel\n"), 2)
        self.assertEqual(sanitizer_text.count("-DKLOGG_OSX_DEPLOYMENT_TARGET=15.0"), 2)
        for label in ("intel-qt6-asan-ubsan", "intel-qt6-first-party-tsan"):
            self.assertIn("label: " + label, sanitizer_text)
        self.assertNotIn("macos-15-intel", sanitizer_text)
        selected = [QUALITY.workflow_step_fields(step)[0] for step in steps["MacPackages"]]
        names = [step.get("name", "") for step in selected]
        self.assertEqual(names.count("Select verified Xcode 26.6"), 1)
        index = names.index("Select verified Xcode 26.6")
        self.assertEqual(index, 1)
        self.assertLess(index, names.index("Brew deps"))
        self.assertLess(index, names.index("Disable Sentry on macOS"))
        self.assertEqual(selected[index].get("shell"), "bash")
        self.assertNotIn("if", selected[index])
        self.assertNotIn("continue-on-error", selected[index])
        self.assertIn("17F113", QUALITY.active_script_content(selected[index].get("run", "")))
        self.assertNotIn("sudo xcode-select", text)
        for job in ("MacArmPackages", "MacSanitizers"):
            self.assertEqual([QUALITY.workflow_step_fields(step)[0] for step in steps[job]], selected)

    def test_registered_dispatch_exposes_explicit_environment_mode_and_source_pins(self):
        text = CI_BUILD.read_text()
        self.assertIn("      environment-mode:\n", text)
        self.assertIn("      dependency-mode:\n", text)
        self.assertIn("      expected-source-sha:\n", text)
        self.assertIn("      analysis-base-sha:\n", text)
        blocks = QUALITY.workflow_job_blocks(text)
        self.assertEqual(QUALITY.workflow_job_direct_value(blocks.get("EnvironmentProducer", []), "uses"),
                         "./.github/workflows/ci-environments.yml")

    def test_prior_source_tuple_is_optional_and_only_forwarded_to_environment_producer(self):
        parent = CI_BUILD.read_text()
        child = PRODUCER.read_text()
        dispatch = QUALITY.workflow_mapping_block(parent.splitlines(), "inputs", 4)
        call = QUALITY.workflow_mapping_block(QUALITY.workflow_job_blocks(parent)["EnvironmentProducer"], "with", 4)
        child_inputs = QUALITY.workflow_mapping_block(child.splitlines(), "inputs", 4)
        self.assertIsNotNone(dispatch)
        self.assertIsNotNone(call)
        self.assertIsNotNone(child_inputs)
        for name in PRIOR_INPUTS:
            with self.subTest(name=name):
                self.assertIn(name, dispatch)
                values = QUALITY.workflow_mapping_block(dispatch[name][1], name, 6)
                self.assertEqual({key: value for key, (value, _) in values.items() if key in {"required", "default", "type"}},
                                 {"required": "false", "default": "", "type": "string"})
                self.assertEqual(call[name][0], "${{ inputs." + name + " }}")
                self.assertIn(name, child_inputs)
                values = QUALITY.workflow_mapping_block(child_inputs[name][1], name, 6)
                self.assertEqual({key: value for key, (value, _) in values.items() if key in {"required", "default", "type"}},
                                 {"required": "false", "default": "", "type": "string"})

    def test_prior_tuple_rejected_by_ordinary_events_and_dependency_dispatch(self):
        text = CI_BUILD.read_text()
        payload = python_payload(text, "DependencyModePreflight", "Reject prior environment source inputs outside producer dispatch")
        base = {name: "" for name in PRIOR_ENV}
        prior = {**base, PRIOR_ENV[0]: "123"}
        for event, environment, dependency, values, accepted in (
            ("pull_request", "off", "off", base, True),
            ("push", "off", "off", base, True),
            ("workflow_dispatch", "off", "off", base, True),
            ("workflow_dispatch", "off", "qualify", base, True),
            ("workflow_dispatch", "qualify", "off", base, True),
            ("pull_request", "off", "off", prior, False),
            ("push", "off", "off", prior, False),
            ("workflow_dispatch", "off", "off", prior, False),
            ("workflow_dispatch", "off", "qualify", prior, False),
            ("workflow_dispatch", "off", "publish", prior, False),
            ("workflow_dispatch", "qualify", "off", prior, True),
        ):
            with self.subTest(event=event, environment=environment, dependency=dependency, accepted=accepted):
                env = {"GITHUB_EVENT_NAME": event, "KLOGG_ENVIRONMENT_MODE": environment,
                       "KLOGG_DEPENDENCY_MODE": dependency, **values}
                with mock.patch.dict(os.environ, env):
                    if accepted:
                        exec(compile(payload, "<prior isolation>", "exec"), {})
                    else:
                        with self.assertRaises(SystemExit):
                            exec(compile(payload, "<prior isolation>", "exec"), {})

    def test_environment_preflight_rejects_partial_or_invalid_prior_tuple(self):
        payload = python_payload(CI_BUILD.read_text(), "EnvironmentModePreflight", "Validate isolated producer mode and exact source")
        valid = {"KLOGG_ENVIRONMENT_MODE": "qualify", "KLOGG_QUALIFICATION_MODE": "validation",
                 "KLOGG_EXPECTED_SOURCE_SHA": "1" * 40, "KLOGG_ANALYSIS_BASE_SHA": "2" * 40,
                 "GITHUB_SHA": "1" * 40, "GITHUB_REPOSITORY": "ZEACENT/klogg", "GITHUB_REF": "refs/heads/feature",
                 **dict.fromkeys(PRIOR_ENV, "")}
        complete = dict(zip(PRIOR_ENV, ("123", "456", "2", "3" * 40)))
        for values in ({}, complete):
            with mock.patch.dict(os.environ, {**valid, **values}):
                exec(compile(payload, "<prior preflight>", "exec"), {})
        invalid = ({PRIOR_ENV[0]: "123"}, {**complete, PRIOR_ENV[1]: ""},
                   {**complete, PRIOR_ENV[0]: "0"}, {**complete, PRIOR_ENV[2]: "01"},
                   {**complete, PRIOR_ENV[3]: "0" * 40}, {**complete, PRIOR_ENV[3]: "1" * 40})
        for values in invalid:
            with self.subTest(values=values), mock.patch.dict(os.environ, {**valid, **values}):
                with self.assertRaises(SystemExit):
                    exec(compile(payload, "<prior preflight>", "exec"), {})

    def test_direct_child_workflow_call_rejects_partial_prior_tuple_before_any_producer_work(self):
        steps = [QUALITY.workflow_step_fields(step) for step in QUALITY.workflow_job_steps(PRODUCER.read_text())["Source"]]
        validation = [(index, fields, children) for index, (fields, children) in enumerate(steps)
                      if fields.get("name") == "Reject partial prior source tuple at reusable boundary"]
        self.assertEqual(len(validation), 1)
        index, fields, children = validation[0]
        self.assertEqual(fields.get("shell"), "bash")
        self.assertIsNone(fields.get("if"))
        self.assertEqual(children.get("env"), {name: "${{ inputs." + source + " }}"
                                               for name, source in zip(PRIOR_ENV, PRIOR_INPUTS)})
        self.assertLess(index, next(i for i, (item, _) in enumerate(steps)
                                    if item.get("name") == "Validate exact producer source and operation"))
        payload = python_payload(PRODUCER.read_text(), "Source", "Reject partial prior source tuple at reusable boundary")
        complete = dict(zip(PRIOR_ENV, ("123", "456", "2", "3" * 40)))
        for values in (dict.fromkeys(PRIOR_ENV, ""), complete):
            with self.subTest(values=values), mock.patch.dict(os.environ, {"KLOGG_EXPECTED_SOURCE_SHA": "1" * 40, **values}):
                exec(compile(payload, "<child prior gate>", "exec"), {})
        for values in ({PRIOR_ENV[1]: "456"}, {PRIOR_ENV[3]: "3" * 40},
                       {**complete, PRIOR_ENV[0]: ""}, {**complete, PRIOR_ENV[2]: ""},
                       {**complete, PRIOR_ENV[0]: "0"}, {**complete, PRIOR_ENV[2]: "01"},
                       {**complete, PRIOR_ENV[3]: "1" * 40}):
            with self.subTest(values=values), mock.patch.dict(os.environ, {"KLOGG_EXPECTED_SOURCE_SHA": "1" * 40,
                                                                  **dict.fromkeys(PRIOR_ENV, ""), **values}):
                with self.assertRaises(SystemExit):
                    exec(compile(payload, "<child prior gate>", "exec"), {})

    def test_linux_fixture_only_imports_prior_bytes_before_fixture_preparation(self):
        steps = QUALITY.workflow_job_steps(PRODUCER.read_text())
        fixture = [QUALITY.workflow_step_fields(step) for step in steps["LinuxFixture"]]
        commands = [(index, tokens) for index, step in enumerate(steps["LinuxFixture"])
                    for tokens in QUALITY.environment_shell_commands(step)]
        imports = [(index, tokens) for index, tokens in commands
                   if tokens[:2] == ["python3", "scripts/ci_environment_source_cache.py"]]
        self.assertEqual(len(imports), 1)
        index, command = imports[0]
        self.assertIn('"$KLOGG_PRIOR_SOURCE_RUN_ID"', fixture[index][0].get("run", ""))
        self.assertEqual(command[2:], ["--lock", "packaging/adb/adb-helper.lock.json",
                                       "--repo-root", "$GITHUB_WORKSPACE", "--source-sha", "$KLOGG_EXPECTED_SOURCE_SHA",
                                       "--run-id", "$KLOGG_PRIOR_SOURCE_RUN_ID", "--run-attempt", "$KLOGG_PRIOR_SOURCE_RUN_ATTEMPT",
                                       "--artifact-id", "$KLOGG_PRIOR_SOURCE_ARTIFACT_ID", "--prior-sha", "$KLOGG_PRIOR_SOURCE_SHA",
                                       "--output-root", "$RUNNER_TEMP/source-cache-import"])
        prepare = next(position for position, tokens in commands if tokens[:3] ==
                       ["python3", "scripts/ci_environment_pipeline.py", "prepare-fixture"])
        self.assertLess(index, prepare)
        self.assertIn('source_cache_args+=(--source-cache-root "$RUNNER_TEMP/source-cache-import")',
                      fixture[prepare][0].get("run", ""))
        self.assertIn('"${source_cache_args[@]}"', fixture[prepare][0].get("run", ""))
        for job, job_steps in steps.items():
            if job != "LinuxFixture":
                self.assertNotIn("ci_environment_source_cache.py", "\n".join("\n".join(step) for step in job_steps))

    def test_skipped_producer_mode_jobs_cannot_emit_ordinary_required_check_names(self):
        blocks = QUALITY.workflow_job_blocks(CI_BUILD.read_text())
        for job in QUALITY.CI_BUILD_REQUIRED_JOBS:
            with self.subTest(job=job):
                name = QUALITY.workflow_job_direct_value(blocks[job], "name")
                prefix = NATIVE_PREFIX if job in QUALITY.CI_BUILD_NATIVE_JOBS else PREFIX
                self.assertTrue(name.startswith(prefix), name)

    def test_lint_rejects_skipped_normal_gate_name_not_only_gate_execution_condition(self):
        text = CI_BUILD.read_text()
        mutated = mutate_job(text, "ci-gate", PREFIX + "ci-gate", "ci-gate")
        self.assertNotEqual(text, mutated)
        self.assertIn("CI producer mode must distinguish every skipped ordinary check name: ci-gate",
                      QUALITY.ci_build_workflow_issues(mutated))

    def test_normal_event_names_and_gate_behavior_are_unchanged_but_producer_names_are_distinct(self):
        blocks = QUALITY.workflow_job_blocks(CI_BUILD.read_text())
        cases = (("push", "off", "off", True), ("pull_request", "off", "off", True),
                 ("workflow_dispatch", "off", "off", True),
                 ("workflow_dispatch", "qualify", "off", False),
                 ("workflow_dispatch", "publish", "off", False),
                 ("workflow_dispatch", "off", "qualify", False),
                 ("workflow_dispatch", "off", "publish", False),
                 ("workflow_dispatch", "qualify", "publish", False),
                 ("push", "publish", "qualify", True))
        for event, environment_mode, dependency_mode, ordinary in cases:
            with self.subTest(event=event, environment_mode=environment_mode, dependency_mode=dependency_mode):
                for job in QUALITY.CI_BUILD_REQUIRED_JOBS:
                    fields = blocks[job]
                    name = QUALITY.workflow_job_direct_value(fields, "name")
                    native = job in QUALITY.CI_BUILD_NATIVE_JOBS
                    prefix = NATIVE_PREFIX if native else PREFIX
                    self.assertTrue(name.startswith(prefix))
                    actual = event_expression(prefix, event, environment_mode, dependency_mode) + name[len(prefix):]
                    should_prefix = event == "workflow_dispatch" and (environment_mode != "off" or
                                    (dependency_mode != "off" and not native))
                    self.assertEqual(actual.startswith("[producer-mode skipped] "), should_prefix)
                    if job == "DispatchContinuous":
                        self.assertIn("github.event_name == 'push'", QUALITY.workflow_job_direct_value(fields, "if"))
                        continue
                    condition = QUALITY.workflow_job_direct_value(fields, "if")
                    native_selected = event == "workflow_dispatch" and dependency_mode in {"qualify", "publish"}
                    selected = ordinary or (native and native_selected and
                                            (environment_mode == "off" or job not in QUALITY.CI_BUILD_NATIVE_ROOT_JOBS))
                    self.assertEqual(bool(event_expression(condition, event, environment_mode, dependency_mode)), selected)
                    self.assertEqual(bool(event_expression(condition, event, environment_mode, dependency_mode, skipped=True)),
                                     not native and ordinary and job == "ci-gate")

    def test_every_ordinary_name_shadow_mutation_is_rejected(self):
        text = CI_BUILD.read_text()
        for job in QUALITY.CI_BUILD_REQUIRED_JOBS:
            with self.subTest(job=job):
                prefix = NATIVE_PREFIX if job in QUALITY.CI_BUILD_NATIVE_JOBS else PREFIX
                mutated = mutate_job(text, job, prefix, "")
                self.assertIn("CI producer mode must distinguish every skipped ordinary check name: " + job,
                              QUALITY.ci_build_environment_mode_issues(mutated))

    def test_ordinary_guard_cannot_omit_dependency_mode(self):
        text = CI_BUILD.read_text()
        old_guard = ORDINARY
        environment_only = "(github.event_name != 'workflow_dispatch' || inputs.environment-mode == 'off')"
        for job in ("BuildAdbLinuxX64", "ci-gate"):
            with self.subTest(job=job):
                mutated = mutate_job(text, job, old_guard, environment_only)
                self.assertIn("CI job must isolate ordinary and native dependency dispatch: " + job,
                              QUALITY.ci_build_environment_mode_issues(mutated))

    def test_isolated_environment_producer_excludes_mixed_modes_with_native_preflight_noop(self):
        blocks = QUALITY.workflow_job_blocks(CI_BUILD.read_text())
        cases = (("push", "off", "off", True, False, False),
                 ("pull_request", "off", "off", True, False, False),
                 ("workflow_dispatch", "off", "off", True, False, False),
                 ("workflow_dispatch", "qualify", "off", True, True, True),
                 ("workflow_dispatch", "publish", "off", True, True, True),
                 ("workflow_dispatch", "off", "qualify", True, False, False),
                 ("workflow_dispatch", "off", "publish", True, False, False),
                 ("workflow_dispatch", "off", "observe-ios-host", True, False, False),
                 ("workflow_dispatch", "qualify", "publish", True, False, False),
                 ("push", "qualify", "publish", True, False, False))
        for event, environment_mode, dependency_mode, dependency, preflight, producer in cases:
            with self.subTest(event=event, environment_mode=environment_mode, dependency_mode=dependency_mode):
                for job, expected in (("DependencyModePreflight", dependency),
                                      ("EnvironmentModePreflight", preflight),
                                      ("EnvironmentProducer", producer)):
                    condition = QUALITY.workflow_job_direct_value(blocks[job], "if")
                    observed = (condition is None if job == "DependencyModePreflight" else
                                bool(event_expression(condition, event, environment_mode, dependency_mode)))
                    self.assertEqual(observed, expected, job)

    def test_mixed_mode_cannot_launch_environment_producer_or_preflight(self):
        text = CI_BUILD.read_text()
        for job, old, new, issue in (
            ("EnvironmentModePreflight", "inputs.dependency-mode == 'off'", "inputs.dependency-mode != 'off'",
             "CI environment dispatch preflight must be a read-only producer-only root"),
            ("EnvironmentProducer", "inputs.dependency-mode == 'off'", "inputs.dependency-mode != 'off'",
             "CI environment caller must use the exact source-local reusable workflow and narrow permission ceiling"),
        ):
            with self.subTest(job=job):
                mutated = mutate_job(text, job, old, new)
                self.assertIn(issue, QUALITY.ci_build_environment_mode_issues(mutated))

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
                 "GITHUB_SHA": "1" * 40, "GITHUB_REPOSITORY": "ZEACENT/klogg", "GITHUB_REF": "refs/heads/feature",
                 **dict.fromkeys(PRIOR_ENV, "")}
        with mock.patch.dict(os.environ, valid):
            exec(compile(payload, "<dispatch preflight>", "exec"), {})
        for key, value in (("KLOGG_QUALIFICATION_MODE", "release"), ("KLOGG_EXPECTED_SOURCE_SHA", ""),
                           ("KLOGG_EXPECTED_SOURCE_SHA", "3" * 40), ("KLOGG_ANALYSIS_BASE_SHA", ""),
                           ("KLOGG_ANALYSIS_BASE_SHA", "1" * 40), ("KLOGG_ENVIRONMENT_MODE", "anything"),
                           ("GITHUB_REPOSITORY", "fork/klogg"), ("GITHUB_REF", "refs/tags/release")):
            with self.subTest(key=key, value=value), mock.patch.dict(os.environ, {**valid, key: value}):
                with self.assertRaises(SystemExit):
                    exec(compile(payload, "<dispatch preflight>", "exec"), {})

    def test_dependency_preflight_rejects_unwired_qualify_publish_and_unsafe_sources(self):
        text = CI_BUILD.read_text()
        payload = python_payload(text, "DependencyModePreflight", "Validate isolated dependency mode and exact source")
        valid = {"KLOGG_DEPENDENCY_MODE": "qualify", "KLOGG_ENVIRONMENT_MODE": "off",
                 "KLOGG_QUALIFICATION_MODE": "validation", "KLOGG_EXPECTED_SOURCE_SHA": "1" * 40,
                 "GITHUB_SHA": "1" * 40, "GITHUB_REPOSITORY": "ZEACENT/klogg", "GITHUB_REF": "refs/heads/feature"}
        for mode in ("qualify", "publish", "observe-ios-host"):
            with self.subTest(mode=mode), mock.patch.dict(os.environ, {**valid, "KLOGG_DEPENDENCY_MODE": mode}):
                exec(compile(payload, "<dependency dispatch preflight>", "exec"), {})
        for key, value in (("KLOGG_DEPENDENCY_MODE", "off"), ("KLOGG_DEPENDENCY_MODE", "unknown"),
                           ("KLOGG_ENVIRONMENT_MODE", "qualify"), ("KLOGG_QUALIFICATION_MODE", "release"),
                           ("KLOGG_EXPECTED_SOURCE_SHA", ""), ("KLOGG_EXPECTED_SOURCE_SHA", "0" * 40),
                           ("KLOGG_EXPECTED_SOURCE_SHA", "3" * 40), ("GITHUB_REPOSITORY", "fork/klogg"),
                           ("GITHUB_REF", "refs/tags/release")):
            with self.subTest(key=key, value=value), mock.patch.dict(os.environ, {**valid, key: value}):
                with self.assertRaises(SystemExit):
                    exec(compile(payload, "<dependency dispatch preflight>", "exec"), {})
        for ref in ("refs/heads/master", "refs/heads/main"):
            with self.subTest(ref=ref), mock.patch.dict(os.environ, {
                    **valid, "KLOGG_DEPENDENCY_MODE": "observe-ios-host", "GITHUB_REF": ref}):
                with self.assertRaises(SystemExit):
                    exec(compile(payload, "<dependency dispatch preflight>", "exec"), {})
        for old, new in (("dependency-mode cannot combine with environment-mode", "mixed modes accepted"),
                         ("inputs.dependency-mode != 'off'", "inputs.dependency-mode == 'off'")):
            with self.subTest(mutation=old):
                mutated = mutate_job(text, "DependencyModePreflight", old, new)
                self.assertIn("CI dependency dispatch must reject mixed modes, noncanonical or stale source",
                              QUALITY.ci_build_environment_mode_issues(mutated))


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

    def test_direct_child_prior_gate_mutations_fail_closed(self):
        text = PRODUCER.read_text()
        cases = (
            ("if any(values) and not all(values):", "# if any(values) and not all(values):"),
            ("if any(values) and not all(values):", "print('if any(values) and not all(values):')"),
            ("if any(values) and not all(values):", "if all(values):"),
            ("      - name: Reject partial prior source tuple at reusable boundary\n",
             "      - name: Reject partial prior source tuple at reusable boundary\n        if: ${{ false }}\n"),
            ("          KLOGG_PRIOR_SOURCE_SHA: ${{ inputs.prior-source-sha }}",
             "          KLOGG_PRIOR_SOURCE_SHA: ${{ github.sha }}"),
            ("'KLOGG_PRIOR_SOURCE_RUN_ATTEMPT', 'KLOGG_PRIOR_SOURCE_SHA')",
             "'KLOGG_PRIOR_SOURCE_RUN_ATTEMPT', 'KLOGG_PRIOR_SOURCE_RUN_ATTEMPT')"),
        )
        for old, new in cases:
            with self.subTest(old=old, new=new):
                mutated = mutate_job(text, "Source", old, new)
                self.assertIn("Environment reusable Source must reject incomplete or malformed prior source tuples",
                              QUALITY.ci_environment_workflow_issues(mutated))

    def test_source_preflight_cannot_import_prior_bytes(self):
        text = PRODUCER.read_text()
        mutated = mutate_job(text, "Source", "          PY\n      - name: Validate exact producer source and operation",
                             "          PY\n          python3 scripts/ci_environment_source_cache.py --output-root /tmp/source\n"
                             "      - name: Validate exact producer source and operation")
        self.assertIn("Environment prior source bytes must stay inside LinuxFixture: Source",
                      QUALITY.ci_environment_workflow_issues(mutated))

    def test_prior_import_must_be_fixture_only_bounded_and_not_spoofed(self):
        text = PRODUCER.read_text()
        fixtures = (
            ("LinuxFixture", '"$KLOGG_PRIOR_SOURCE_RUN_ID"', '"$KLOGG_IGNORED"'),
            ("LinuxFixture", '--artifact-id "$KLOGG_PRIOR_SOURCE_ARTIFACT_ID"', '--artifact-id 42'),
            ("LinuxFixture", '--output-root "$RUNNER_TEMP/source-cache-import"', '--output-root "$GITHUB_WORKSPACE"'),
            ("LinuxFixture", 'python3 scripts/ci_environment_source_cache.py', '# python3 scripts/ci_environment_source_cache.py'),
            ("LinuxFixture", '--source-cache-root "$RUNNER_TEMP/source-cache-import"', '--source-cache-root "$GITHUB_WORKSPACE"'),
            ("CpmSources", '      - uses: ./.github/actions/prefetch-cpm-cache',
             '      - run: python3 scripts/ci_environment_source_cache.py --output-root /tmp/cache\n      - uses: ./.github/actions/prefetch-cpm-cache'),
        )
        for job, old, new in fixtures:
            with self.subTest(job=job, old=old):
                mutated = mutate_job(text, job, old, new)
                self.assertTrue(any("prior source" in issue.lower() or "fixture" in issue.lower()
                                    for issue in QUALITY.ci_environment_workflow_issues(mutated)))
        malformed = text.replace("      prior-source-sha:\n", "      prior-source-sha:\n        required: true\n", 1)
        self.assertTrue(QUALITY.ci_environment_workflow_issues(malformed))

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
