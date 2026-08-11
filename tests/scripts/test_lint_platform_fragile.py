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


class Qt6IfReTest(unittest.TestCase):
    """_QT6_IF_RE must match Qt-6-only guards (>= 6, > 5) but NOT Qt-5
    guards (e.g. < QT_VERSION_CHECK(6, 0, 0))."""

    def test_qt6_ge_6_matches(self):
        cases = [
            "#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)",
            "#if QT_VERSION >= 6",
            "#if QT_VERSION >= 0x060000",
            "#if QT_VERSION > QT_VERSION_CHECK(5, 15, 0)",
            "#if QT_VERSION > 5",
            "#if QT_VERSION > 0x050f00",
            "#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)",
        ]
        for case in cases:
            with self.subTest(case=case):
                self.assertIsNotNone(lint._QT6_IF_RE.search(case),
                                     f"Should match: {case}")

    def test_qt5_lt_6_does_not_match(self):
        cases = [
            "#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)",
            "#if QT_VERSION <= QT_VERSION_CHECK(5, 15, 0)",
            "#if QT_VERSION_CHECK(6, 0, 0)",  # no comparison
        ]
        for case in cases:
            with self.subTest(case=case):
                self.assertIsNone(lint._QT6_IF_RE.search(case),
                                  f"Should NOT match: {case}")

    def test_qt_version_major_variant(self):
        # QT_VERSION_MAJOR guards also need to be recognized.
        self.assertIsNotNone(
            lint._QT6_IF_RE.search("#if QT_VERSION_MAJOR >= 6"))

    def test_qt6_ge_6_with_comment(self):
        self.assertIsNotNone(
            lint._QT6_IF_RE.search(
                "#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)  // Qt 6 only"))


class QsizetypeConversionQStringViewDeclTest(unittest.TestCase):
    """var_decl_re in the qsizetype check must recognise QStringView
    reference and pointer parameters."""

    def _do_check(self, text: str) -> list:
        return lint._check_qsizetype_to_int_conversion(
            text, Path("test.cpp"))

    def test_qstringview_ref_var_is_recognised(self):
        # const QStringView& sv should be added to qstrview_vars.
        text = (
            "void f(const QStringView& sv) {\n"
            "    qsizetype n = sv.indexOf('x');\n"
            "}\n"
        )
        self.assertEqual(self._do_check(text), [])

    def test_qstringview_ptr_var_is_recognised(self):
        text = (
            "void f(QStringView* sv) {\n"
            "    qsizetype n = sv->indexOf('x');\n"
            "}\n"
        )
        self.assertEqual(self._do_check(text), [])

    def test_qstringview_value_var_still_recognised(self):
        text = (
            "void f() {\n"
            "    QStringView sv = getView();\n"
            "    qsizetype n = sv.indexOf('x');\n"
            "}\n"
        )
        self.assertEqual(self._do_check(text), [])


class QsizetypeConversionReceiverGatingTest(unittest.TestCase):
    """PR #57 review: .remove() moved to the receiver-gated matcher
    (QSet/QMap/QHash/QCache::remove take const Key& -- a qsizetype key does not
    narrow), and the receiver declaration pattern must recognise
    template-declared containers (QVector<int>, QList<T>, ...) and prefer
    QStringList over the QString prefix."""

    def _do_check(self, text: str) -> list:
        return lint._check_qsizetype_to_int_conversion(
            text, Path("test.cpp"))

    def test_remove_on_qset_qsizetype_is_not_flagged(self):
        text = (
            "QSet<qsizetype> s;\n"
            "qsizetype q = 0;\n"
            "void f() { s.remove(q); }\n"
        )
        self.assertEqual(self._do_check(text), [])

    def test_remove_on_qmap_qsizetype_key_is_not_flagged(self):
        text = (
            "QMap<qsizetype, QString> m;\n"
            "qsizetype q = 0;\n"
            "void f() { m.remove(q); }\n"
        )
        self.assertEqual(self._do_check(text), [])

    def test_remove_on_declared_qstring_is_flagged(self):
        text = (
            "void f() {\n"
            "    QString s;\n"
            "    qsizetype q = 0;\n"
            "    s.remove(q, 1);\n"
            "}\n"
        )
        self.assertEqual(len(self._do_check(text)), 1)

    def test_remove_on_undeclared_receiver_is_not_flagged(self):
        # Member declared in a header: the gated check stays silent rather
        # than guessing (recall trade-off documented at the matcher).
        text = (
            "qsizetype q = 0;\n"
            "void f() { line.remove(q, 1); }\n"
        )
        self.assertEqual(self._do_check(text), [])

    def test_qvector_template_decl_at_is_flagged(self):
        text = (
            "void f() {\n"
            "    QVector<int> values;\n"
            "    qsizetype q = 0;\n"
            "    values.at(q);\n"
            "}\n"
        )
        self.assertEqual(len(self._do_check(text)), 1)

    def test_qlist_template_decl_value_is_flagged(self):
        text = (
            "void f() {\n"
            "    QList<QString> rows;\n"
            "    qsizetype q = 0;\n"
            "    rows.value(q);\n"
            "}\n"
        )
        self.assertEqual(len(self._do_check(text)), 1)

    def test_qvarlengtharray_two_param_decl_at_is_flagged(self):
        text = (
            "void f() {\n"
            "    QVarLengthArray<char, 256> buf;\n"
            "    qsizetype q = 0;\n"
            "    buf.at(q);\n"
            "}\n"
        )
        self.assertEqual(len(self._do_check(text)), 1)

    def test_nested_template_decl_at_is_flagged(self):
        text = (
            "void f() {\n"
            "    QVector<QPair<int, int>> pairs;\n"
            "    qsizetype q = 0;\n"
            "    pairs.at(q);\n"
            "}\n"
        )
        self.assertEqual(len(self._do_check(text)), 1)

    def test_qstringlist_decl_beats_qstring_prefix(self):
        text = (
            "void f() {\n"
            "    QStringList values;\n"
            "    qsizetype q = 0;\n"
            "    values.at(q);\n"
            "}\n"
        )
        self.assertEqual(len(self._do_check(text)), 1)

    def test_iterator_decl_is_not_registered_as_receiver(self):
        text = (
            "void f() {\n"
            "    QVector<int>::iterator it;\n"
            "    qsizetype q = 0;\n"
            "    it.value(q);\n"
            "}\n"
        )
        self.assertEqual(self._do_check(text), [])


class QStringListBraceAssignmentTest(unittest.TestCase):
    def check(self, text: str) -> list:
        return lint._check_qstringlist_brace_assignment(
            text, Path("versionchecker_test.cpp")
        )

    def test_assignment_from_braced_list_is_flagged(self):
        # This compiles with Qt 6 but is ambiguous between QStringList and
        # QList<QString> assignment overloads on Qt 5.
        text = (
            "QStringList archAliases;\n"
            "if ( useArm ) {\n"
            '    archAliases = { QStringLiteral( "arm64" ),\n'
            '                    QStringLiteral( "aarch64" ) };\n'
            "}\n"
        )
        findings = self.check(text)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][0], 3)

    def test_explicit_qstringlist_temporary_is_accepted(self):
        text = (
            "QStringList archAliases;\n"
            "if ( useArm ) {\n"
            '    archAliases = QStringList{ QStringLiteral( "arm64" ),\n'
            '                               QStringLiteral( "aarch64" ) };\n'
            "}\n"
        )
        self.assertEqual(self.check(text), [])

    def test_initializer_at_declaration_is_accepted(self):
        text = (
            'QStringList archAliases{ QStringLiteral( "x86_64" ),\n'
            '                         QStringLiteral( "amd64" ) };\n'
        )
        self.assertEqual(self.check(text), [])

    def test_allow_marker_suppresses_assignment(self):
        text = (
            "QStringList archAliases;\n"
            'archAliases = { QStringLiteral( "x64" ) }; '
            "// lint-allow: platform-fragile\n"
        )
        self.assertEqual(self.check(text), [])


class QtVersionMacroInTestsTest(unittest.TestCase):
    """Qt-version preprocessor guards are banned under tests/ (PR #57: the
    open-coded QWheelEvent constructor guard failed every Qt 5.15 CI leg with
    -Werror=deprecated-declarations). The split belongs in
    src/utils/include/platform/."""

    def check(self, text, name="tests/ui/crawlerwidget_test.cpp"):
        return lint._check_qt_version_macro_in_tests(text, Path(name))

    def test_qt_version_guard_in_test_is_flagged(self):
        cases = [
            "#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)",
            "#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)",
            "#elif QT_VERSION < QT_VERSION_CHECK(5, 15, 0)",
            "#if QT_VERSION_MAJOR == 6",
        ]
        for case in cases:
            with self.subTest(case=case):
                findings = self.check(case + "\n")
                self.assertEqual(len(findings), 1, f"Should flag: {case}")
                self.assertEqual(findings[0][0], 1)

    def test_guard_outside_tests_is_accepted(self):
        # src/ legitimately guards 5.15+ APIs per CLAUDE.md.
        text = "#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)\n"
        self.assertEqual(self.check(text, name="src/ui/src/abstractlogview.cpp"), [])

    def test_plain_code_and_comments_are_accepted(self):
        text = (
            "// #if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) used to be here\n"
            "const auto version = QT_VERSION_STR;\n"
        )
        self.assertEqual(self.check(text), [])

    def test_allow_marker_suppresses(self):
        text = (
            "#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0) "
            "// lint-allow: platform-fragile\n"
        )
        self.assertEqual(self.check(text), [])


if __name__ == "__main__":
    unittest.main()
