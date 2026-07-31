import pathlib
import unittest


ROOT = pathlib.Path(__file__).parents[2]
PATCH = ROOT / "3rdparty" / "patches" / "fix_efsw_inotify_watcher_uaf.patch"
KQUEUE_PATCH = ROOT / "3rdparty" / "patches" / "fix_efsw_kqueue_watcher_races.patch"
THIRD_PARTY = ROOT / "3rdparty" / "CMakeLists.txt"
PREFETCH = ROOT / "cmake" / "prefetch_cpm" / "CMakeLists.txt"
STREAMVBYTE_PATCH = (
    ROOT / "3rdparty" / "patches" / "fix_streamvbyte_unaligned_stores.patch"
)


class EfswPatchContractTest(unittest.TestCase):
    def test_pending_move_survives_read_boundaries_and_blocks_reclamation(self):
        text = PATCH.read_text()
        self.assertNotIn("trailing IN_MOVED_FROM has no paired event in this read batch", text)
        pending_guard = text.index("if ( *it == currentMoveFrom )")
        delete = text.index("efSAFE_DELETE( *it )", pending_guard)
        self.assertLess(pending_guard, delete)

    def test_moved_out_queue_uses_one_mutex_contract(self):
        text = PATCH.read_text()
        self.assertIn("Mutex mMovedOutsideLock;", text)
        self.assertGreaterEqual(text.count("Lock movedLock( mMovedOutsideLock )"), 6)

    def test_kqueue_watchers_are_initialized_before_publication(self):
        patch = KQUEUE_PATCH.read_text()

        lock = patch.index("+\tLock lock( mWatchesLock )")
        initialize = patch.index(" \t\twatch->addAll()")
        publish = patch.index("+\t\t\tmWatches.insert", initialize)
        self.assertLess(lock, initialize)
        self.assertLess(initialize, publish)
        self.assertEqual(patch.count("+\tLock lock( mWatchesLock )"), 1)

        stop = patch.index("+\tefSAFE_DELETE( mThread )")
        reclaim = patch.index(" \tWatchMap::iterator iter = mWatches.begin()")
        self.assertLess(patch.index("+\tmInitOK = false"), stop)
        self.assertLess(stop, reclaim)

        remove_by_path = patch[patch.index("@@ -135,7 +133,9") :]
        self.assertIn("-\t\t\tremoveWatch( iter->first )", remove_by_path)
        self.assertIn("+\t\t\tmWatches.erase( iter )", remove_by_path)

    def test_dependency_is_immutable_and_rejects_target_substitution(self):
        text = THIRD_PARTY.read_text()
        pinned_revision = "62f785c56b7a34f035193d4cb831921347b586b8"
        prefetch = PREFETCH.read_text()
        self.assertIn(pinned_revision, text)
        self.assertIn(pinned_revision, prefetch)
        self.assertIn("c43294a81501e0fdf14adc83818d47f7f9bc1bb6", prefetch)
        inotify_patch_call = text.index("fix_efsw_inotify_watcher_uaf.patch")
        kqueue_patch_call = text.index("fix_efsw_kqueue_watcher_races.patch")
        efsw_revision_check = text.index(
            "klogg_require_pinned_revision(\n  efsw"
        )
        target_guard = text.index("if(TARGET efsw)", kqueue_patch_call)
        source_check = text.index("Existing efsw target comes from", target_guard)
        self.assertLess(efsw_revision_check, inotify_patch_call)
        self.assertLess(inotify_patch_call, kqueue_patch_call)
        self.assertLess(kqueue_patch_call, target_guard)
        self.assertLess(target_guard, source_check)

        stream_package = text.index("NAME streamvbyte")
        stream_revision_check = text.index(
            "klogg_require_pinned_revision(\n  streamvbyte", stream_package
        )
        stream_patch = text.index("fix_streamvbyte_unaligned_stores.patch")
        self.assertLess(stream_package, stream_revision_check)
        self.assertLess(stream_revision_check, stream_patch)

        efsw_package = text.index("NAME\n  efsw")
        for package in (stream_package, efsw_package):
            local_off = text.rfind("set(CPM_USE_LOCAL_PACKAGES OFF)", 0, package)
            local_restore = text.index(
                "set(CPM_USE_LOCAL_PACKAGES ${_TMP_CPM_USE_LOCAL_PACKAGES})",
                package,
            )
            self.assertGreaterEqual(local_off, 0)
            self.assertLess(local_off, package)
            self.assertLess(package, local_restore)

    def test_streamvbyte_uses_defined_unaligned_writes(self):
        patch = STREAMVBYTE_PATCH.read_text()
        added = "\n".join(
            line[1:] for line in patch.splitlines() if line.startswith("+") and not line.startswith("+++")
        )
        self.assertIn("memcpy(keyPtr, &packedKeys, sizeof(packedKeys))", added)
        self.assertIn("memcpy(dataPtr, &dw, sizeof(dw))", added)
        self.assertIn("const uint8_t *keyBytes = keyPtr", added)
        self.assertNotIn("*((uint16_t*)keyPtr)", added)
        self.assertNotIn("*((uint32_t*)dataPtr)", added)
        self.assertNotIn("const uint64_t *keyPtr64", added)
        cmake = THIRD_PARTY.read_text()
        self.assertIn("c43294a81501e0fdf14adc83818d47f7f9bc1bb6", cmake)
        self.assertIn("fix_streamvbyte_unaligned_stores.patch", cmake)


if __name__ == "__main__":
    unittest.main()
