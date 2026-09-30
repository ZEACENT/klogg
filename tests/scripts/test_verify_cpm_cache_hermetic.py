import os
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
MODULE = ROOT / "cmake" / "verify_cpm_cache_hermetic.cmake"
BOOTSTRAP = "CPM_0.38.6.cmake"
PREFETCHED = "prefetched bootstrap\n"


class VerifyCpmCacheHermeticTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.cache = self.root / "cpm_cache"
        self.bootstrap = self.cache / "cpm"
        self.bootstrap.mkdir(parents=True)

    def tearDown(self):
        self.temporary.cleanup()

    def run_guard(self, cache, created=(), mutation="", content="bootstrap"):
        driver = self.root / "guard.cmake"
        configuration = [f"set(CPM_SOURCE_CACHE [[{cache.as_posix()}]])"] if cache else []
        writes = []
        for name in created:
            writes.append(f"file(MAKE_DIRECTORY [[{self.bootstrap.as_posix()}]])")
            writes.append(
                f"file(WRITE [[{(self.bootstrap / name).as_posix()}]] [[{content}]])"
            )
        driver.write_text(
            "\n".join(
                configuration
                + [
                    f"include([[{MODULE.as_posix()}]])",
                    "klogg_cpm_bootstrap_artifacts(_before)",
                ]
                + writes
                + [mutation, 'klogg_require_hermetic_cpm_cache("${_before}")', ""]
            )
        )
        return subprocess.run(
            ["cmake", "-P", str(driver)],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )

    def make_symlink(self, link, target):
        try:
            link.symlink_to(target, target_is_directory=target.is_dir())
        except (NotImplementedError, OSError) as error:
            self.skipTest(f"Platform cannot create test symlinks: {error}")

    def guard_message(self, result):
        # CMake wraps FATAL_ERROR text at the terminal width, so compare on
        # whitespace-normalized output rather than raw substrings.
        return " ".join((result.stdout + result.stderr).split())

    def test_configure_without_shared_cache_is_not_checked(self):
        result = self.run_guard(cache=None, created=[BOOTSTRAP])

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.bootstrap / BOOTSTRAP).read_text(), "bootstrap")

    def test_empty_cache_passes(self):
        result = self.run_guard(cache=self.cache)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_warm_cache_bootstrap_artifact_passes(self):
        artifact = self.bootstrap / BOOTSTRAP
        artifact.write_text(PREFETCHED)

        result = self.run_guard(cache=self.cache)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(artifact.read_text(), PREFETCHED)

    def test_warm_cache_reused_artifact_passes_when_unchanged(self):
        artifact = self.bootstrap / BOOTSTRAP
        # Do not let Python's Windows newline translation change the fixture
        # relative to the literal bytes written by CMake's file(WRITE).
        artifact.write_bytes(PREFETCHED.encode("utf-8"))

        result = self.run_guard(
            cache=self.cache, created=[BOOTSTRAP], content=PREFETCHED
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(artifact.read_bytes(), PREFETCHED.encode("utf-8"))

    def test_same_length_changed_bytes_fail_and_are_removed(self):
        artifact = self.bootstrap / BOOTSTRAP
        artifact.write_text("old bytes")

        result = self.run_guard(
            cache=self.cache, created=[BOOTSTRAP], content="new bytes"
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(BOOTSTRAP, self.guard_message(result))
        self.assertFalse(artifact.exists())

    def test_downloaded_bootstrap_artifact_fails_configure_and_is_removed(self):
        result = self.run_guard(cache=self.cache, created=[BOOTSTRAP])

        self.assertNotEqual(result.returncode, 0)
        message = self.guard_message(result)
        self.assertIn("bootstrap artifacts", message)
        self.assertIn("shared CPM source cache", message)
        self.assertIn(BOOTSTRAP, message)
        self.assertFalse((self.bootstrap / BOOTSTRAP).exists())

    def test_repeated_download_on_same_cache_never_becomes_trusted(self):
        for attempt in range(3):
            with self.subTest(attempt=attempt):
                result = self.run_guard(cache=self.cache, created=[BOOTSTRAP])

                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.bootstrap / BOOTSTRAP).exists())

    def test_failure_names_only_offenders_and_preserves_other_sources(self):
        prefetched = self.bootstrap / "CPM_0.39.0.cmake"
        prefetched.write_text(PREFETCHED)
        source = self.cache / "dependency" / "source.cpp"
        source.parent.mkdir()
        source.write_text("prefetched source")
        outside = self.root / "unrelated.cmake"
        outside.write_text("outside cache")

        result = self.run_guard(cache=self.cache, created=[BOOTSTRAP])

        self.assertNotEqual(result.returncode, 0)
        message = self.guard_message(result)
        self.assertIn(BOOTSTRAP, message)
        self.assertNotIn(prefetched.name, message)
        self.assertEqual(prefetched.read_text(), PREFETCHED)
        self.assertEqual(source.read_text(), "prefetched source")
        self.assertEqual(outside.read_text(), "outside cache")

    def test_all_new_and_changed_files_are_removed_before_failure(self):
        changed = self.bootstrap / "changed.cmake"
        changed.write_text(PREFETCHED)

        result = self.run_guard(cache=self.cache, created=[BOOTSTRAP, changed.name])

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.bootstrap / BOOTSTRAP).exists())
        self.assertFalse(changed.exists())

    def test_download_outside_bootstrap_directory_is_out_of_scope(self):
        outside = self.cache / "dependency" / "downloaded.cmake"
        mutation = (
            f"file(MAKE_DIRECTORY [[{outside.parent.as_posix()}]])\n"
            f"file(WRITE [[{outside.as_posix()}]] [[download]])"
        )

        result = self.run_guard(cache=self.cache, mutation=mutation)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(outside.read_text(), "download")

    def test_failure_message_names_the_remedy(self):
        result = self.run_guard(cache=self.cache, created=[BOOTSTRAP])

        message = self.guard_message(result)
        self.assertIn("ROARING_USE_CPM OFF", message)
        self.assertIn("cmake/prefetch_cpm", message)

    def test_prefetched_symlink_is_rejected_without_touching_target(self):
        outside = self.root / "outside.cmake"
        outside.write_text(PREFETCHED)
        link = self.bootstrap / BOOTSTRAP
        self.make_symlink(link, outside)

        result = self.run_guard(cache=self.cache)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symlink", self.guard_message(result).lower())
        self.assertEqual(outside.read_text(), PREFETCHED)

    def test_downloaded_symlink_is_unlinked_without_touching_target(self):
        outside = self.root / "outside.cmake"
        outside.write_text(PREFETCHED)
        staged = self.root / "staged-link"
        self.make_symlink(staged, outside)
        link = self.bootstrap / BOOTSTRAP
        mutation = f"file(RENAME [[{staged.as_posix()}]] [[{link.as_posix()}]])"

        result = self.run_guard(cache=self.cache, mutation=mutation)

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(link.is_symlink())
        self.assertEqual(outside.read_text(), PREFETCHED)

    def test_downloaded_dangling_symlink_is_unlinked(self):
        outside = self.root / "missing.cmake"
        staged = self.root / "staged-link"
        self.make_symlink(staged, outside)
        link = self.bootstrap / BOOTSTRAP
        mutation = f"file(RENAME [[{staged.as_posix()}]] [[{link.as_posix()}]])"

        result = self.run_guard(cache=self.cache, mutation=mutation)

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(link.is_symlink())
        self.assertFalse(outside.exists())

    def test_symlinked_bootstrap_directory_is_rejected_without_traversal(self):
        outside = self.root / "outside"
        outside.mkdir()
        sentinel = outside / BOOTSTRAP
        sentinel.write_text(PREFETCHED)
        self.bootstrap.rmdir()
        self.make_symlink(self.bootstrap, outside)

        result = self.run_guard(cache=self.cache)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symlink", self.guard_message(result).lower())
        self.assertEqual(sentinel.read_text(), PREFETCHED)
        self.assertTrue(self.bootstrap.is_symlink())

    def test_directory_artifact_is_rejected_without_recursive_removal(self):
        directory = self.bootstrap / BOOTSTRAP
        directory.mkdir()
        sentinel = directory / "preserve.txt"
        sentinel.write_text("not a bootstrap file")

        result = self.run_guard(cache=self.cache)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("directory", self.guard_message(result).lower())
        self.assertEqual(sentinel.read_text(), "not a bootstrap file")

    def test_semicolon_filename_fails_closed(self):
        artifact = self.bootstrap / "bad;name.cmake"
        artifact.write_text(PREFETCHED)

        result = self.run_guard(cache=self.cache)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsupported", self.guard_message(result).lower())
        self.assertEqual(artifact.read_text(), PREFETCHED)

    def test_semicolon_cache_path_fails_closed(self):
        self.cache = self.root / "bad;cache"
        self.bootstrap = self.cache / "cpm"
        self.bootstrap.mkdir(parents=True)

        result = self.run_guard(cache=self.cache)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsupported", self.guard_message(result).lower())

    def test_cleanup_failure_cannot_become_a_trusted_retry_baseline(self):
        # Override only removal inside the driver to model an unremovable file,
        # independently of privileges and platform-specific permission rules.
        mutation = """
macro(file)
  if(NOT "${ARGV0}" STREQUAL "REMOVE")
    _file(${ARGV})
  endif()
endmacro()
"""
        result = self.run_guard(
            cache=self.cache, created=[BOOTSTRAP], mutation=mutation
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cleanup", self.guard_message(result).lower())
        self.assertTrue((self.bootstrap / BOOTSTRAP).exists())
        retry = self.run_guard(cache=self.cache)
        self.assertNotEqual(retry.returncode, 0)
        self.assertIn("prefetch", self.guard_message(retry).lower())

    def test_successful_cleanup_does_not_block_a_clean_retry(self):
        rejected = self.run_guard(cache=self.cache, created=[BOOTSTRAP])
        self.assertNotEqual(rejected.returncode, 0)
        self.assertFalse((self.bootstrap / BOOTSTRAP).exists())

        result = self.run_guard(cache=self.cache)

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_downloaded_directory_symlink_only_removes_the_link(self):
        outside = self.root / "outside"
        outside.mkdir()
        sentinel = outside / "preserve.txt"
        sentinel.write_text("outside directory")
        staged = self.root / "staged-link"
        self.make_symlink(staged, outside)
        link = self.bootstrap / BOOTSTRAP
        mutation = f"file(RENAME [[{staged.as_posix()}]] [[{link.as_posix()}]])"

        result = self.run_guard(cache=self.cache, mutation=mutation)

        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(link.is_symlink())
        self.assertEqual(sentinel.read_text(), "outside directory")

    def test_symlinked_cleanup_marker_never_overwrites_its_target(self):
        outside = self.root / "outside-marker"
        outside.write_text("preserve marker target")
        marker = self.cache / ".klogg-cpm-cleanup-pending"
        self.make_symlink(marker, outside)

        result = self.run_guard(cache=self.cache, created=[BOOTSTRAP])

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(outside.read_text(), "preserve marker target")
        self.assertTrue(marker.is_symlink())
        self.assertFalse((self.bootstrap / BOOTSTRAP).exists())

    def test_directory_cleanup_marker_is_rejected_without_removal(self):
        marker = self.cache / ".klogg-cpm-cleanup-pending"
        marker.mkdir()
        sentinel = marker / "preserve.txt"
        sentinel.write_text("preserve marker directory")

        result = self.run_guard(cache=self.cache)

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_text(), "preserve marker directory")

    def test_fifo_artifact_is_rejected_without_hanging(self):
        if not hasattr(os, "mkfifo"):
            self.skipTest("Platform cannot create FIFO test artifacts")
        artifact = self.bootstrap / BOOTSTRAP
        os.mkfifo(artifact)

        result = self.run_guard(cache=self.cache)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsupported", self.guard_message(result).lower())
        self.assertTrue(artifact.exists())


if __name__ == "__main__":
    unittest.main()
