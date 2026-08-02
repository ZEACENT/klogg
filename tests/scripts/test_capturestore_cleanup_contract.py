import pathlib
import unittest


ROOT = pathlib.Path(__file__).parents[2]
CAPTURESTORE_SOURCE = ROOT / "src" / "logdata" / "src" / "capturestore.cpp"


class CaptureStoreCleanupContractTest(unittest.TestCase):
    def test_async_cleanup_snapshots_candidates_before_detaching(self):
        source = CAPTURESTORE_SOURCE.read_text()
        method_start = source.index(
            "void CaptureStore::scheduleCleanupUnusedCaptures("
        )
        method_end = source.index("CaptureStore::CaptureStore(", method_start)
        method = source[method_start:method_end]

        collect_position = method.index(
            "auto capturePaths = collectUnusedCapturePaths("
        )
        thread_position = method.index("std::thread(")
        self.assertLess(collect_position, thread_position)

        detached_work = method[thread_position:]
        self.assertIn(
            "[ capturePaths = std::move( capturePaths ), preserveModifiedAfter ]",
            detached_work,
        )
        self.assertNotIn("retainCaptureIds", detached_work)


if __name__ == "__main__":
    unittest.main()
