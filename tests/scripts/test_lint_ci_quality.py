import importlib.util
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "lint_ci_quality.py"
SPEC = importlib.util.spec_from_file_location("lint_ci_quality", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


PINNED = "11d5960a326750d5838078e36cf38b85af677262"
CODEQL_PINNED = "4187e74d05793876e9989daffde9c3e66b4acd07"


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

    def test_codeql_requires_pinned_matching_actions_and_timeout(self):
        insecure = """\
jobs:
  analyze:
    runs-on: ubuntu-24.04
    steps:
      - uses: github/codeql-action/init@v3
      - uses: github/codeql-action/analyze@v3
"""
        issues = MODULE.codeql_workflow_issues(insecure)
        self.assertTrue(any("init must use" in issue for issue in issues))
        self.assertTrue(any("analyze must use" in issue for issue in issues))
        self.assertTrue(any("timeout-minutes" in issue for issue in issues))

    def test_codeql_rejects_mismatched_action_revisions(self):
        text = f"""\
jobs:
  analyze:
    timeout-minutes: 30
    steps:
      - uses: github/codeql-action/init@{CODEQL_PINNED}
      - uses: github/codeql-action/analyze@{'a' * 40}
"""
        issues = MODULE.codeql_workflow_issues(text)
        self.assertEqual(len(issues), 1)
        self.assertIn("same reviewed SHA", issues[0])

    def test_codeql_rejects_continue_on_error(self):
        text = f"""\
jobs:
  analyze:
    timeout-minutes: 30
    continue-on-error: true
    steps:
      - uses: github/codeql-action/init@{CODEQL_PINNED}
      - uses: github/codeql-action/analyze@{CODEQL_PINNED}
"""
        issues = MODULE.codeql_workflow_issues(text)
        self.assertEqual(len(issues), 1)
        self.assertIn("continue-on-error", issues[0])

    def test_secure_codeql_workflow_is_accepted(self):
        text = f"""\
jobs:
  analyze:
    timeout-minutes: 30
    steps:
      - uses: github/codeql-action/init@{CODEQL_PINNED}
      - uses: github/codeql-action/analyze@{CODEQL_PINNED}
"""
        self.assertEqual(MODULE.codeql_workflow_issues(text), [])

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
        self.assertEqual(len(MODULE.coverage_workflow_issues(insecure)), 5)

        secure = """\
env:
  EVENT_NAME: ${{ github.event_name }}
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --json -o coverage_report/coverage.json
      git fetch --no-tags origin "$base_sha"
      git cat-file -e "$base_sha^{commit}"
      echo "coverage ratchet base commit"
"""
        self.assertEqual(MODULE.coverage_workflow_issues(secure), [])

    def test_coverage_ratchet_requires_the_authoritative_event_base(self):
        workflow = (
            ROOT / ".github" / "workflows" / "coverage.yml"
        ).read_text()
        self.assertNotIn(
            "coverage ratchet must fail closed on the event base commit",
            MODULE.coverage_workflow_issues(workflow),
        )

        insecure = workflow.replace(
            'git fetch --no-tags origin "$base_sha"',
            'base_sha=$(git rev-parse HEAD^ 2>/dev/null || true)',
            1,
        )
        self.assertIn(
            "coverage ratchet must fail closed on the event base commit",
            MODULE.coverage_workflow_issues(insecure),
        )

    def test_coverage_comments_cannot_spoof_scope_or_targets(self):
        workflow = """\
steps:
  # cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
  - run: |
      # --filter '^src/'
      # --filter '^src/'
      cmake --build build_root -t klogg_tests klogg_itests
"""
        self.assertEqual(len(MODULE.coverage_workflow_issues(workflow)), 5)

    def test_inline_shell_comments_cannot_spoof_coverage_policy(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg_tests klogg_itests : # klogg klogg_grep
      true # --filter '^src/'
      true # --filter '^src/'
"""
        self.assertEqual(len(MODULE.coverage_workflow_issues(workflow)), 5)

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

    def test_coverage_rejects_duplicate_scope_and_output_options(self):
        workflow = """\
steps:
  - run: |
      cmake --build build_root -t klogg klogg_grep klogg_tests klogg_itests
      gcovr --filter '^src/' --filter '.*' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --html-details --print-summary -o coverage_report/index.html -o /tmp/redirected.html
      gcovr --filter '^src/' --gcov-ignore-parse-errors negative_hits.warn_once_per_file --json -o coverage_report/coverage.json
"""
        issues = MODULE.coverage_workflow_issues(workflow)
        self.assertIn(
            "gcovr must include root-relative src/ paths in every report pass",
            issues,
        )
        self.assertIn(
            "coverage must produce the authoritative HTML summary and JSON report",
            issues,
        )

    def test_clang_tidy_diff_find_fallback_preserves_explicit_error(self):
        workflow = (ROOT / ".github" / "workflows" / "static-analysis.yml").read_text()
        self.assertIn(
            "find /usr/lib/llvm-*/share/clang -name clang-tidy-diff.py -print -quit 2>/dev/null || true",
            workflow,
        )

    def test_static_analysis_installs_optional_dependency_build_tools(self):
        workflow = (ROOT / ".github" / "workflows" / "static-analysis.yml").read_text()
        self.assertIn("libcurl4-openssl-dev", workflow)
        self.assertIn("ragel", workflow)

    def test_static_analysis_uses_reusable_changed_line_runner(self):
        workflow = (ROOT / ".github" / "workflows" / "static-analysis.yml").read_text()
        self.assertIn("python3 scripts/run_changed_clang_tidy.py", workflow)
        self.assertIn("--base '${{ github.event.pull_request.base.sha }}'", workflow)
        self.assertIn('--build-dir "$KLOGG_BUILD_ROOT"', workflow)
        self.assertIn('--clang-tidy-diff "$CLANG_TIDY_DIFF"', workflow)

    def test_static_analysis_rejects_masked_report_only_tool_failures(self):
        workflow = (
            ROOT / ".github" / "workflows" / "static-analysis.yml"
        ).read_text()
        mutated = workflow.replace(
            "' _ < tidy_files.nul\n",
            "' _ < tidy_files.nul || true\n",
            1,
        )
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(mutated),
        )

    def test_static_analysis_rejects_duplicate_runner_options(self):
        workflow = (
            ROOT / ".github" / "workflows" / "static-analysis.yml"
        ).read_text()
        mutated = workflow.replace(
            "--jobs \"$(nproc)\"",
            "--jobs \"$(nproc)\" --base HEAD",
            1,
        )
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(mutated),
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
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
        self.assertEqual(MODULE.static_analysis_workflow_issues(secure), [])

    def test_static_analysis_configures_vectorscan_production_code(self):
        workflow = (
            ROOT / ".github" / "workflows" / "static-analysis.yml"
        ).read_text()
        self.assertIn("-DKLOGG_USE_VECTORSCAN=ON", workflow)
        self.assertNotIn("-DKLOGG_USE_VECTORSCAN=OFF", workflow)

        insecure = workflow.replace(
            "-DKLOGG_USE_VECTORSCAN=ON",
            "-DKLOGG_USE_VECTORSCAN=OFF",
            1,
        )
        self.assertIn(
            "static analysis must configure optional Vectorscan production code",
            MODULE.static_analysis_workflow_issues(insecure),
        )

    def test_static_analysis_sentry_flag_must_belong_to_the_real_configure(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT"
      cmake -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON -P verify_sentry.cmake
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
        self.assertIn(
            "static analysis must configure optional Sentry production code",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_requires_nul_consumers_for_the_tu_list(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
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
      cmake -S . -B build -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON -DKLOGG_USE_SENTRY=OFF
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
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
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

    def test_static_analysis_consumers_must_forward_each_tu_to_clang_tidy(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" src/benign.cpp' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" src/benign.cpp || exit 0' _ < tidy_files.nul || true
"""
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_consumers_reject_xargs_batch_and_fixed_arguments(self):
        consumers = (
            (
                "xargs -0 -n 2 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" --line-filter=\"$TIDY_LINE_FILTER\" \"$1\"' _ < tidy_files.nul",
                "xargs -0 -n 2 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" \"$1\" || exit 0' _ < tidy_files.nul || true",
            ),
            (
                "xargs -0 -n 1 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" --line-filter=\"$TIDY_LINE_FILTER\" \"$1\"' _ src/benign.cpp < tidy_files.nul",
                "xargs -0 -n 1 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" \"$1\" || exit 0' _ src/benign.cpp < tidy_files.nul || true",
            ),
            (
                "xargs -0 -n 1 true bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" --line-filter=\"$TIDY_LINE_FILTER\" \"$1\"' _ < tidy_files.nul",
                "xargs -0 -n 1 true bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" \"$1\" || exit 0' _ < tidy_files.nul || true",
            ),
            (
                "xargs -0 -n 1 -a benign.nul bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" --line-filter=\"$TIDY_LINE_FILTER\" \"$1\"' _ < tidy_files.nul",
                "xargs -0 -n 1 -a benign.nul bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" \"$1\" || exit 0' _ < tidy_files.nul || true",
            ),
            (
                "xargs -0 -n 1 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" --line-filter=\"$TIDY_LINE_FILTER\" \"$1\"' _ < tidy_files.nul < benign.nul",
                "xargs -0 -n 1 bash -c 'clang-tidy -p \"$KLOGG_BUILD_ROOT\" \"$1\" || exit 0' _ < tidy_files.nul < benign.nul || true",
            ),
        )
        for strict_consumer, report_consumer in consumers:
            with self.subTest(strict_consumer=strict_consumer):
                workflow = f"""\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      {strict_consumer}
      {report_consumer}
"""
                self.assertIn(
                    "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
                    MODULE.static_analysis_workflow_issues(workflow),
                )

    def test_static_analysis_requires_the_strict_pr_consumer(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_strict_consumer_must_propagate_exit_status(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"; exit 0' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
        self.assertIn(
            "clang-tidy TU scope must use NUL-delimited strict and report-only consumers",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_rejects_later_sentry_reconfigure_override(self):
        workflow = """\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      cmake -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=OFF
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
        self.assertIn(
            "static analysis must configure optional Sentry production code",
            MODULE.static_analysis_workflow_issues(workflow),
        )

    def test_static_analysis_rejects_typed_and_unset_sentry_reconfiguration(self):
        reconfigures = (
            'cmake -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY:BOOL=OFF',
            'cmake -B "$KLOGG_BUILD_ROOT" -U KLOGG_USE_SENTRY',
            'cmake -B "$KLOGG_BUILD_ROOT" -UKLOGG_USE_SENTRY',
            'cmake -B"$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY:BOOL=OFF',
            'cmake -B"$KLOGG_BUILD_ROOT" -UKLOGG_USE_SENTRY',
            'cmake -B="$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY:BOOL=OFF',
            'cmake -B="$KLOGG_BUILD_ROOT" -UKLOGG_USE_SENTRY',
            'cmake -B "$KLOGG_BUILD_ROOT"',
            'cmake -B "$KLOGG_BUILD_ROOT" --fresh',
        )
        for reconfigure in reconfigures:
            with self.subTest(reconfigure=reconfigure):
                workflow = f"""\
env:
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      {reconfigure}
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
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
  KLOGG_CPM_CACHE_KEY_SUFFIX: -sentry-vectorscan
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON -DKLOGG_USE_SENTRY={false_value}
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
                self.assertIn(
                    "static analysis must configure optional Sentry production code",
                    MODULE.static_analysis_workflow_issues(workflow),
                )

    def test_static_analysis_uses_a_sentry_specific_cpm_cache_key(self):
        workflow = """\
steps:
  - run: |
      cmake -S "$KLOGG_WORKSPACE" -B "$KLOGG_BUILD_ROOT" -DKLOGG_USE_SENTRY=ON -DKLOGG_USE_VECTORSCAN=ON
      python3 scripts/first_party_compile_units.py "$KLOGG_BUILD_ROOT/compile_commands.json" "$KLOGG_WORKSPACE/src" --null > tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" --line-filter="$TIDY_LINE_FILTER" "$1"' _ < tidy_files.nul
      xargs -0 -n 1 bash -c 'clang-tidy -p "$KLOGG_BUILD_ROOT" "$1"' _ < tidy_files.nul
"""
        self.assertIn(
            "static analysis must use a Sentry-and-Vectorscan-specific CPM cache key",
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
