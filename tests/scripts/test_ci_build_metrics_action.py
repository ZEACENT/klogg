"""docker-build compile-database and metrics wiring; no real container runs."""
import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
ACTION = ROOT / ".github/actions/docker-build/action.yml"
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


if __name__ == "__main__":
    unittest.main()
