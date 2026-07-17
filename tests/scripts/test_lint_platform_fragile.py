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
        # patternmatcher block-scan tests must stay clean.
        for name in ("foldersearchengine_test.cpp", "patternmatcher_test.cpp"):
            path = REPO_ROOT / "tests" / "unit" / name
            self.assertEqual(
                lint._check_vectorscan_capability_assertion(
                    path.read_text(encoding="utf-8"), path
                ),
                [],
                name,
            )


if __name__ == "__main__":
    unittest.main()
