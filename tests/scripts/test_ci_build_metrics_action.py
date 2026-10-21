"""docker-build compile-database and metrics wiring; no real container runs."""
import ast
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
ACTION = ROOT / ".github/actions/docker-build/action.yml"
NATIVE_ACTION = ROOT / ".github/actions/agent-build/action.yml"
CI_BUILD = ROOT / ".github/workflows/ci-build.yml"
SPEC = importlib.util.spec_from_file_location("linux_validate_quality", ROOT / "scripts/lint_ci_quality.py")
QUALITY = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(QUALITY)


class DockerBuildMetricsActionTest(unittest.TestCase):
    def setUp(self):
        self.text = ACTION.read_text()
        steps = QUALITY.workflow_step_blocks(self.text.splitlines())
        self.bodies = {}
        for step in steps:
            fields = QUALITY.workflow_step_fields(step)[0]
            name = fields.get("name")
            if name and fields.get("run"):
                self.bodies[name] = "\n".join(step)

    def test_configure_exports_compile_commands_and_rejects_conflicts(self):
        body = self.bodies["configure"]
        self.assertIn("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", body)
        self.assertLess(body.index("KLOGG_CMAKE_OPTS"), body.index("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"))
        self.assertIn('cmake -B /usr/local/$KLOGG_BUILD_ROOT', body)
        self.assertNotIn("-DCMAKE_EXPORT_COMPILE_COMMANDS=$", body)

    def test_one_unchanged_build_is_measured_then_reported(self):
        body = self.bodies["build"]
        self.assertEqual(body.count("cmake --build /usr/local/$KLOGG_BUILD_ROOT --target ci_build"), 1)
        self.assertEqual(body.count("cmake --build"), 1)
        self.assertIn("ci_build_metrics.py run-build", body)
        self.assertIn("--repo-root /usr/local", body)
        self.assertIn("--build-root /usr/local/$KLOGG_BUILD_ROOT", body)
        self.assertIn("--object-cache", body)
        self.assertIn("ci-build-metrics-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT.json", body)
        self.assertNotIn("klogg_codeql_thirdparty", body)
        self.assertNotIn("rm ", body)

    def test_tsan_reports_cache_disabled_and_others_measure_global_counters(self):
        body = self.bodies["build"]
        self.assertIn('thread) KLOGG_OBJECT_CACHE=disabled', body)
        self.assertIn('KLOGG_OBJECT_CACHE=measured', body)

    def test_metrics_output_stays_in_the_build_root(self):
        body = self.bodies["build"]
        self.assertIn("--output /usr/local/$KLOGG_BUILD_ROOT/ci-build-metrics-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT.json", body)


class NativeBuildMetricsActionTest(unittest.TestCase):
    def setUp(self):
        steps = QUALITY.workflow_step_blocks(NATIVE_ACTION.read_text().splitlines())
        self.bodies = {fields["name"]: fields.get("run", "")
                       for step in steps for fields, _ in [QUALITY.workflow_step_fields(step)]
                       if "name" in fields}

    def test_native_configure_exports_compile_commands_without_allowing_off_override(self):
        body = self.bodies["configure"]
        self.assertIn("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", body)
        self.assertLess(body.index("$KLOGG_CMAKE_OPTS"), body.index("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"))
        self.assertIn("-DCMAKE_EXPORT_COMPILE_COMMANDS=OFF", body)
        self.assertIn("-DCMAKE_EXPORT_COMPILE_COMMANDS=0", body)
        self.assertIn("-DFETCHCONTENT_FULLY_DISCONNECTED=ON", body)

    def test_one_native_ci_build_reports_cache_only_where_available(self):
        body = self.bodies["build"]
        self.assertIn("scripts/ci_build_observation.py run-build", body)
        self.assertEqual(body.count("cmake --build"), 1)
        self.assertIn('Windows) object_cache=${KLOGG_WINDOWS_OBJECT_CACHE:-disabled}', body)
        self.assertIn('--cache-tool "${KLOGG_OBJECT_CACHE_TOOL:-ccache}"', body)
        self.assertIn('object_cache=measured', body)
        self.assertIn('--repo-root "$KLOGG_WORKSPACE"', body)
        self.assertIn('--build-root "$KLOGG_WORKSPACE/$KLOGG_BUILD_ROOT"', body)
        self.assertIn('--output "$KLOGG_WORKSPACE/$KLOGG_BUILD_ROOT/ci-build-metrics-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT.json"', body)
        self.assertIn('--object-cache "$object_cache"', body)
        self.assertIn('-- cmake --build "$KLOGG_BUILD_ROOT" -t ci_build', body)

    @unittest.skipUnless(shutil.which("sh"), "requires the action's POSIX shell")
    def test_execution_diagnostics_only_opt_in_macos_tsan_without_changing_build(self):
        body = self.bodies["build"]
        stub = ('python3() { printf \'%s\\n\' "$@" > "$CAPTURE"; '
                'printf \'%s\' "${NINJA_STATUS-}" > "$STATUS"; return "$FAKE_EXIT"; }\n')
        with tempfile.TemporaryDirectory(prefix="native action ") as directory:
            root = pathlib.Path(directory)
            for runner, sanitizer, code in (("macOS", "thread", 0),
                                            ("macOS", "thread", 9),
                                            ("macOS", "address-undefined", 0),
                                            ("macOS", "", 0),
                                            ("Windows", "thread", 0)):
                with self.subTest(runner=runner, sanitizer=sanitizer, code=code):
                    env = dict(os.environ, RUNNER_OS=runner, KLOGG_SANITIZER=sanitizer,
                               KLOGG_WORKSPACE=directory, KLOGG_BUILD_ROOT="build root",
                               GITHUB_RUN_ID="123", GITHUB_RUN_ATTEMPT="2",
                               CAPTURE=str(root / "args"), STATUS=str(root / "status"),
                               NINJA_STATUS="inherited-status", FAKE_EXIT=str(code))
                    result = subprocess.run([shutil.which("sh"), "-eu", "-c", stub + body],
                                            cwd=root, env=env, capture_output=True, text=True)
                    self.assertEqual(result.returncode, code, result.stderr)
                    args = (root / "args").read_text().splitlines()
                    status = (root / "status").read_text()
                    enabled = runner == "macOS" and sanitizer == "thread"
                    self.assertEqual("--execution-diagnostics" in args, enabled)
                    if enabled:
                        self.assertEqual(args[args.index("--execution-diagnostics") + 1],
                                         "macos-tsan")
                        self.assertIn("%r", status)
                        self.assertIn("%e", status)
                    else:
                        self.assertEqual(status, "inherited-status")
                    self.assertEqual(args[args.index("--") + 1:],
                                     ["cmake", "--build", "build root", "-t", "ci_build"])
                    self.assertEqual(args[args.index("--build-root") + 1],
                                     directory + "/build root")


class BuildMetricsUploadWorkflowTest(unittest.TestCase):
    def test_partial_tsan_diagnostics_upload_never_exports_build_objects(self):
        jobs = QUALITY.workflow_job_steps(CI_BUILD.read_text())
        for job in ("MacPackages", "MacArmPackages", "MacSanitizers"):
            with self.subTest(job=job):
                parsed = [QUALITY.workflow_step_fields(step) for step in jobs[job]]
                diagnostic = [(i, fields, children["with"])
                              for i, (fields, children) in enumerate(parsed)
                              if fields.get("name") == "Upload macOS TSan execution diagnostics"]
                self.assertEqual(len(diagnostic), 1)
                index, fields, options = diagnostic[0]
                build = max(i for i, (row, _) in enumerate(parsed)
                            if row.get("uses") == "./.github/actions/agent-build")
                self.assertGreater(index, build)
                self.assertEqual(fields["if"],
                                 "${{ always() && env.KLOGG_SANITIZER == 'thread' && "
                                 "hashFiles(format('{0}/ci-build-diagnostics.jsonl', "
                                 "env.KLOGG_BUILD_ROOT)) != '' }}")
                self.assertEqual(fields["continue-on-error"], "true")
                self.assertEqual(fields["timeout-minutes"], "2")
                self.assertEqual(options["path"].splitlines(), [
                    "${{ env.KLOGG_BUILD_ROOT }}/ci-build-diagnostics.jsonl",
                    "${{ env.KLOGG_BUILD_ROOT }}/ci-build-diagnostics.ninja-log.txt"])
                self.assertIn("attempt-${{ github.run_attempt }}", options["name"])
                self.assertTrue(options["name"].startswith("build-diagnostics-"))

    def test_windows_exact_restore_still_revalidates_after_qualification(self):
        steps = QUALITY.workflow_job_steps(CI_BUILD.read_text())["WindowsPackages"]
        save = next(QUALITY.workflow_step_fields(step)[0] for step in steps
                    if QUALITY.workflow_step_fields(step)[0].get("id") == "save-windows-cache")
        context = {"success": True, "steps.run-tests.outcome": "success",
                   "steps.windows-cache-report.outputs.validated": "true",
                   "steps.windows-cache.outputs.save-key": "windows-sccache-master-today",
                   "steps.restore-windows-cache.outputs.cache-matched-key": "windows-sccache-master-today"}
        # A seed can disappear between restore and qualification. The final
        # verifier must run even when the original restore was an exact hit.
        self.assertTrue(cache_expression(save["if"], context))
        for field, value in (("success", False), ("steps.run-tests.outcome", "failure"),
                             ("steps.windows-cache-report.outputs.validated", "false"),
                             ("steps.windows-cache.outputs.save-key", "")):
            with self.subTest(field=field):
                self.assertFalse(cache_expression(save["if"], dict(context, **{field: value})))

    def test_every_build_uploads_only_its_current_report_after_platform_observation(self):
        steps = QUALITY.workflow_job_steps(CI_BUILD.read_text())
        for job in QUALITY.CI_BUILD_PACKAGE_ENABLEMENT:
            with self.subTest(job=job):
                parsed = [QUALITY.workflow_step_fields(step) for step in steps[job]]
                builds = [index for index, (fields, _) in enumerate(parsed)
                          if fields.get("uses") in ("./.github/actions/docker-build",
                                                    "./.github/actions/agent-build")]
                metrics = [(index, fields, children.get("with")) for index, (fields, children)
                           in enumerate(parsed) if fields.get("name") == "Upload current-run build metrics"]
                observed_builds = list(builds)
                native = job.startswith(('Mac', 'Windows'))
                self.assertEqual(len(builds), 2 if native else 1)
                if native:
                    early, early_options = parsed[builds[0]]
                    self.assertEqual(early.get('if'), "${{ env.KLOGG_PACKAGE_ENABLED == 'true' }}")
                    self.assertEqual(early_options.get('with'), {'phase': 'prepare'})
                    final_phase = parsed[builds[-1]][1]['with']['phase']
                    self.assertEqual(final_phase,
                                     "${{ env.KLOGG_PACKAGE_ENABLED == 'true' && 'finalize' || 'full' }}")
                    context = {'success': True, 'env.KLOGG_PACKAGE_ENABLED': QUALITY.CI_BUILD_PACKAGE_ENABLEMENT[job]}
                    package = context['env.KLOGG_PACKAGE_ENABLED'] == 'true'
                    self.assertEqual(cache_expression(early['if'], context), package)
                    self.assertEqual(cache_expression(final_phase, context), 'finalize' if package else 'full')
                builds = builds[-1:]
                self.assertEqual(len(metrics), 1)
                index, fields, options = metrics[0]
                if job.startswith("Linux"):
                    self.assertEqual(index, builds[0] + 2)
                    binding, settings = parsed[builds[0] + 1]
                    self.assertEqual(binding.get("name"), "Bind build metrics to verified Linux environment")
                    self.assertEqual(settings.get("env"), {
                        "KLOGG_CI_ENVIRONMENT_IDENTITY": "${{ steps.linux-environment.outputs.image }}",
                        "KLOGG_CI_CACHE_ARTIFACT_ID": "${{ steps.restore-shared-compiler-cache.outputs.artifact-id }}",
                        "KLOGG_CI_CACHE_RESTORE_KEY": "${{ steps.restore-ccache.outputs.cache-matched-key }}"})
                    self.assertIn("scripts/ci_build_observation.py bind-environment", binding.get("run", ""))
                    self.assertIn('--identity "$KLOGG_CI_ENVIRONMENT_IDENTITY"', binding["run"])
                    resolver = next(row for row, _ in parsed if row.get("id") == "linux-environment")
                    self.assertEqual(resolver.get("uses"), "./.github/actions/prepare-linux-environment")
                elif job.startswith("Windows"):
                    report = next(position for position, (row, _) in enumerate(parsed)
                                  if row.get("id") == "windows-cache-report")
                    self.assertLess(builds[0], report)
                    self.assertGreater(index, report)
                    release_upload = next(position for position, (_, settings) in enumerate(parsed)
                                          if settings.get("with", {}).get("name", "").startswith("packages-"))
                    self.assertLess(index, release_upload)
                else:
                    self.assertEqual(index, builds[0] + 1)
                if not job.startswith("Linux"):
                    restore = ("steps.restore-windows-cache.outputs.cache-matched-key || "
                               "steps.restore-windows-master-cache.outputs.cache-matched-key"
                               if job.startswith("Windows") else
                               "steps.restore-pr-ccache.outputs.cache-matched-key || "
                               "steps.restore-ccache.outputs.cache-matched-key")
                    for build_index in observed_builds:
                        self.assertEqual(parsed[build_index][1].get("env"), {
                            "KLOGG_CI_CACHE_ARTIFACT_ID": "${{ steps.restore-shared-compiler-cache.outputs.artifact-id }}",
                            "KLOGG_CI_CACHE_RESTORE_KEY": "${{ " + restore + " }}"})
                self.assertEqual(fields.get("uses"), "actions/upload-artifact@" +
                                 QUALITY.REVIEWED_ACTION_REVISIONS["actions/upload-artifact"])
                self.assertEqual(fields.get("if"),
                                 "${{ " + ("always() && " if job.startswith("Windows") else "") +
                                 "hashFiles(format('{0}/ci-build-metrics-{1}-{2}.json', "
                                 "env.KLOGG_BUILD_ROOT, github.run_id, github.run_attempt)) != '' }}")
                self.assertEqual(fields.get("continue-on-error"), "true")
                self.assertEqual(options, {
                    "name": "build-metrics-${{ env.KLOGG_LABEL }}-${{ env.KLOGG_CONFIG_PACKAGE_TAG }}-attempt-${{ github.run_attempt }}",
                    "path": ("${{ env.KLOGG_BUILD_ROOT }}/ci-build-metrics-"
                             "${{ github.run_id }}-${{ github.run_attempt }}.json"),
                    "if-no-files-found": "warn", "retention-days": "7", "compression-level": "0",
                })


def cache_expression(expression, context):
    """Project only the small expression subset used by the actual cache steps."""
    text = expression.strip()
    if text.startswith("${{") and text.endswith("}}"):
        text = text[3:-2].strip()
    text = re.sub(r"toJSON\(([\w.\-]+)\)",
                  lambda match: repr(json.dumps(context[match[1]], separators=(",", ":"))), text)
    text = text.replace("success()", repr(context["success"]))
    for name in sorted(context, key=len, reverse=True):
        if name != "success":
            text = text.replace(name, repr(context[name]))
    text = text.replace("&&", " and ").replace("||", " or ")
    text = re.sub(r"(?<!['\w])false(?!['\w])", "False", text)
    text = re.sub(r"(?<!['\w])true(?!['\w])", "True", text)
    parsed = ast.parse(text, mode="eval")
    allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.Compare, ast.Eq,
               ast.NotEq, ast.Gt, ast.Constant)
    if not all(isinstance(node, allowed) for node in ast.walk(parsed)):
        raise AssertionError("unmodeled compiler cache expression: " + expression)
    return eval(compile(parsed, "<compiler cache event>", "eval"), {"__builtins__": {}})


def cache_template(template, context):
    return re.sub(r"\$\{\{(.*?)\}\}", lambda match: str(cache_expression(match[1], context)), template)


class NativeCompilerCacheWorkflowTest(unittest.TestCase):
    MAC_JOBS = ("MacPackages", "MacArmPackages", "MacSanitizers")
    SHARED_KEY = "${{ env.KLOGG_LABEL }}-ccache-content-v1-${{ steps.ccache-generation.outputs.current }}"
    PATH = "${{ github.workspace }}/.ccache"
    REVISION = QUALITY.REVIEWED_ACTION_REVISIONS["actions/cache"]

    def rows(self, text=None, job="MacPackages"):
        steps = QUALITY.workflow_job_steps(CI_BUILD.read_text() if text is None else text)[job]
        return [QUALITY.workflow_step_fields(step) for step in steps]

    def named(self, rows, name):
        found = [row for row in rows if row[0].get("name") == name]
        self.assertEqual(len(found), 1, "missing or duplicate compiler cache step: " + name)
        return found[0]

    def context(self, event="push", success=True, label="intel-qt6", number=123,
                repository="ZEACENT/klogg", head="ZEACENT/klogg", fork=True,
                head_id=999, repository_id="999"):
        return {"github.event_name": event, "github.repository": repository,
                "github.repository_id": repository_id,
                "github.event.pull_request.head.repo.full_name": head,
                "github.event.pull_request.head.repo.id": head_id,
                "github.event.pull_request.head.repo.fork": fork,
                "github.event.pull_request.number": number, "env.KLOGG_LABEL": label,
                "github.workspace": "/workspace", "steps.ccache-generation.outputs.current": "2026-10-14",
                "steps.restore-pr-ccache.outcome": "skipped",
                "steps.restore-pr-ccache.outputs.cache-matched-key": "", "success": success}

    PR_KEY = ("${{ env.KLOGG_LABEL }}-ccache-content-pr-v1-${{ github.event.pull_request.number }}-"
              "${{ steps.ccache-generation.outputs.current }}")
    PR_PREFIX = "${{ env.KLOGG_LABEL }}-ccache-content-pr-v1-${{ github.event.pull_request.number }}-"
    PR_RESTORE_PREFIXES = PR_PREFIX + "\n${{ env.KLOGG_LABEL }}-ccache-pr-v1-${{ github.event.pull_request.number }}-"
    SHARED_RESTORE_PREFIXES = "${{ env.KLOGG_LABEL }}-ccache-content-v1-\n${{ env.KLOGG_LABEL }}-ccache-v2-"

    def project(self, rows, context, own_hit=False, restore_success=True, cap_success=True):
        """Apply real step conditions and restore results in workflow order."""
        context = dict(context)
        restores, saves, bounded = [], [], False
        for fields, children in rows:
            if fields.get("uses") in ("./.github/actions/restore-compiler-object-artifact",
                                      "./.github/actions/export-compiler-object-artifact"):
                # Shared artifacts have their own admission/order contract tests.
                continue
            name = fields.get("name")
            options = children.get("with", {})
            if name != "Bound macOS ccache before save" and options.get("path") != self.PATH:
                continue
            enabled = context["success"] and bool(cache_expression(fields.get("if", "true"), context))
            if not enabled:
                continue
            if name == "Bound macOS ccache before save":
                bounded = cap_success
                context["success"] = cap_success
                continue
            key = cache_template(options["key"], context)
            if fields.get("uses", "").startswith("actions/cache/restore@"):
                prefixes = [cache_template(value, context) for value in options.get("restore-keys", "").splitlines()]
                restores.append((key, prefixes))
                if fields.get("id") == "restore-pr-ccache":
                    context["steps.restore-pr-ccache.outcome"] = "success" if restore_success else "failure"
                    context["success"] = restore_success
                    context["steps.restore-pr-ccache.outputs.cache-matched-key"] = key if own_hit and restore_success else ""
            elif fields.get("uses") == "./.github/actions/save-verified-compiler-cache":
                self.assertTrue(bounded, "compiler archive saved without successful cap step")
                saves.append(key)
            else:
                self.fail("unexpected compiler cache action")
        return restores, saves, bounded

    def assert_pr_policy(self, text):
        self.assertEqual(set(QUALITY.workflow_trigger_mapping(text)), {"push", "pull_request", "workflow_dispatch"})
        for job in self.MAC_JOBS:
            rows = self.rows(text, job)
            own_restore, options = self.named(rows, "Restore own-PR ccache for the macOS build")
            self.assertEqual(own_restore["id"], "restore-pr-ccache")
            self.assertEqual(own_restore["uses"], "actions/cache/restore@" + self.REVISION)
            self.assertEqual(options["with"], {"path": self.PATH, "key": self.PR_KEY, "restore-keys": self.PR_RESTORE_PREFIXES})
            own_save, options = self.named(rows, "Save own-PR ccache for the macOS build")
            self.assertEqual(own_save["uses"], "./.github/actions/save-verified-compiler-cache")
            self.assertEqual(options["with"], {"path": self.PATH, "key": self.PR_KEY,
                                               "descriptor": "${{ steps.shared-cache-descriptor.outputs.descriptor }}"})
            for fields in (own_restore, own_save):
                self.assertNotIn("continue-on-error", fields)
            names = [fields.get("name") for fields, _ in rows]
            self.assertLess(names.index(own_restore["name"]), names.index("Restore ccache for the macOS build"))
            self.assertLess(names.index("Enable ccache compiler launcher"), names.index("Bound macOS ccache before save"))
            self.assertLess(names.index("Bound macOS ccache before save"), names.index(own_save["name"]))
            tests = next(index for index, (fields, _) in enumerate(rows)
                         if fields.get("uses") == "./.github/actions/agent-run-tests")
            self.assertLess(tests, names.index("Bound macOS ccache before save"))
            for label in ("intel-qt6", "arm64-qt6", "intel-qt6-asan-ubsan", "intel-qt6-first-party-tsan"):
                for number in (1, 10, 123):
                    context = self.context("pull_request", label=label, number=number)
                    shared = label + "-ccache-content-v1-2026-10-14"
                    private_prefix = label + "-ccache-content-pr-v1-" + str(number) + "-"
                    private = private_prefix + "2026-10-14"
                    for hit in (False, True):
                        restores, saves, bounded = self.project(rows, context, own_hit=hit)
                        self.assertEqual(restores, [(private, [private_prefix, label + "-ccache-pr-v1-" + str(number) + "-"])]
                                         + ([] if hit else [(shared, [label + "-ccache-content-v1-", label + "-ccache-v2-"])]))
                        self.assertEqual(saves, [private])
                        self.assertTrue(bounded)
            for values, shared_write in (
                ({"event": "push"}, True), ({"event": "workflow_dispatch"}, False),
                ({"event": "schedule"}, False), ({"event": "pull_request_target"}, False),
                ({"event": "pull_request", "head": "fork/klogg", "fork": True}, False),
                ({"event": "pull_request", "head": "fork/klogg", "fork": False}, False),
                ({"event": "pull_request", "head": "", "fork": False}, False),
                ({"event": "pull_request", "repository": "other/klogg", "head": "other/klogg"}, False),
                ({"event": "pull_request", "head": "", "fork": None}, False),
                ({"event": "pull_request", "head": None, "fork": None}, False),
                ({"event": "pull_request", "head_id": None}, False),
                ({"event": "pull_request", "head_id": 0}, False),
                ({"event": "pull_request", "head_id": 1000}, False),
                ({"event": "pull_request", "head_id": "999"}, False),
                ({"event": "pull_request", "repository_id": ""}, False),
                ({"event": "pull_request", "repository": "fork/klogg"}, False),
                ({"event": "pull_request", "number": 0}, False),
            ):
                context = self.context(**values)
                restores, saves, bounded = self.project(rows, context, own_hit=True)
                self.assertEqual(restores, [("intel-qt6-ccache-content-v1-2026-10-14", ["intel-qt6-ccache-content-v1-", "intel-qt6-ccache-v2-"])], values)
                self.assertEqual(saves, ["intel-qt6-ccache-content-v1-2026-10-14"] if shared_write else [], values)
                self.assertEqual(bounded, shared_write, values)
            for event in ("push", "pull_request", "workflow_dispatch", "schedule"):
                self.assertEqual(self.project(rows, self.context(event, success=False)), ([], [], False))

    def test_same_canonical_repo_pr_restores_own_cache_before_base_and_saves_only_private_archive(self):
        self.assert_pr_policy(CI_BUILD.read_text())

    def test_upstream_forked_canonical_repo_pr_still_reuses_private_cache(self):
        # ZEACENT/klogg itself has fork=true; cross-repo identity is the boundary.
        for job in self.MAC_JOBS:
            restores, saves, bounded = self.project(self.rows(job=job), self.context("pull_request", fork=True))
            self.assertEqual(restores[0][0], "intel-qt6-ccache-content-pr-v1-123-2026-10-14")
            self.assertEqual(saves, ["intel-qt6-ccache-content-pr-v1-123-2026-10-14"])
            self.assertTrue(bounded)

    def test_failed_primary_restore_or_cap_cannot_publish_any_compiler_archive(self):
        for job in self.MAC_JOBS:
            rows = self.rows(job=job)
            for event in ("push", "pull_request"):
                self.assertEqual(self.project(rows, self.context(event, success=False)), ([], [], False))
                restores, saves, bounded = self.project(rows, self.context(event), cap_success=False)
                self.assertTrue(restores)
                self.assertEqual((saves, bounded), ([], False))
            restores, saves, bounded = self.project(rows, self.context("pull_request"), restore_success=False)
            self.assertEqual(restores, [("intel-qt6-ccache-content-pr-v1-123-2026-10-14", ["intel-qt6-ccache-content-pr-v1-123-", "intel-qt6-ccache-pr-v1-123-"])])
            self.assertEqual((saves, bounded), ([], False))

    def test_four_native_labels_keep_arch_qt_and_sanitizer_identity(self):
        text = CI_BUILD.read_text()
        self.assert_label_identity(text)
        block = "\n".join(QUALITY.workflow_job_blocks(text)["MacSanitizers"])
        for old, new in (("label: intel-qt6-asan-ubsan", "label: intel-qt6"),
                         ("label: intel-qt6-first-party-tsan", "label: intel-qt6-asan-ubsan"),
                         ("sanitizer: address-undefined", "sanitizer: thread")):
            self.assertIn(old, block)
            with self.subTest(mutation=new), self.assertRaises(AssertionError):
                self.assert_label_identity(text.replace(block, block.replace(old, new), 1))
        swapped = (block.replace("label: intel-qt6-asan-ubsan", "label: temporary-label")
                   .replace("label: intel-qt6-first-party-tsan", "label: intel-qt6-asan-ubsan")
                   .replace("label: temporary-label", "label: intel-qt6-first-party-tsan"))
        with self.assertRaises(AssertionError):
            self.assert_label_identity(text.replace(block, swapped, 1))

    def assert_label_identity(self, text):
        blocks = QUALITY.workflow_job_blocks(text)
        for job, arch, label in (("MacPackages", "x64", "intel-qt6"),
                                 ("MacArmPackages", "arm64", "arm64-qt6")):
            env = QUALITY.workflow_mapping_block(blocks[job], "env", 4)
            self.assertEqual(env["KLOGG_LABEL"][0], label)
            self.assertEqual(env["KLOGG_CONFIG_ARCH"][0], arch)
            self.assertEqual(env["KLOGG_QT_VERSION"][0], "6.9.3")
        matrix = QUALITY.workflow_job_matrix_values(blocks["MacSanitizers"])
        self.assertEqual(matrix["label"], {"intel-qt6-asan-ubsan", "intel-qt6-first-party-tsan"})
        self.assertEqual(matrix["arch"], {"x64"})
        self.assertEqual(matrix["qt_version"], {"6.9.3"})
        self.assertEqual(matrix["sanitizer"], {"address-undefined", "thread"})
        sanitizer_block = "\n".join(blocks["MacSanitizers"])
        for label, sanitizer, flags in (
            ("intel-qt6-asan-ubsan", "address-undefined", ("ADDRESS", "UNDEFINED_BEHAVIOR")),
            ("intel-qt6-first-party-tsan", "thread", ("THREAD",)),
        ):
            entry = re.search(r"label: " + label + r"\n(.*?)(?=\n\s*- os:|\n\s*runs-on:)",
                              sanitizer_block, re.S)
            self.assertIsNotNone(entry)
            self.assertRegex(entry[1], r"sanitizer: " + sanitizer + r"(?:\n|$)")
            for flag in flags:
                self.assertIn("-DENABLE_SANITIZER_" + flag + "=ON", entry[1])

    def test_real_cache_mutations_cannot_share_write_scope_or_reuse_wrong_label(self):
        text = CI_BUILD.read_text()
        block = "\n".join(QUALITY.workflow_job_blocks(text)["MacPackages"])
        mutations = (
            ("github.event.pull_request.head.repo.full_name == github.repository", "true"),
            ("toJSON(github.event.pull_request.head.repo.id) == github.repository_id", "true"),
            ("github.repository == 'ZEACENT/klogg'", "true"),
            ("-ccache-content-pr-v1-${{ github.event.pull_request.number }}-", "-ccache-content-v1-"),
            ("key: " + self.PR_KEY, "key: intel-qt6-ccache-content-pr-v1-${{ github.event.pull_request.number }}-${{ steps.ccache-generation.outputs.current }}"),
            (self.PR_PREFIX, "${{ env.KLOGG_LABEL }}-ccache-content-pr-v1-"),
            ("if: ${{ success() && github.event_name == 'push' }}",
             "if: ${{ success() && (github.event_name == 'push' || github.event_name == 'pull_request') }}"),
            ("steps.restore-pr-ccache.outcome == 'success'", "true"),
            ('python3 scripts/ci_ccache.py trim --label "$KLOGG_LABEL"', "ccache --max-size=250M"),
            ('python3 scripts/ci_ccache.py trim --label "$KLOGG_LABEL"', ""),
            ('python3 scripts/ci_ccache.py trim --label "$KLOGG_LABEL"', "# python3 scripts/ci_ccache.py trim"),
            ('python3 scripts/ci_ccache.py trim --label "$KLOGG_LABEL"', "printf '%s\\n' 'python3 scripts/ci_ccache.py trim'"),
        )
        for old, new in mutations:
            with self.subTest(mutation=new):
                self.assertIn(old, block)
                mutated = text.replace(block, block.replace(old, new), 1)
                with self.assertRaises(AssertionError):
                    self.assert_pr_policy(mutated)
                    self.assert_push_policy(mutated)

    def test_actual_workflow_projection_rejects_unknown_functions_and_malformed_guards(self):
        text = CI_BUILD.read_text()
        restore, _ = self.named(self.rows(text), "Restore own-PR ccache for the macOS build")
        for expression in ("${{ unknown(github.repository) }}", "${{ github.event_name == }}"):
            mutated = text.replace("if: " + restore["if"], "if: " + expression, 1)
            self.assertNotEqual(text, mutated)
            with self.subTest(expression=expression), self.assertRaises((AssertionError, SyntaxError)):
                self.assert_pr_policy(mutated)

    def test_push_baseline_retains_daily_source_independent_label_key_and_cap(self):
        self.assert_push_policy(CI_BUILD.read_text())

    def assert_push_policy(self, text):
        for job in self.MAC_JOBS:
            rows = self.rows(text, job)
            generation, _ = self.named(rows, "Compute bounded ccache generation")
            self.assertIn("datetime.datetime.now(datetime.timezone.utc).date()", generation["run"])
            restore, options = self.named(rows, "Restore ccache for the macOS build")
            self.assertEqual(restore["uses"], "actions/cache/restore@" + self.REVISION)
            self.assertEqual(options["with"], {"path": self.PATH, "key": self.SHARED_KEY,
                                               "restore-keys": self.SHARED_RESTORE_PREFIXES})
            bound, _ = self.named(rows, "Bound macOS ccache before save")
            self.assertEqual(bound["shell"], "sh")
            self.assertEqual(QUALITY.active_script_content(bound["run"]).strip(),
                             'python3 scripts/ci_ccache.py trim --label "$KLOGG_LABEL"')
            save, options = self.named(rows, "Save ccache for the macOS build")
            self.assertEqual(save["uses"], "./.github/actions/save-verified-compiler-cache")
            self.assertEqual(save["if"], "${{ success() && github.event_name == 'push' }}")
            self.assertEqual(options["with"], {"path": self.PATH, "key": self.SHARED_KEY,
                                               "descriptor": "${{ steps.shared-cache-descriptor.outputs.descriptor }}"})
            for step in (bound, save):
                self.assertNotIn("continue-on-error", step)
                for event, succeeds, enabled in (("push", True, True), ("push", False, False),
                                                 ("pull_request", False, False),
                                                 ("workflow_dispatch", True, False), ("schedule", True, False)):
                    self.assertEqual(bool(cache_expression(step["if"], self.context(event, succeeds))), enabled)
            launcher, _ = self.named(rows, "Enable ccache compiler launcher")
            self.assertIn("-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache", launcher["run"])
            self.assertIn("CCACHE_BASEDIR=${{ github.workspace }}", launcher["run"])


if __name__ == "__main__":
    unittest.main()
