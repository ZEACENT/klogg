"""Final AppImage artifact smoke contract; no real packaging or execution."""
import pathlib
import re
import subprocess
import tempfile
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

    def test_offscreen_platform_plugin_is_bundled_before_packaging(self):
        bundle = self.text.index("-bundle-non-qt-libs")
        packaging = self.text.index("appdir/usr/share/applications/*.desktop -appimage")
        block = self.text[bundle:packaging]
        self.assertIn("libqoffscreen.so", block)
        self.assertIn("libqxcb.so", block)

    def test_missing_xcb_plugin_cannot_copy_offscreen_outside_appdir(self):
        block = self.text[self.text.index("xcb_plugin="):self.text.index("mkdir -p appdir/usr/lib")]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "appdir").mkdir()
            source = root / "usr/lib/qt5/plugins/platforms/libqoffscreen.so"
            source.parent.mkdir(parents=True)
            source.write_bytes(b"fixture")
            command = "set -euo pipefail\n" + block.replace("/usr/lib", str(root / "usr/lib"))
            result = subprocess.run(["bash", "-c", command], cwd=root,
                                    capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / "libqoffscreen.so").exists())
            self.assertFalse((root / "appdir/libqoffscreen.so").exists())

    def test_present_xcb_plugin_receives_offscreen_inside_appdir(self):
        block = self.text[self.text.index("xcb_plugin="):self.text.index("mkdir -p appdir/usr/lib")]
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            deployed = root / "appdir/usr/plugins/platforms/libqxcb.so"
            deployed.parent.mkdir(parents=True)
            deployed.write_bytes(b"xcb")
            source = root / "usr/lib/qt5/plugins/platforms/libqoffscreen.so"
            source.parent.mkdir(parents=True)
            source.write_bytes(b"offscreen")
            command = "set -euo pipefail\n" + block.replace("/usr/lib", str(root / "usr/lib"))
            result = subprocess.run(["bash", "-c", command], cwd=root,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((deployed.parent / "libqoffscreen.so").read_bytes(), b"offscreen")
            self.assertFalse((root / "libqoffscreen.so").exists())

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
