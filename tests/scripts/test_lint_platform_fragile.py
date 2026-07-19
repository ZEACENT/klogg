import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import lint_platform_fragile as lint


def check(text, name="foldersearchengine_test.cpp"):
    return lint._check_vectorscan_capability_assertion(text, Path(name))


class VectorscanCapabilityAssertionTest(unittest.TestCase):
    def test_unguarded_require_is_flagged(self):
        # The exact shape that broke the Windows x86-qt5 [QTRegex] job in PR #42.
        text = (
            'TEST_CASE( "scanFile fast path honors shouldStop", "[folder]" )\n'
            "{\n"
            '    auto matcher = matcherFor( "MATCH" );\n'
            "    REQUIRE( matcher->hasBufferScan() );\n"
            "}\n"
        )
        findings = check(text)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][0], 4)

    def test_capability_early_return_guard_is_accepted(self):
        text = (
            'TEST_CASE( "t", "[folder]" )\n'
            "{\n"
            '    auto matcher = matcherFor( "MATCH" );\n'
            "    if ( !matcher->hasBufferScan() ) {\n"
            "        return;\n"
            "    }\n"
            "    REQUIRE( matcher->hasBufferScan() );\n"
            "}\n"
        )
        self.assertEqual(check(text), [])

    def test_regexp_engine_guard_is_accepted(self):
        # patternmatcher_test.cpp's block-scan parity test pattern.
        text = (
            'TEST_CASE( "t", "[patternmatcher]" )\n'
            "{\n"
            "    if ( config.regexpEngine() != RegexpEngine::Vectorscan ) {\n"
            "        return;\n"
            "    }\n"
            "    const auto matcher = expression.createMatcher();\n"
            "    REQUIRE( matcher->hasBufferScan() );\n"
            "}\n"
        )
        self.assertEqual(check(text), [])

    def test_require_false_is_not_an_availability_assertion(self):
        text = (
            'TEST_CASE( "t", "[patternmatcher]" )\n'
            "{\n"
            "    REQUIRE_FALSE( matcher->hasBufferScan() );\n"
            "}\n"
        )
        self.assertEqual(check(text), [])

    def test_non_test_file_is_ignored(self):
        text = "REQUIRE( matcher->hasBufferScan() );\n"
        self.assertEqual(check(text, name="foldersearchengine.cpp"), [])

    def test_guard_in_another_test_case_does_not_cover(self):
        text = (
            'TEST_CASE( "first", "[folder]" )\n'
            "{\n"
            "    if ( !matcher->hasBufferScan() ) {\n"
            "        return;\n"
            "    }\n"
            "}\n"
            'TEST_CASE( "second", "[folder]" )\n'
            "{\n"
            "    REQUIRE( matcher->hasBufferScan() );\n"
            "}\n"
        )
        findings = check(text)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][0], 9)

    def test_allow_marker_suppresses(self):
        text = (
            "// lint-allow: platform-fragile\n"
            'TEST_CASE( "t", "[folder]" )\n'
            "{\n"
            "    REQUIRE( matcher->hasBufferScan() );\n"
            "}\n"
        )
        self.assertEqual(check(text), [])

    def test_current_tree_has_no_findings(self):
        # Regression lock: the fixed folder tests and the engine-guarded
        # patternmatcher block-scan tests must stay clean. Decode with the
        # same lenient policy the lint uses (errors="replace") so a stray
        # non-UTF-8 byte in a test fixture cannot make this lock crash on a
        # file the lint itself would handle cleanly.
        for name in ("foldersearchengine_test.cpp", "patternmatcher_test.cpp"):
            path = REPO_ROOT / "tests" / "unit" / name
            self.assertEqual(
                lint._check_vectorscan_capability_assertion(
                    path.read_text(encoding="utf-8", errors="replace"), path
                ),
                [],
                name,
            )

    def test_assertion_in_line_comment_is_not_flagged(self):
        # A doc comment that literally mentions the banned macro must not be
        # flagged (the substring match must strip // comments first).
        text = (
            'TEST_CASE( "t", "[folder]" )\n'
            "{\n"
            "    // do not write REQUIRE( matcher->hasBufferScan() ) unconditionally.\n"
            "    auto m = matcherFor( \"X\" );\n"
            "}\n"
        )
        self.assertEqual(check(text), [])

    def test_file_scope_assertion_does_not_read_last_line(self):
        # Regression for the case_start=0 bug: an assertion with no enclosing
        # TEST_CASE must scan from line 1, not wrap to lines[-1]. A guard
        # substring that appears ONLY on the file's last line must not
        # falsely cover an earlier file-scope assertion.
        text = (
            "// preamble\n"
            "    REQUIRE( matcher->hasBufferScan() );\n"
            "TEST_CASE( \"t\", \"[x]\" ) { if ( !matcher->hasBufferScan() ) { return; } }\n"
        )
        findings = check(text)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][0], 2)


class TestPrivateCurrentCrawlerTest(unittest.TestCase):
    def check(self, text, name="foldercrawler_test.cpp"):
        return lint._check_test_private_current_crawler(text, Path(name))

    def test_real_call_is_flagged(self):
        text = "    auto* cw = mainWindow->currentCrawlerWidget();\n"
        findings = self.check(text)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][0], 1)

    def test_full_line_comment_is_not_flagged(self):
        # The exact shape that broke the platform-fragile CI gate on this PR:
        # a doc comment referencing the private accessor by name.
        text = "    // MainWindow routed them via currentCrawlerWidget() / the SignalMux.\n"
        self.assertEqual(self.check(text), [])

    def test_trailing_comment_after_code_is_stripped(self):
        # Code on the line is fine; a trailing // mention must not double-flag.
        text = "    doThing(); // see currentCrawlerWidget() history\n"
        self.assertEqual(self.check(text), [])

    def test_comment_with_string_containing_slash_slash_is_not_mistreated(self):
        # The // inside the string literal is not a comment start.
        text = '    QString s = "//"; currentCrawlerWidget();\n'
        self.assertEqual(len(self.check(text)), 1)

    def test_non_test_file_is_ignored(self):
        text = "    cw->currentCrawlerWidget();\n"
        self.assertEqual(self.check(text, name="mainwindow.cpp"), [])

    def test_current_tree_test_files_are_clean(self):
        # Regression lock: the UI test files must not call the private API,
        # and doc comments mentioning it (foldercrawler_test.cpp:2418) must
        # not false-positive. Decode leniently, matching the lint.
        for name in ("foldercrawler_test.cpp", "crawlerwidget_test.cpp", "mainwindow_test.cpp"):
            path = REPO_ROOT / "tests" / "ui" / name
            self.assertEqual(
                lint._check_test_private_current_crawler(
                    path.read_text(encoding="utf-8", errors="replace"), path
                ),
                [],
                name,
            )


if __name__ == "__main__":
    unittest.main()
