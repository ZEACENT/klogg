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
        self.assertEqual(len(MODULE.coverage_workflow_issues(insecure)), 4)

        secure = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --json -o coverage_report/coverage.json
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
        self.assertEqual(len(MODULE.coverage_workflow_issues(workflow)), 4)

    def test_inline_shell_comments_cannot_spoof_coverage_policy(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg_tests klogg_itests : # klogg klogg_grep
      true # --filter '^src/'
      true # --filter '^src/'
"""
        self.assertEqual(len(MODULE.coverage_workflow_issues(workflow)), 4)

    def test_coverage_noop_tokens_cannot_spoof_report_policy(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
      echo "--filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file"
      echo "--filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file"
      gcovr --filter '^src/' --html
      gcovr --filter '^src/' --json
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn(
            "every coverage report must narrowly handle GCC's negative branch-hit bug",
            issues,
        )

    def test_every_gcovr_command_requires_the_narrow_workaround(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file
      gcovr --filter '^src/' --json
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn(
            "every coverage report must narrowly handle GCC's negative branch-hit bug",
            issues,
        )

    def test_coverage_reports_must_fail_closed(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html || echo ignored
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --json -o coverage_report/coverage.json || :
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn("coverage report generation must fail closed", issues)

    def test_coverage_requires_one_html_and_one_json_report(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index-2.html
"""
        self.assertIn(
            "coverage must produce the authoritative HTML summary and JSON report",
            MODULE.coverage_workflow_issues(workflow),
        )

    def test_coverage_targets_cannot_be_spoofed_after_shell_operators(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg_tests klogg_itests && printf '%s' klogg klogg_grep
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --json -o coverage_report/coverage.json
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn("coverage build must include target klogg", issues)
        self.assertIn("coverage build must include target klogg_grep", issues)

    def test_coverage_options_cannot_be_spoofed_inside_another_argument(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
      gcovr --html-title "--filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file" --html-details --print-summary -o coverage_report/index.html
      gcovr --html-title "--filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file" --json -o coverage_report/coverage.json
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn(
            "gcovr must include root-relative src/ paths in every report pass",
            issues,
        )
        self.assertIn(
            "every coverage report must narrowly handle GCC's negative branch-hit bug",
            issues,
        )

    def test_coverage_and_lists_cannot_mask_report_failure(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html && :
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --json -o coverage_report/coverage.json
"""
        self.assertIn(
            "coverage report generation must fail closed",
            MODULE.coverage_workflow_issues(workflow),
        )

    def test_static_analysis_uses_only_configured_first_party_units(self):
        insecure = """\
steps:
  - run: |
      find src -type f -name '*.cpp' > tidy_files.txt
      cmake -DKLOGG_USE_SENTRY=OFF
"""
        issues = MODULE.static_analysis_workflow_issues(insecure)
        self.assertIn(
            "clang-tidy TU scope must come from the configured compile database",
            issues,
        )
        self.assertIn(
            "static analysis must configure optional Sentry production code",
            issues,
        )

        secure = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1" || exit 0' _ < tidy_files.nul || true
"""
        self.assertEqual(MODULE.static_analysis_workflow_issues(secure), [])

    def test_static_analysis_requires_nul_consumers_for_the_tu_list(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -n 1 clang-tidy < tidy_files.nul
"""
        issues = MODULE.static_analysis_workflow_issues(workflow)
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            issues,
        )

    def test_static_analysis_noop_tokens_and_overrides_are_rejected(self):
        workflow = """\
steps:
  - run: |
      echo "python3 scripts/first_party_compile_units.py db src > tidy_files.txt"
      find src -type f -name '*.cpp' > tidy_files.txt
      cmake -S . -B build -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_SENTRY=OFF
"""
        issues = MODULE.static_analysis_workflow_issues(workflow)
        self.assertIn(
            "clang-tidy TU scope must come from the configured compile database",
            issues,
        )
        self.assertIn(
            "static analysis must configure optional Sentry production code",
            issues,
        )

    def test_static_analysis_rejects_wrong_scope_and_fake_consumers(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON
      python3 scripts/first_party_compile_units.py wrong.json src/ui --null > tidy_files.nul
      xargs -0 -n 1 echo clang-tidy < tidy_files.nul
      xargs -0 -n 1 bash -c 'echo clang-tidy "$1" || exit 0' _ < tidy_files.nul || true
"""
        issues = MODULE.static_analysis_workflow_issues(workflow)
        self.assertIn(
            "clang-tidy TU scope must come from the configured compile database",
            issues,
        )
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            issues,
        )

    def test_static_analysis_requires_the_strict_pr_consumer(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1" || exit 0' _ < tidy_files.nul || true
"""
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_strict_consumer_must_propagate_exit_status(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"; exit 0' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1" || exit 0' _ < tidy_files.nul || true
"""
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_rejects_later_sentry_reconfigure_override(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON
      cmake -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=OFF
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1" || exit 0' _ < tidy_files.nul || true
"""
        self.assertIn(
            "static analysis must configure optional Sentry production code",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_rejects_all_false_sentry_spellings(self):
        for false_value in ("OFF", "FALSE", "NO", "0"):
            with self.subTest(false_value=false_value):
                workflow = f"""\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_SENTRY={false_value}
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1" || exit 0' _ < tidy_files.nul || true
"""
                self.assertIn(
                    "static analysis must configure optional Sentry production code",
                    MODULE.static_analysis_workflow_issues(workflow),
                )

    def test_static_analysis_uses_a_sentry_specific_cpm_cache_key(self):
        workflow = """\
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1" || exit 0' _ < tidy_files.nul || true
"""
        self.assertIn(
            "static analysis must use a Sentry-specific CPM cache key",
            MODULE.static_analysis_workflow_issues(workflow),
        )

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
