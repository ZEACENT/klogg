import os
import pathlib
import re
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
PATCH = ROOT / "3rdparty" / "patches" / "fix_efsw_inotify_watcher_uaf.patch"
KQUEUE_PATCH = ROOT / "3rdparty" / "patches" / "fix_efsw_kqueue_watcher_races.patch"
THIRD_PARTY = ROOT / "3rdparty" / "CMakeLists.txt"
PREFETCH = ROOT / "cmake" / "prefetch_cpm" / "CMakeLists.txt"
STREAMVBYTE_PATCH = (
    ROOT / "3rdparty" / "patches" / "fix_streamvbyte_unaligned_stores.patch"
)
VECTORSCAN_HISTORY_PATCH = (
    ROOT / "3rdparty" / "patches" / "fix_vectorscan_null_history_ubsan.patch"
)
IMMUTABLE_GIT_PACKAGES = {
    "efsw",
    "libimobiledevice",
    "streamvbyte",
    "vectorscan",
}


def packages_with_autocrlf_disabled(source):
    packages = set()
    for body in re.findall(
        r"cpmaddpackage\s*\((.*?)\n\s*\)", source, re.IGNORECASE | re.DOTALL
    ):
        if "core.autocrlf=false" not in body:
            continue
        name = re.search(r"\bNAME\s+([^\s\)]+)", body, re.IGNORECASE)
        if name is None:
            raise AssertionError("CPM package with core.autocrlf=false has no NAME")
        packages.add(name.group(1))
    return packages


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

        descriptor_cleanup = patch[patch.index("diff --git a/src/efsw/WatcherKqueue.cpp") :]
        self.assertIn("i <= mChangeListCount", descriptor_cleanup)
        self.assertIn(
            "+\t\tclose( mChangeList[i].ident );\n"
            "+\t\tmWatcher->removeFD();\n"
            " \t}",
            descriptor_cleanup,
        )
        self.assertIn("if ( -1 != mKqueue )", descriptor_cleanup)

    def test_windows_completion_patch_is_applied_only_to_build_private_source(self):
        text = THIRD_PARTY.read_text()
        patch_call = text.split("klogg_apply_pinned_patch_series(\n  efsw\n", 1)[1].split("\n)", 1)[0]
        self.assertIn("fix_efsw_win32_iocp_retirement.patch", patch_call)
        self.assertNotIn("${efsw_SOURCE_DIR}", patch_call)
        self.assertIn("${KLOGG_EFSW_PREPARED_SOURCE_DIR}", patch_call)
        self.assertLess(patch_call.index("fix_efsw_inotify_watcher_uaf.patch"),
                        patch_call.index("fix_efsw_kqueue_watcher_races.patch"))
        self.assertLess(patch_call.index("fix_efsw_kqueue_watcher_races.patch"),
                        patch_call.index("fix_efsw_win32_iocp_retirement.patch"))
        self.assertIn("add_subdirectory(${KLOGG_EFSW_PREPARED_SOURCE_DIR}", text)
        self.assertIn('get_filename_component(_efsw_expected_source_dir "${KLOGG_EFSW_PREPARED_SOURCE_DIR}" REALPATH)', text)

    def test_efsw_patch_bytes_survive_git_clean_filters(self):
        # Win32 hunks include literal upstream CRLF context. Git staging must
        # preserve those bytes, not just a patch that works before committing.
        patches = (PATCH, KQUEUE_PATCH, ROOT / "3rdparty/patches/fix_efsw_win32_iocp_retirement.patch")
        with tempfile.TemporaryDirectory() as temporary:
            fixture = pathlib.Path(temporary)
            environment = os.environ.copy()
            environment.update({"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"})
            subprocess.run(["git", "init", "-q", str(fixture)], check=True, env=environment)
            (fixture / ".gitattributes").write_bytes((ROOT / ".gitattributes").read_bytes())
            for patch in patches:
                with self.subTest(patch=patch.name):
                    original = patch.read_bytes()
                    relative = patch.relative_to(ROOT).as_posix()
                    blob = subprocess.check_output(
                        ["git", "-c", "core.autocrlf=true", "hash-object", "-w", f"--path={relative}", "--stdin"],
                        input=original, cwd=fixture, env=environment,
                    ).decode("ascii").strip()
                    restored = subprocess.check_output(
                        ["git", "cat-file", "blob", blob], cwd=fixture, env=environment,
                    )
                    self.assertEqual(restored, original, "Git clean filters changed pinned patch bytes")

    def test_private_source_identity_changes_with_the_complete_patch_series(self):
        text = THIRD_PARTY.read_text()
        identity = re.search(r'set\(KLOGG_EFSW_PATCHED_TREE_SHA256 ([0-9a-f]{64})\)', text)
        self.assertIsNotNone(identity, "private source needs the verified final tree identity")
        self.assertIn(
            '${CMAKE_CURRENT_BINARY_DIR}/klogg-prepared/efsw-${KLOGG_EFSW_PATCHED_TREE_SHA256}',
            text,
        )
        patch_call = text.split("klogg_apply_pinned_patch_series(\n  efsw\n", 1)[1].split("\n)", 1)[0]
        self.assertIn("${KLOGG_EFSW_PATCHED_TREE_SHA256}", patch_call)

    def test_dependency_is_immutable_and_rejects_target_substitution(self):
        text = THIRD_PARTY.read_text()
        pinned_revision = "62f785c56b7a34f035193d4cb831921347b586b8"
        prefetch = PREFETCH.read_text()
        self.assertIn(
            "klogg_apply_pinned_patch_series(\n"
            "  efsw\n"
            "  ${KLOGG_EFSW_PREPARED_SOURCE_DIR}\n"
            f"  {pinned_revision}\n"
            "  c9dfe877307cb578f7db72f536a8ce26f4049573ceb434d3a8736862746be677\n"
            "  ${KLOGG_EFSW_PATCHED_TREE_SHA256}\n",
            text,
        )
        self.assertIn(pinned_revision, text)
        self.assertIn(pinned_revision, prefetch)
        self.assertIn("c43294a81501e0fdf14adc83818d47f7f9bc1bb6", prefetch)
        self.assertEqual(
            packages_with_autocrlf_disabled(text),
            IMMUTABLE_GIT_PACKAGES,
        )
        self.assertEqual(
            packages_with_autocrlf_disabled(prefetch),
            IMMUTABLE_GIT_PACKAGES,
        )
        for digest in (
            "3f4a410e6b1ad8f1f1d53c8463dc7accc7e1c94a32ca1ed1da778a181f5892b9",
            "d736c1c9bc9e3aca800bea90765f805e27229785e3d8e417e64103ba5fc96164",
            "c9dfe877307cb578f7db72f536a8ce26f4049573ceb434d3a8736862746be677",
            "dd0fac8dd03bee124f4b780ad131019590dd49e7ae7ce75f812e280e50c039f3",
            "04357c577d094b3ea8760c3635c776789fb671c1bff010c66a43f479f442eefd",
            "505337c394bf0b6ed954a62a865e6814eccab821e4fd22bba5ecbbdd3fb57e8d",
        ):
            self.assertIn(digest, text)
        inotify_patch_call = text.index("fix_efsw_inotify_watcher_uaf.patch")
        kqueue_patch_call = text.index("fix_efsw_kqueue_watcher_races.patch")
        efsw_revision_check = text.index(
            "klogg_apply_pinned_patch_series(\n  efsw"
        )
        target_guard = text.index("if(TARGET efsw)", kqueue_patch_call)
        source_check = text.index("Existing efsw target comes from", target_guard)
        self.assertLess(efsw_revision_check, inotify_patch_call)
        self.assertLess(inotify_patch_call, kqueue_patch_call)
        self.assertLess(kqueue_patch_call, target_guard)
        self.assertLess(target_guard, source_check)

        stream_package = text.index("NAME streamvbyte")
        stream_revision_check = text.index(
            "klogg_apply_pinned_patch_series(\n  streamvbyte", stream_package
        )
        stream_patch = text.index("fix_streamvbyte_unaligned_stores.patch")
        self.assertLess(stream_package, stream_revision_check)
        self.assertLess(stream_revision_check, stream_patch)

        vectorscan_package = text.index("NAME\n    vectorscan")
        vectorscan_revision_check = text.index(
            "klogg_apply_pinned_patch_series(\n    vectorscan", vectorscan_package
        )
        vectorscan_patch = text.index(
            "fix_vectorscan_msvc_warning_flags.patch", vectorscan_revision_check
        )
        vectorscan_target_guard = text.index("if(TARGET hs)", vectorscan_patch)
        vectorscan_source_check = text.index(
            "hs was configured before Klogg could apply", vectorscan_target_guard
        )
        self.assertLess(vectorscan_package, vectorscan_revision_check)
        self.assertLess(vectorscan_revision_check, vectorscan_patch)
        self.assertLess(vectorscan_patch, vectorscan_target_guard)
        self.assertLess(vectorscan_target_guard, vectorscan_source_check)

        efsw_package = text.index("NAME\n  efsw")
        for package in (stream_package, vectorscan_package, efsw_package):
            local_off = text.rfind("set(CPM_USE_LOCAL_PACKAGES OFF)", 0, package)
            local_restore = text.index(
                "set(CPM_USE_LOCAL_PACKAGES ${_TMP_CPM_USE_LOCAL_PACKAGES})",
                package,
            )
            self.assertGreaterEqual(local_off, 0)
            self.assertLess(local_off, package)
            self.assertLess(package, local_restore)

    def test_vectorscan_normalizes_absent_block_history_before_engine_dispatch(self):
        patch = VECTORSCAN_HISTORY_PATCH.read_text()
        added = "\n".join(
            line[1:]
            for line in patch.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        )
        self.assertIn(
            "s->core_info.hbuf = history ? history : (const u8 *)data;",
            added,
        )
        self.assertIn("assert(history || data);", added)
        self.assertNotIn("src/nfa/mcsheng.c", patch)

        cmake = THIRD_PARTY.read_text()
        patch_call = cmake.index("fix_vectorscan_null_history_ubsan.patch")
        vectorscan_subdirectory = cmake.index(
            "add_subdirectory(${vectorscan_SOURCE_DIR}", patch_call
        )
        self.assertLess(patch_call, vectorscan_subdirectory)

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
