import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).parents[2]
CAPTURESTORE_SOURCE = ROOT / "src" / "logdata" / "src" / "capturestore.cpp"
LIVE_SOURCE_TRANSPORT = ROOT / "src" / "ui" / "src" / "livesourcetransport.cpp"


class StaticAnalysisRegressionTest(unittest.TestCase):
    def test_capturestore_widens_before_next_index_arithmetic(self):
        source = CAPTURESTORE_SOURCE.read_text()
        self.assertNotIn(
            "static_cast<size_t>( lastLocalLine + 1 )",
            source,
        )
        self.assertIn(
            "static_cast<size_t>( lastLocalLine ) + 1U",
            source,
        )

    def test_capturestore_reinitializes_one_shot_callback_after_move(self):
        source = CAPTURESTORE_SOURCE.read_text()
        self.assertRegex(
            source,
            re.compile(
                r"auto callback = std::move\( beforeRawSnapshotCopyCallback \);\s*"
                r"beforeRawSnapshotCopyCallback = \{\};\s*callback\(\);"
            ),
        )

    def test_qt_ownership_transfer_consumes_unique_ptr_release_result(self):
        source = LIVE_SOURCE_TRANSPORT.read_text()
        self.assertNotRegex(
            source,
            re.compile(r"^\s*dyingProcess\.release\(\);", re.MULTILINE),
        )
        self.assertEqual(
            source.count(
                "auto* const qtOwnedProcess = dyingProcess.release();"
            ),
            2,
        )


if __name__ == "__main__":
    unittest.main()
