import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).parents[2]
MODULE = ROOT / "cmake" / "verify_cpm_cache_hermetic.cmake"
BOOTSTRAP = "CPM_0.38.6.cmake"


class VerifyCpmCacheHermeticTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.cache = self.root / "cpm_cache"
        self.bootstrap = self.cache / "cpm"
        self.bootstrap.mkdir(parents=True)

    def tearDown(self):
        self.temporary.cleanup()

    def run_guard(self, cache, created):
        driver = self.root / "guard.cmake"
        configuration = [f"set(CPM_SOURCE_CACHE [[{cache}]])"] if cache else []
        writes = []
        for name in created:
            writes.append(f"file(MAKE_DIRECTORY [[{(self.cache / 'cpm').as_posix()}]])")
            writes.append(f"file(WRITE [[{(self.bootstrap / name).as_posix()}]] [[bootstrap]])")
        driver.write_text(
            "\n".join(
                configuration
                + [
                    f"include([[{MODULE.as_posix()}]])",
                    "klogg_cpm_bootstrap_artifacts(_before)",
                ]
                + writes
                + ['klogg_require_hermetic_cpm_cache("${_before}")', ""]
            )
        )
        return subprocess.run(
            ["cmake", "-P", str(driver)],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_configure_without_shared_cache_is_not_checked(self):
        result = self.run_guard(cache=None, created=[BOOTSTRAP])

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_warm_cache_bootstrap_artifact_passes(self):
        (self.bootstrap / BOOTSTRAP).write_text("prefetched bootstrap\n")

        result = self.run_guard(cache=self.cache, created=[])

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_warm_cache_reused_artifact_passes_when_unchanged(self):
        (self.bootstrap / BOOTSTRAP).write_text("prefetched bootstrap\n")

        result = self.run_guard(cache=self.cache, created=[BOOTSTRAP])

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def guard_message(self, result):
        # CMake wraps FATAL_ERROR text at the terminal width, so compare on
        # whitespace-normalized output rather than raw substrings.
        return " ".join((result.stdout + result.stderr).split())

    def test_downloaded_bootstrap_artifact_fails_configure(self):
        result = self.run_guard(cache=self.cache, created=[BOOTSTRAP])

        self.assertNotEqual(result.returncode, 0)
        message = self.guard_message(result)
        self.assertIn("bootstrap artifacts to the shared CPM source cache", message)
        self.assertIn(BOOTSTRAP, message)

    def test_failure_names_only_the_newly_downloaded_artifact(self):
        prefetched = "CPM_0.39.0.cmake"
        (self.bootstrap / prefetched).write_text("prefetched bootstrap\n")

        result = self.run_guard(cache=self.cache, created=[BOOTSTRAP])

        self.assertNotEqual(result.returncode, 0)
        message = self.guard_message(result)
        self.assertIn(f"cache: {BOOTSTRAP} (under", message)
        self.assertNotIn(prefetched, message)

    def test_failure_message_names_the_remedy(self):
        result = self.run_guard(cache=self.cache, created=[BOOTSTRAP])

        message = self.guard_message(result)
        self.assertIn("ROARING_USE_CPM OFF", message)
        self.assertIn("cmake/prefetch_cpm", message)


if __name__ == "__main__":
    unittest.main()
