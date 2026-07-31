import importlib.util
import pathlib
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).parents[2] / "scripts" / "lint_ci_quality.py"
SPEC = importlib.util.spec_from_file_location("lint_ci_quality", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


PINNED = "11d5960a326750d5838078e36cf38b85af677262"


class CiQualityLintTest(unittest.TestCase):
    def test_checkout_requires_sha_and_disabled_credentials(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n  - uses: actions/checkout@v4\n  - run: true\n",
        )
        self.assertEqual(len(issues), 2)

    def test_secure_checkout_is_accepted(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            f"  - uses: actions/checkout@{PINNED} # reviewed version\n"
            "    with:\n"
            "      persist-credentials: false\n",
        )
        self.assertEqual(issues, [])

    def test_comments_cannot_spoof_checkout_security(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            f"  - uses: actions/checkout@v4 # actions/checkout@{PINNED}\n"
            "    with:\n"
            "      # persist-credentials: false\n",
        )
        self.assertEqual(len(issues), 2)

    def test_block_scalar_checkout_is_still_validated(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            "  - uses: >-\n"
            "      actions/checkout@v4\n"
            "    with:\n"
            "      persist-credentials: false\n",
        )
        self.assertEqual(len(issues), 1)
        self.assertIn("40-char SHA", issues[0])

    def test_block_scalar_cannot_spoof_checkout_credentials(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            f"  - uses: actions/checkout@{PINNED}\n"
            "    with:\n"
            "      sparse-checkout: |\n"
            "        persist-credentials: false\n",
        )
        self.assertEqual(len(issues), 1)
        self.assertIn("persist-credentials", issues[0])

    def test_bare_dash_checkout_step_is_parsed(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            "  -\n"
            "    uses: actions/checkout@v4\n"
            "    with:\n"
            "      persist-credentials: false\n",
        )
        self.assertEqual(len(issues), 1)
        self.assertIn("40-char SHA", issues[0])

    def test_named_checkout_step_is_parsed(self):
        path = pathlib.Path(".github/workflows/test.yml")
        issues = MODULE.check_checkout_blocks(
            path,
            "steps:\n"
            "  - name: Checkout\n"
            f"    uses: actions/checkout@{PINNED}\n"
            "    with:\n"
            "      persist-credentials: false\n",
        )
        self.assertEqual(issues, [])

    def test_scans_yaml_workflows_and_composite_actions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            workflow = root / ".github/workflows/test.yaml"
            action_yml = root / ".github/actions/one/action.yml"
            action_yaml = root / ".github/actions/two/action.yaml"
            for path in (workflow, action_yml, action_yaml):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("steps:\n  - uses: actions/checkout@v4\n")
            self.assertEqual(
                {path.relative_to(root) for path in MODULE.ci_manifests(root)},
                {
                    pathlib.Path(".github/workflows/test.yaml"),
                    pathlib.Path(".github/actions/one/action.yml"),
                    pathlib.Path(".github/actions/two/action.yaml"),
                },
            )

    def test_broad_sanitizer_patterns_are_rejected(self):
        self.assertIsNotNone(MODULE.BROAD_SUPPRESSION_RE.search("race:*Qt*\n"))
        self.assertIsNotNone(MODULE.BROAD_SUPPRESSION_RE.search("leak:*mimalloc*\n"))
        self.assertIsNone(MODULE.BROAD_SUPPRESSION_RE.search("leak:qoffscreen\n"))

    def test_macos_lsan_rule_ignores_explanatory_comments(self):
        self.assertFalse(
            MODULE.has_unsupported_macos_lsan(
                "# detect_leaks=1 is unsupported on Apple ASan\n"
                'echo "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1"\n'
            )
        )
        self.assertTrue(
            MODULE.has_unsupported_macos_lsan(
                'echo "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1"\n'
            )
        )

    def test_coverage_floor_rejects_non_finite_values(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "floor.txt"
            for value in ("NaN", "inf", "-inf", "0", "101"):
                with self.subTest(value=value):
                    path.write_text(value)
                    with self.assertRaises(ValueError):
                        MODULE.coverage_floor(path)

    def test_coverage_workflow_requires_root_sources_and_production_targets(self):
        insecure = """\
steps:
  - run: |
      cmake --build build_root -t klogg_tests klogg_itests
      --filter '.*/src/.*'
      --filter '.*/src/.*'
"""
        self.assertEqual(len(MODULE.coverage_workflow_issues(insecure)), 3)

        secure = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
      --filter '^src/'
      --filter '^src/'
"""
        self.assertEqual(MODULE.coverage_workflow_issues(secure), [])

    def test_coverage_comments_cannot_spoof_scope_or_targets(self):
        workflow = """\
steps:
  # cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
  - run: |
      # --filter '^src/'
      # --filter '^src/'
      cmake --build build_root -t klogg_tests klogg_itests
"""
        self.assertEqual(len(MODULE.coverage_workflow_issues(workflow)), 3)

    def test_inline_shell_comments_cannot_spoof_coverage_policy(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg_tests klogg_itests : # klogg klogg_grep
      true # --filter '^src/'
      true # --filter '^src/'
"""
        self.assertEqual(len(MODULE.coverage_workflow_issues(workflow)), 3)

    def test_coverage_reports_must_fail_closed(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
      gcovr --filter '^src/' --gcov-ignore-parse-errors
      gcovr --filter '^src/' --json || true
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn("authoritative coverage reports must fail on malformed gcov data", issues)
        self.assertIn("coverage JSON generation must fail closed", issues)

    def test_cppcheck_baselines_require_path_and_line(self):
        self.assertEqual(
            len(MODULE.cppcheck_suppression_issues("unreadVariable\n")), 1
        )
        self.assertEqual(
            MODULE.cppcheck_suppression_issues(
                "unreadVariable:*/src/example.cpp:42\n"
            ),
            [],
        )


if __name__ == "__main__":
    unittest.main()
