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


class PerfBudgetRuleTest(unittest.TestCase):
    CASE = 'TEST_CASE( "fast path", "[.perf]" )\n{\n%s\n}\n'

    def test_elapsed_budget_assertion_is_flagged(self):
        findings = scan(self.CASE % 'CHECK( elapsedMs < 200 );')
        self.assertIn("perf-budget-assertion", [f.rule for f in findings])

    def test_marked_budget_in_tagged_case_is_allowed(self):
        findings = scan(
            self.CASE % 'CHECK( elapsedMs < 200 ); // lint-allow: perf-budget'
        )
        self.assertEqual(findings, [])

    def test_budget_outside_perf_tagged_case_is_flagged(self):
        text = (
            'TEST_CASE( "fast path", "[capture]" )\n{\n'
            'CHECK( elapsedMs < 200 ); // lint-allow: perf-budget\n}\n'
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
    def test_repository_tree_has_no_findings(self):
        findings = lint.scan_repository(REPO_ROOT)
        self.assertEqual(
            findings,
            [],
            msg="\n".join(str(f) for f in findings),
        )


if __name__ == "__main__":
    unittest.main()
