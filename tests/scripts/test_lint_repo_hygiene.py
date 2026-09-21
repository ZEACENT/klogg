import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "lint_repo_hygiene.py"
SPEC = importlib.util.spec_from_file_location("lint_repo_hygiene", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)

# The fixture characters below are exactly what the lint under test rejects,
# so each line carrying them opts in with the lint's own allow marker.
CJK_SAMPLE = "中文日志"  # lint-allow: repo-hygiene
GREEK_SAMPLE = "βγδ"  # lint-allow: repo-hygiene
# Supplementary-plane and other scripts missing from the enumerated denylist;
# caught by the non-Latin-letter catch-all (review finding on PR #76).
ADLAM_SAMPLE = "𞤀𞤣𞤤𞤢𞤥"  # lint-allow: repo-hygiene
DESERET_SAMPLE = "𐐔𐐯𐑆𐐨𐑉𐐯𐐻"  # lint-allow: repo-hygiene
CHEROKEE_SUPPLEMENT_SAMPLE = "ꭰꭱꭲ"  # lint-allow: repo-hygiene


class BinaryFileLintTest(unittest.TestCase):
    def test_plain_text_is_not_binary(self):
        self.assertIsNone(MODULE.binary_issue("src/app/main.cpp", b"int main() {}\n"))

    def test_nul_beyond_8kib_still_detected(self):
        # A textual header longer than the sampled prefix must not let a
        # binary blob through (review finding on PR #76).
        blob = b"textual header\n" * 1000 + b"\0" + b"tail"
        self.assertTrue(MODULE.is_binary(blob))
        self.assertIsNotNone(MODULE.binary_issue("src/app/main.cpp", blob))

    def test_asset_under_allowlisted_root_and_extension_is_allowed(self):
        png = b"\x89PNG\r\n\x1a\n" + b"\0" * 16
        self.assertIsNone(MODULE.binary_issue("src/app/images/hicolor/16x16/klogg.png", png))
        self.assertIsNone(MODULE.binary_issue("Resources/klogg.icns", b"icns\0\0\0\0"))

    def test_binary_with_wrong_extension_is_rejected_even_under_asset_root(self):
        blob = b"MZ" + b"\0" * 32
        issue = MODULE.binary_issue("website/static/tool.exe", blob)
        self.assertIsNotNone(issue)
        self.assertIn("asset extensions", issue)

    def test_binary_with_asset_extension_outside_asset_root_is_rejected(self):
        png = b"\x89PNG\r\n\x1a\n" + b"\0" * 16
        issue = MODULE.binary_issue("docs/screenshot.png", png)
        self.assertIsNotNone(issue)
        self.assertIn("asset locations", issue)

    def test_binary_in_source_tree_is_rejected(self):
        core_dump = b"\x7fELF" + b"\0" * 32
        self.assertIsNotNone(MODULE.binary_issue("src/logdata/core", core_dump))


class Utf8LintTest(unittest.TestCase):
    def test_valid_utf8_passes(self):
        data = "plain ASCII\ncafé — fine\n".encode("utf-8")
        self.assertIsNone(MODULE.utf8_issue("docs/BUILD.md", data))

    def test_utf8_bom_is_tolerated(self):
        data = b"\xef\xbb\xbf" + "content\n".encode("utf-8")
        self.assertIsNone(MODULE.utf8_issue("README.md", data))

    def test_utf8_bom_is_not_reported_as_non_english(self):
        # U+FEFF (BOM) sits inside the Arabic presentation-forms reject range;
        # the English-only scan must decode the BOM-stripped payload so a
        # BOM-prefixed English file does not fail as non-English.
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            data = b"\xef\xbb\xbf" + "# klogg build notes\n".encode("utf-8")
            (root / "bom-notes.md").write_bytes(data)

            issues = MODULE.check_file(root, "bom-notes.md")

            self.assertEqual(issues, 0)

    def test_legacy_encoding_is_rejected(self):
        gbk = "中文".encode("gbk")  # lint-allow: repo-hygiene
        issue = MODULE.utf8_issue("docs/notes.md", gbk)
        self.assertIsNotNone(issue)
        self.assertIn("not valid UTF-8", issue)

    def test_binary_files_are_not_utf8_checked(self):
        self.assertIsNone(MODULE.utf8_issue("src/app/fonts/DejaVuSansMono.ttf", b"\0\1\0\0"))


class NonEnglishLintTest(unittest.TestCase):
    def test_english_prose_with_typography_passes(self):
        text = (
            "// Auto-reconnect toggle — enables reconnection\n"
            "See docs → PORTABILITY.md ✓\n"
            "Author: café maintainer © 2026\n"
        )
        self.assertEqual(MODULE.non_english_issues("src/ui/src/session.cpp", text), [])

    def test_cjk_comment_is_rejected(self):
        text = f"// {CJK_SAMPLE}\nint line = 0;\n"
        findings = MODULE.non_english_issues("src/app/main.cpp", text)
        self.assertEqual(len(findings), 1)
        line_num, message = findings[0]
        self.assertEqual(line_num, 1)
        self.assertIn("non-English", message)
        self.assertIn("U+4E2D", message)

    def test_greek_and_fullwidth_forms_are_rejected(self):
        text = f"auto label = \"{GREEK_SAMPLE}＜（\";\n"  # lint-allow: repo-hygiene
        findings = MODULE.non_english_issues("src/ui/src/dialog.cpp", text)
        self.assertEqual(len(findings), 1)
        self.assertIn("U+03B2", findings[0][1])
        self.assertIn("U+FF1C", findings[0][1])

    def test_letterlike_math_symbols_pass(self):
        # R (U+211D), C (U+2102) and the Kelvin sign (U+212A) are Unicode
        # letters but serve as math/technical notation in English prose; the
        # letter catch-all must exempt the Letterlike Symbols block. These
        # characters need no lint-allow marker precisely because they pass.
        text = "// the domain is ℝ, the field is ℂ, the unit is K\n"
        self.assertEqual(MODULE.non_english_issues("docs/math.md", text), [])

    def test_supplementary_plane_scripts_are_rejected(self):        # These scripts are not in the enumerated NON_LATIN_SCRIPT_RE blocks;
        # the letter-category catch-all must still reject them.
        for sample, code_point in (
            (ADLAM_SAMPLE, "U+1E900"),
            (DESERET_SAMPLE, "U+10414"),
            (CHEROKEE_SUPPLEMENT_SAMPLE, "U+AB70"),
        ):
            text = f"// note: {sample}\n"
            findings = MODULE.non_english_issues("docs/notes.md", text)
            self.assertEqual(len(findings), 1, sample)
            self.assertIn(code_point, findings[0][1])

    def test_allow_marker_suppresses_its_own_line_only(self):
        text = (
            f"const auto sample = u8\"{CJK_SAMPLE}\";  // {MODULE.ALLOW_MARKER}\n"
            f"// {CJK_SAMPLE}\n"
        )
        findings = MODULE.non_english_issues("tests/unit/encodingdetector_test.cpp", text)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][0], 2)

    def test_exempt_prefixes_pass(self):
        text = f"<translation>{CJK_SAMPLE}</translation>\n"
        self.assertEqual(MODULE.non_english_issues("src/app/i18n/zh_CN.ts", text), [])
        self.assertEqual(
            MODULE.non_english_issues("website/themes/hugo-book/i18n/zh.yaml", text), []
        )
        self.assertEqual(MODULE.non_english_issues("3rdparty/patches/fix.patch", text), [])

    def test_markdown_docs_are_checked(self):
        text = "## 概述\n\nEnglish body.\n"  # lint-allow: repo-hygiene
        findings = MODULE.non_english_issues("docs/BUILD.md", text)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][0], 1)


class NonLatinScriptCoverageTest(unittest.TestCase):
    def test_armenian_georgian_bengali_letters_are_rejected(self):
        text = (
            "// Հայերեն\n"  # lint-allow: repo-hygiene
            "// ქართული\n"  # lint-allow: repo-hygiene
            "// বাংলা\n"  # lint-allow: repo-hygiene
        )
        findings = MODULE.non_english_issues("src/app/main.cpp", text)
        self.assertEqual(len(findings), 3)
        self.assertEqual([line for line, _ in findings], [1, 2, 3])

    def test_cjk_punctuation_and_fullwidth_forms_still_rejected(self):
        text = "x = 、！\n"  # lint-allow: repo-hygiene
        findings = MODULE.non_english_issues("src/app/main.cpp", text)
        self.assertEqual(len(findings), 1)

    def test_typographic_punctuation_still_passes(self):
        text = "// dashes — – quotes “…” arrow →\n"
        self.assertEqual(MODULE.non_english_issues("src/app/main.cpp", text), [])


class GitFilenameParsingTest(unittest.TestCase):
    @staticmethod
    def make_repo(root):
        import subprocess

        def git(*args):
            return subprocess.run(
                ["git", *args], cwd=root, capture_output=True, check=True
            )

        git("init", "-q")
        git("config", "user.email", "test@example.com")
        git("config", "user.name", "test")
        return git

    def test_tracked_files_returns_non_ascii_names_unquoted(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            git = self.make_repo(root)
            (root / "docs").mkdir()
            name = "docs/测试.md"  # lint-allow: repo-hygiene
            (root / name).write_text("hello\n", encoding="utf-8")
            git("add", ".")
            git("commit", "-qm", "init")

            files = MODULE.tracked_files(root)

            self.assertEqual(files, [name])

    def test_staged_files_returns_non_ascii_names_unquoted(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            git = self.make_repo(root)
            (root / "src").mkdir()
            name = "src/测试.cpp"  # lint-allow: repo-hygiene
            (root / name).write_text("int x;\n", encoding="utf-8")
            git("add", ".")

            files = MODULE.staged_files(root)

            self.assertEqual(files, [name])


class StagedModeIndexTest(unittest.TestCase):
    def test_check_file_reads_index_blob_not_worktree(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            git = GitFilenameParsingTest.make_repo(root)
            (root / "a.md").write_text("clean\n", encoding="utf-8")
            git("add", "a.md")
            git("commit", "-qm", "init")

            # Stage a violating blob, then clean the working-tree copy.
            (root / "a.md").write_text("// 中文\n", encoding="utf-8")  # lint-allow: repo-hygiene
            git("add", "a.md")
            (root / "a.md").write_text("clean\n", encoding="utf-8")

            issues = MODULE.check_file(root, "a.md", from_index=True)

            self.assertEqual(issues, 1)


if __name__ == "__main__":
    unittest.main()
