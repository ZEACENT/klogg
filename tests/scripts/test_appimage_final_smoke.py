"""Final AppImage artifact smoke contract; no real packaging or execution."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "packaging/linux/appimage/generate_appimage.sh"


class AppImageFinalSmokeTest(unittest.TestCase):
    def setUp(self):
        self.text = GENERATOR.read_text()

    def test_final_package_is_extracted_and_executed_not_merely_hashed(self):
        final = self.text.index("linuxdeployqt-continuous-x86_64.AppImage appdir/usr/share/applications/*.desktop -appimage")
        receipt = self.text.index("--package-file")
        block = self.text[final:receipt]
        self.assertIn("--appimage-extract", block)
        self.assertIn("squashfs-root/AppRun", block)
        self.assertIn("QT_QPA_PLATFORM=offscreen", block)
        self.assertRegex(block, r"AppRun[^\n]*(-v|--version)")
        self.assertIn("squashfs-root/usr/bin/helpers/adb", block)
        self.assertNotRegex(block, r"smoke_adb_helper\.py \\\n\s+--adb appdir")

    def test_extracted_tree_passes_the_existing_package_verifier(self):
        final = self.text.index("-appimage")
        receipt = self.text.index("--package-file")
        block = self.text[final:receipt]
        self.assertIn("--package-root squashfs-root", block)
        self.assertIn("--asset-scope package", block)
        self.assertIn("--require-lock-binding", block)
        self.assertIn("adb-helper-appimage-final-smoke.json", block)

    def test_extraction_is_private_and_failure_cannot_smuggle_the_appdir_result(self):
        block = self.text[self.text.index("mkdir ./packages"):self.text.index("--package-file")]
        self.assertLess(block.index("rm -rf squashfs-root"), block.index("--appimage-extract"))
        self.assertLess(block.index("--appimage-extract"), block.index("smoke_adb_helper.py"))


if __name__ == "__main__":
    unittest.main()
