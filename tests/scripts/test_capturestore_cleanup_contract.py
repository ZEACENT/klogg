import pathlib
import unittest


ROOT = pathlib.Path(__file__).parents[2]
CAPTURESTORE_HEADER = ROOT / "src" / "logdata" / "include" / "capturestore.h"
CAPTURESTORE_SOURCE = ROOT / "src" / "logdata" / "src" / "capturestore.cpp"


class CaptureStoreCleanupContractTest(unittest.TestCase):
    def test_async_cleanup_collects_candidates_after_detaching(self):
        source = CAPTURESTORE_SOURCE.read_text()
        method_start = source.index(
            "void CaptureStore::scheduleCleanupUnusedCaptures("
        )
        method_end = source.index("CaptureStore::CaptureStore(", method_start)
        method = source[method_start:method_end]

        thread_position = method.index("std::thread(")
        collect_position = method.index("collectUnusedCaptureCandidates(")
        self.assertLess(thread_position, collect_position)

        detached_work = method[thread_position:]
        self.assertIn("retainCaptureIds", detached_work)
        self.assertIn("rootPath", detached_work)
        self.assertIn("cleanupCaptureCandidates", detached_work)
        self.assertGreaterEqual(
            method.count("catch ( const std::exception& error )"), 2
        )
        self.assertGreaterEqual(method.count("catch ( ... )"), 2)

    def test_cleanup_retries_hidden_quarantine_directories(self):
        source = CAPTURESTORE_SOURCE.read_text()
        method_start = source.index(
            "CaptureStore::collectUnusedCaptureCandidates("
        )
        method_end = source.index(
            "QStringList CaptureStore::collectUnusedCapturePaths", method_start
        )
        method = source[method_start:method_end]

        self.assertIn("QDir::Hidden", method)
        self.assertIn("QDir::System", method)

    def test_cleanup_candidate_captures_path_state_and_epoch(self):
        header = CAPTURESTORE_HEADER.read_text()
        source = CAPTURESTORE_SOURCE.read_text()

        self.assertIn("struct CleanupCandidate {", header)
        self.assertIn("std::shared_ptr<CapturePathState> capturePathState;", header)
        self.assertIn("qint64 activityEpoch = 0;", header)
        self.assertIn("QByteArray processGeneration;", header)
        self.assertIn("capturePathState->processGeneration()", source)
        self.assertIn(
            "candidate.capturePathState->processGeneration()",
            source,
        )
        self.assertIn("QString registryKey_;", source)
        self.assertIn("qint64 registrySlotEpoch_ = 0;", source)
        self.assertIn("directory.identityKey()", source)
        self.assertIn(
            "candidate.capturePathState->finalizeRemovedGenerationGateHeld();",
            source,
        )
        self.assertIn("processFileOwnership_->ownedFiles.clear();", source)
        self.assertIn(
            "segmentIds_->nextSegmentId.store( 0, std::memory_order_release );",
            source,
        )


if __name__ == "__main__":
    unittest.main()
