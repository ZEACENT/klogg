import importlib.util
import pathlib
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
SCRIPT = ROOT / "scripts" / "lint_translation_catalogs.py"
SPEC = importlib.util.spec_from_file_location("lint_translation_catalogs", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class TranslationCatalogLintTest(unittest.TestCase):
    def write_translation_repo(
        self,
        root: pathlib.Path,
        languages,
        generated,
        packaged_qt,
        catalogs,
    ) -> None:
        i18n_dir = root / "src" / "app" / "i18n"
        i18n_dir.mkdir(parents=True)
        language_entries = "\n".join(
            f'  <language ietfCode="{language}" name="{language}" />'
            for language in languages
        )
        (i18n_dir / "Languages.xml").write_text(
            f"<languages>\n{language_entries}\n</languages>\n", encoding="utf-8"
        )
        for language in catalogs:
            (i18n_dir / f"{language}.ts").write_text("<TS/>\n", encoding="utf-8")

        generated_entries = "\n".join(
            f"  ${{CMAKE_CURRENT_SOURCE_DIR}}/i18n/{language}.ts"
            for language in generated
        )
        packaged_entries = " ".join(packaged_qt)
        (root / "src" / "app" / "CMakeLists.txt").write_text(
            "set(TS_FILES\n"
            f"{generated_entries}\n"
            ")\n"
            "add_qt_translations_resource(KLOGG_QT_TRANSLATION_RES"
            f" {packaged_entries})\n",
            encoding="utf-8",
        )

    def test_supported_languages_are_read_from_app_language_manifest(self):
        xml_text = """\
<languages>
  <language ietfCode="en" name="English" />
  <language ietfCode="zh_TW" name="Traditional Chinese" />
</languages>
"""
        self.assertEqual(MODULE.supported_languages(xml_text), ["en", "zh_TW"])

    def test_cmake_language_lists_are_parsed_independently(self):
        cmake_text = """\
set(TS_FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/i18n/en.ts
  ${CMAKE_CURRENT_SOURCE_DIR}/i18n/zh_CN.ts
)
add_app_translations_resource(KLOGG_I18N_RES ${QM_FILES})
add_qt_translations_resource(KLOGG_QT_TRANSLATION_RES zh_CN zh_TW)
"""
        self.assertEqual(
            MODULE.generated_catalog_languages(cmake_text), {"en", "zh_CN"}
        )
        self.assertEqual(
            MODULE.packaged_qt_translation_languages(cmake_text), {"zh_CN", "zh_TW"}
        )

    def test_direct_tr_literals_are_checked_without_noop_false_positives(self):
        source_text = """\
widget->setText( tr( "Translated text" ) );
const char* deferred = QT_TR_NOOP( "Handled by its owning context" );
"""
        self.assertEqual(
            MODULE.direct_translation_literals(source_text),
            {"Translated text"},
        )

    def test_explicit_translation_noop_literals_keep_their_context(self):
        source_text = """\
return QT_TRANSLATE_NOOP( "MainWindow", "Import failed" );
"""
        self.assertEqual(
            MODULE.explicit_translation_messages(source_text),
            {("MainWindow", "Import failed")},
        )

    def test_catalog_parser_is_context_aware_and_ignores_obsolete_messages(self):
        catalog = """\
<TS>
  <context><name>Expected</name>
    <message><source>Current</source><translation>Current</translation></message>
    <message><source>Old</source><translation type="obsolete">Old</translation></message>
  </context>
  <context><name>Other</name>
    <message><source>Current</source><translation>Current</translation></message>
  </context>
</TS>
"""
        self.assertEqual(
            MODULE.catalog_messages(catalog),
            {("Expected", "Current"), ("Other", "Current")},
        )

    def test_build_parity_is_derived_from_the_manifest_for_any_language(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self.write_translation_repo(
                root,
                languages=("en", "fr_CA"),
                generated=("en", "fr_CA"),
                packaged_qt=("fr_CA",),
                catalogs=("en", "fr_CA"),
            )

            self.assertEqual(MODULE.build_parity_issues(root), [])

    def test_build_parity_reports_missing_and_unlisted_entries_in_every_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            self.write_translation_repo(
                root,
                languages=("en", "fr_CA"),
                generated=("en", "de"),
                packaged_qt=("de",),
                catalogs=("en", "de"),
            )

            self.assertEqual(
                MODULE.build_parity_issues(root),
                [
                    "fr_CA: missing src/app/i18n/fr_CA.ts catalog",
                    "fr_CA: missing from TS_FILES translation generation/app resource packaging",
                    "fr_CA: missing from add_qt_translations_resource Qt resource packaging",
                    "de: catalog is not listed in src/app/i18n/Languages.xml",
                    "de: TS_FILES entry is not listed in src/app/i18n/Languages.xml",
                    "de: Qt resource entry is not required by src/app/i18n/Languages.xml",
                ],
            )

    def test_supported_app_languages_have_build_and_resource_parity(self):
        self.assertEqual(MODULE.build_parity_issues(ROOT), [])

    def test_guarded_ui_translation_messages_are_present_in_the_right_context(self):
        self.assertEqual(MODULE.catalog_coverage_issues(ROOT), [])


if __name__ == "__main__":
    unittest.main()
