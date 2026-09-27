"""docker-build compile-database and metrics wiring; no real container runs."""
import importlib.util
import pathlib
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
        self.assertIn("ci-build-metrics.json", body)
        self.assertNotIn("klogg_codeql_thirdparty", body)
        self.assertNotIn("rm ", body)

    def test_tsan_reports_cache_disabled_and_others_measure_global_counters(self):
        body = self.bodies["build"]
        self.assertIn('thread) KLOGG_OBJECT_CACHE=disabled', body)
        self.assertIn('KLOGG_OBJECT_CACHE=measured', body)

    def test_metrics_output_stays_in_the_build_root(self):
        body = self.bodies["build"]
        self.assertIn("--output /usr/local/$KLOGG_BUILD_ROOT/ci-build-metrics.json", body)


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
        self.assertIn("scripts/ci_build_metrics.py run-build", body)
        self.assertEqual(body.count("cmake --build"), 1)
        self.assertIn('Windows) object_cache=disabled', body)
        self.assertIn('object_cache=measured', body)
        self.assertIn('--repo-root "$KLOGG_WORKSPACE"', body)
        self.assertIn('--build-root "$KLOGG_WORKSPACE/$KLOGG_BUILD_ROOT"', body)
        self.assertIn('--output "$KLOGG_WORKSPACE/$KLOGG_BUILD_ROOT/ci-build-metrics.json"', body)
        self.assertIn('--object-cache "$object_cache"', body)
        self.assertIn('-- cmake --build "$KLOGG_BUILD_ROOT" -t ci_build', body)


class BuildMetricsUploadWorkflowTest(unittest.TestCase):
    def test_every_native_and_linux_build_uploads_its_small_report_before_tests(self):
        steps = QUALITY.workflow_job_steps(CI_BUILD.read_text())
        for job in QUALITY.CI_BUILD_PACKAGE_ENABLEMENT:
            with self.subTest(job=job):
                parsed = [QUALITY.workflow_step_fields(step) for step in steps[job]]
                builds = [index for index, (fields, _) in enumerate(parsed)
                          if fields.get("uses") in ("./.github/actions/docker-build",
                                                    "./.github/actions/agent-build")]
                metrics = [(index, fields, children.get("with")) for index, (fields, children)
                           in enumerate(parsed) if fields.get("name") == "Upload current-run build metrics"]
                self.assertEqual(len(builds), 1)
                self.assertEqual(len(metrics), 1)
                index, fields, options = metrics[0]
                self.assertEqual(index, builds[0] + 1)
                self.assertEqual(fields.get("uses"), "actions/upload-artifact@" +
                                 QUALITY.REVIEWED_ACTION_REVISIONS["actions/upload-artifact"])
                self.assertNotIn("if", fields)
                self.assertEqual(options, {
                    "name": "build-metrics-${{ env.KLOGG_LABEL }}-${{ env.KLOGG_CONFIG_PACKAGE_TAG }}",
                    "path": "${{ env.KLOGG_BUILD_ROOT }}/ci-build-metrics.json",
                    "if-no-files-found": "error", "retention-days": "7", "compression-level": "0",
                })


if __name__ == "__main__":
    unittest.main()
