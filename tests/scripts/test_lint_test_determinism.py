from __future__ import annotations

import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import lint_test_determinism as lint


def scan(text, name="tests/unit/example_test.cpp"):
    return lint.check_text(text, Path(name))


class QTestSleepRuleTest(unittest.TestCase):
    def test_qsleep_is_flagged(self):
        findings = scan('QTest::qSleep( 100 );')
        self.assertEqual([f.rule for f in findings], ["qsleep-in-tests"])

    def test_qsleep_marker_does_not_help(self):
        findings = scan('QTest::qSleep( 100 ); // lint-allow: test-timing')
        self.assertEqual([f.rule for f in findings], ["qsleep-in-tests"])

    def test_qwait_is_not_qsleep(self):
        findings = scan('QTest::qWait( 100 );')
        self.assertEqual(findings, [])


class LongQwaitRuleTest(unittest.TestCase):
    def test_qwait_above_threshold_is_flagged(self):
        findings = scan('QTest::qWait( 2000 );')
        self.assertEqual([f.rule for f in findings], ["long-qwait"])

    def test_qwait_at_threshold_is_allowed(self):
        self.assertEqual(scan('QTest::qWait( 500 );'), [])
        self.assertEqual(scan('QTest::qWait( 20 );'), [])

    def test_marked_long_qwait_is_allowed(self):
        self.assertEqual(
            scan('QTest::qWait( 1200 ); // lint-allow: test-timing'), []
        )

    def test_legacy_platform_fragile_marker_is_accepted(self):
        self.assertEqual(
            scan('QTest::qWait( 1200 ); // lint-allow: platform-fragile'), []
        )

    def test_non_literal_qwait_is_not_flagged(self):
        self.assertEqual(scan('QTest::qWait( pollMs );'), [])

    def test_qwait_inside_comment_is_not_flagged(self):
        self.assertEqual(scan('// QTest::qWait( 2000 );'), [])
        self.assertEqual(scan('/* QTest::qWait( 2000 ); */'), [])

    def test_qwait_inside_string_literal_is_not_flagged(self):
        self.assertEqual(scan('const char* s = "QTest::qWait( 2000 )";'), [])


class ThreadSleepRuleTest(unittest.TestCase):
    def test_thread_sleeps_are_flagged(self):
        for snippet in (
            'QThread::sleep( 2 );',
            'QThread::msleep( 200 );',
            'QThread::usleep( 50 );',
            'std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );',
        ):
            findings = scan(snippet)
            self.assertEqual(
                [f.rule for f in findings], ["thread-sleep-in-tests"], snippet
            )

    def test_marked_thread_sleep_is_allowed(self):
        self.assertEqual(
            scan('QThread::msleep( 250 ); // lint-allow: test-timing'), []
        )

    def test_sleep_lookalikes_are_allowed(self):
        self.assertEqual(scan('worker.msleep( 5 );'), [])
        self.assertEqual(scan('harness.sleep_for( 1 );'), [])


class SpinDrainRuleTest(unittest.TestCase):
    def test_iteration_bounded_drain_is_flagged(self):
        for snippet in (
            'for ( int iteration = 0; iteration < 10000 && !predicate(); ++iteration ) {',
            'for ( int attempt = 0; attempt < 10000 && !ready(); ++attempt ) {',
            'for ( int iteration = 0; !predicate() && iteration < 10000; ++iteration ) {',
        ):
            findings = scan(snippet)
            self.assertEqual([f.rule for f in findings], ["spin-drain-loop"], snippet)

    def test_marked_spin_drain_is_allowed(self):
        self.assertEqual(
            scan(
                'for ( int i = 0; i < 10000 && !pred(); ++i ) { // lint-allow: test-timing'
            ),
            [],
        )

    def test_time_bounded_drain_is_allowed(self):
        text = (
            'QElapsedTimer guard;\n'
            'guard.start();\n'
            'while ( !predicate() && guard.elapsed() < DrainTimeoutMs ) {\n'
            '    QCoreApplication::processEvents( QEventLoop::AllEvents, 1 );\n'
            '}\n'
        )
        self.assertEqual(scan(text), [])

    def test_small_iteration_caps_are_allowed(self):
        self.assertEqual(
            scan('for ( int i = 0; i < 100 && !pred(); ++i ) {'), []
        )


class DoubleCheckedPredicateRuleTest(unittest.TestCase):
    def test_trailing_predicate_recheck_is_flagged(self):
        text = (
            'while ( !predicate() && timer.elapsed() < timeoutMs ) {\n'
            '    QTest::qWait( 10 );\n'
            '}\n'
            'return predicate();\n'
        )
        findings = scan(text)
        self.assertEqual(
            [(f.rule, f.line) for f in findings], [("double-checked-predicate", 4)]
        )

    def test_single_evaluation_is_allowed(self):
        text = (
            'while ( timer.elapsed() < timeoutMs ) {\n'
            '    if ( predicate() ) {\n'
            '        return true;\n'
            '    }\n'
            '    QTest::qWait( 10 );\n'
            '}\n'
            'return predicate();\n'
        )
        self.assertEqual(scan(text), [])

    def test_marked_trailing_recheck_is_allowed(self):
        text = (
            'while ( !predicate() && timer.elapsed() < timeoutMs ) {\n'
            '    QTest::qWait( 10 );\n'
            '}\n'
            'return predicate(); // lint-allow: test-timing\n'
        )
        self.assertEqual(scan(text), [])

    def test_different_predicate_name_is_not_flagged(self):
        text = (
            'while ( !ready() && timer.elapsed() < timeoutMs ) {\n'
            '    pump();\n'
            '}\n'
            'return done();\n'
        )
        self.assertEqual(scan(text), [])


class PerfBudgetRuleTest(unittest.TestCase):
    CASE = 'TEST_CASE( "fast path", "[.perf]" )\n{\n%s\n}\n'

    def test_elapsed_budget_assertion_is_flagged(self):
        findings = scan(self.CASE % 'CHECK( elapsedMs < 200 );')
        self.assertIn("perf-budget-assertion", [f.rule for f in findings])

    def test_marked_budget_in_tagged_case_is_allowed(self):
        findings = scan(
            self.CASE % 'CHECK( elapsedMs < 200 ); // lint-allow: perf-budget -- Local speed budget.'
        )
        self.assertEqual(findings, [])

    def test_budget_outside_perf_tagged_case_is_flagged(self):
        text = (
            'TEST_CASE( "fast path", "[capture]" )\n{\n'
            'CHECK( elapsedMs < 200 ); // lint-allow: perf-budget -- Local speed budget.\n}\n'
        )
        findings = scan(text)
        self.assertEqual([f.rule for f in findings], ["perf-budget-needs-perf-tag"])

    def test_deadline_loop_condition_is_not_a_budget(self):
        text = (
            'while ( !predicate() && deadline.elapsed() < MaxWaitMs ) {\n'
            '    pump();\n'
            '}\n'
        )
        self.assertEqual(scan(text), [])

    def test_chrono_budget_assertion_is_flagged(self):
        findings = scan(self.CASE % 'CHECK( shutdownElapsed < std::chrono::milliseconds( 500 ) );')
        self.assertIn("perf-budget-assertion", [f.rule for f in findings])

    def test_timer_call_budget_assertion_is_flagged(self):
        findings = scan(self.CASE % 'CHECK( timer.elapsed() < 100 );')
        self.assertIn("perf-budget-assertion", [f.rule for f in findings])

    def test_chrono_now_diff_budget_assertion_is_flagged(self):
        findings = scan(
            self.CASE
            % 'CHECK( std::chrono::steady_clock::now() - started < limit );'
        )
        self.assertIn("perf-budget-assertion", [f.rule for f in findings])

    def test_chrono_now_diff_lower_bound_is_not_a_budget(self):
        findings = scan(
            self.CASE
            % 'CHECK( std::chrono::steady_clock::now() - started >= minimum );'
        )
        self.assertEqual(findings, [])

    def test_chrono_cast_diff_budget_assertion_is_flagged(self):
        findings = scan(
            self.CASE
            % 'CHECK( std::chrono::duration_cast<std::chrono::milliseconds>('
              ' timer.now() - started ).count() < 200 );'
        )
        self.assertIn("perf-budget-assertion", [f.rule for f in findings])

    def test_chrono_cast_diff_lower_bound_is_not_a_budget(self):
        findings = scan(
            self.CASE
            % 'CHECK( std::chrono::duration_cast<std::chrono::milliseconds>('
              ' timer.now() - started ).count() >= 1 );'
        )
        self.assertEqual(findings, [])

    def test_line_wrapped_budget_assertion_is_flagged(self):
        text = (
            'TEST_CASE( "fast path", "[.perf]" )\n{\n'
            'REQUIRE(\n'
            '    bestElapsedMs\n'
            '    < 200 );\n'
            '}\n'
        )
        findings = scan(text)
        self.assertIn("perf-budget-assertion", [f.rule for f in findings])

    def test_line_wrapped_budget_with_marker_on_closing_line_is_allowed(self):
        text = (
            'TEST_CASE( "fast path", "[.perf]" )\n{\n'
            'REQUIRE(\n'
            '    bestElapsedMs\n'
            '    < 200 ); // lint-allow: perf-budget -- Local speed budget.\n'
            '}\n'
        )
        self.assertEqual(scan(text), [])

    def test_elapsed_lower_bound_is_not_a_budget(self):
        findings = scan(self.CASE % 'CHECK( timer.elapsed() >= 1 );')
        self.assertEqual(findings, [])

    def test_negated_elapsed_comparison_is_not_a_budget(self):
        # CHECK_FALSE( elapsed < minimum ) demands a *minimum* duration, so it
        # is a correctness assertion that must keep running in the default CI
        # run rather than being pushed behind a [.perf] tag.
        for snippet in (
            'CHECK_FALSE( timer.elapsed() < 50 );',
            'REQUIRE_FALSE( timer.elapsed() < 50 );',
            'CHECK_FALSE( std::chrono::steady_clock::now() - started < 50ms );',
        ):
            self.assertEqual(scan(self.CASE % snippet), [], snippet)


class PerfBudgetMacroRuleTest(unittest.TestCase):
    def test_unmarked_macro_call_site_is_flagged(self):
        findings = scan('KLOGG_CHECK_PERF_BUDGET( elapsedMs < 200 );')
        self.assertEqual([f.rule for f in findings], ["perf-budget-unmarked"])

    def test_marked_macro_call_site_is_allowed(self):
        self.assertEqual(
            scan(
                'KLOGG_CHECK_PERF_BUDGET( elapsedMs < 200 ); '
                '// lint-allow: perf-budget -- Local speed budget.'
            ),
            [],
        )

    def test_line_wrapped_macro_with_marker_is_allowed(self):
        text = (
            'KLOGG_CHECK_PERF_BUDGET(\n'
            '    elapsedMs\n'
            '    < 200 );  // lint-allow: perf-budget -- Local speed budget.\n'
        )
        self.assertEqual(scan(text), [])

    def test_macro_definition_is_not_a_call_site(self):
        text = (
            '#define KLOGG_CHECK_PERF_BUDGET( expr )\\\n'
            '    do {                                \\\n'
            '        CHECK( expr );                  \\\n'
            '    } while ( 0 )\n'
        )
        self.assertEqual(scan(text), [])

    def test_marker_does_not_silence_plain_budget_rule(self):
        # A marked plain assertion inside an untagged case still reports the
        # missing [.perf] tag.
        text = (
            'TEST_CASE( "fast path", "[capture]" )\n{\n'
            'CHECK( elapsedMs < 200 ); // lint-allow: perf-budget -- Local speed budget.\n}\n'
        )
        self.assertEqual([f.rule for f in scan(text)], ["perf-budget-needs-perf-tag"])


class PerfBudgetMarkerContractTest(unittest.TestCase):
    CASE = 'TEST_CASE( "fast path", "[.perf]" )\n{\n%s\n}\n'
    ASSERTIONS = (
        ("CHECK", "perf-budget-assertion"),
        ("KLOGG_CHECK_PERF_BUDGET", "perf-budget-unmarked"),
    )

    def assert_marker_findings(self, template, allowed=False):
        for assertion, rule in self.ASSERTIONS:
            with self.subTest(assertion=assertion, template=template):
                findings = scan(self.CASE % template.replace("ASSERT", assertion))
                self.assertEqual(
                    [finding.rule for finding in findings], [] if allowed else [rule]
                )

    def test_bare_marker_is_rejected(self):
        self.assert_marker_findings(
            'ASSERT( elapsedMs < 200 ); // lint-allow: perf-budget'
        )

    def test_empty_or_whitespace_reason_is_rejected(self):
        for reason in ("", " ", "\t  "):
            self.assert_marker_findings(
                'ASSERT( elapsedMs < 200 ); // lint-allow: perf-budget --' + reason
            )

    def test_reason_without_delimiter_is_rejected(self):
        self.assert_marker_findings(
            'ASSERT( elapsedMs < 200 ); // lint-allow: perf-budget local speed only'
        )

    def test_reason_cannot_be_borrowed_from_next_line(self):
        self.assert_marker_findings(
            'ASSERT( // lint-allow: perf-budget -- \t\n'
            '    elapsedMs < 200 ); // Local speed budget.'
        )

    def test_reasoned_marker_on_assertion_line_is_allowed(self):
        self.assert_marker_findings(
            'ASSERT( elapsedMs < 200 ); '
            '// lint-allow: perf-budget -- Local speed budget.',
            allowed=True,
        )

    def test_reasoned_marker_inside_multiline_assertion_is_allowed(self):
        self.assert_marker_findings(
            'ASSERT(\n'
            '    // lint-allow: perf-budget -- Local speed budget.\n'
            '    elapsedMs < 200 );',
            allowed=True,
        )

    def test_quoted_reason_is_allowed(self):
        self.assert_marker_findings(
            'ASSERT( elapsedMs < 200 ); '
            '// lint-allow: perf-budget -- "Fast" is local; CI checks completion.',
            allowed=True,
        )

    def test_unmatched_quote_in_comment_does_not_hide_marker(self):
        self.assert_marker_findings(
            '// This test\'s speed is local.\n'
            'ASSERT( elapsedMs < 200 ); '
            '// lint-allow: perf-budget -- It\'s a local speed budget.',
            allowed=True,
        )

    def test_string_literal_marker_is_rejected(self):
        self.assert_marker_findings(
            'ASSERT( elapsedMs < 200 ); '
            'const char* spoof = "// lint-allow: perf-budget -- Not a comment.";'
        )

    def test_raw_string_literal_marker_is_rejected(self):
        for literal in (
            'R"tag(// lint-allow: perf-budget -- Not a comment.)tag"',
            'R"tag(\n// lint-allow: perf-budget -- Not a comment.\n)tag"',
        ):
            self.assert_marker_findings(
                'ASSERT( elapsedMs < 200 && accepts( ' + literal + ' ) );'
            )

    def test_marker_outside_assertion_span_is_rejected(self):
        marker = '// lint-allow: perf-budget -- A different assertion.\n'
        assertion = 'ASSERT( elapsedMs < 200 );\n'
        self.assert_marker_findings(marker + assertion)
        self.assert_marker_findings(assertion + marker)

    def test_diagnostics_show_required_reason_format(self):
        for assertion, rule in self.ASSERTIONS:
            with self.subTest(assertion=assertion):
                findings = scan(self.CASE % f'{assertion}( elapsedMs < 200 );')
                self.assertEqual([finding.rule for finding in findings], [rule])
                self.assertIn(
                    '// lint-allow: perf-budget -- <nonempty reason>',
                    findings[0].message,
                )


class ScopeTest(unittest.TestCase):
    def test_non_test_files_are_not_scanned(self):
        self.assertEqual(
            scan('QTest::qSleep( 1 );', name="src/app/main.cpp"), []
        )

    def test_benchmark_files_are_not_scanned(self):
        self.assertEqual(
            scan('QThread::msleep( 5 );', name="benchmarks/foo.cpp"), []
        )


class RepoScanTest(unittest.TestCase):
    def test_real_call_site_with_reason_removed_is_flagged(self):
        path = REPO_ROOT / "tests/unit/capturestore_test.cpp"
        lines = path.read_text(encoding="utf-8").splitlines()
        call = 'KLOGG_CHECK_PERF_BUDGET( elapsedMs < 200 );'
        line_index = next(
            index for index, line in enumerate(lines) if call in line
        )
        lines[line_index] = lines[line_index].split("// lint-allow: perf-budget")[0]
        lines[line_index] += '// lint-allow: perf-budget'
        findings = lint.check_text("\n".join(lines), path)
        self.assertIn(
            ("perf-budget-unmarked", line_index + 1),
            [(finding.rule, finding.line) for finding in findings],
        )

    def test_repository_tree_has_no_findings(self):
        findings = lint.scan_repository(REPO_ROOT)
        self.assertEqual(
            findings,
            [],
            msg="\n".join(str(f) for f in findings),
        )


if __name__ == "__main__":
    unittest.main()
