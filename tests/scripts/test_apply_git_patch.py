import pathlib
import subprocess
import tempfile
import time
import unittest


ROOT = pathlib.Path(__file__).parents[2]
APPLY_SCRIPT = ROOT / "cmake" / "apply_git_patch.cmake"


class ApplyGitPatchTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.repo = self.root / "source"
        self.repo.mkdir()
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        (self.repo / "value.txt").write_text("before\n")
        self.patch = self.root / "change.patch"
        self.patch.write_text(
            "diff --git a/value.txt b/value.txt\n"
            "index 90be1d2..af3bd91 100644\n"
            "--- a/value.txt\n"
            "+++ b/value.txt\n"
            "@@ -1 +1 @@\n"
            "-before\n"
            "+after\n"
        )

    def tearDown(self):
        self.temporary.cleanup()

    def apply_command(self):
        return [
            "cmake",
            f"-DREPO_DIR={self.repo}",
            f"-DPATCH_FILE={self.patch}",
            "-P",
            str(APPLY_SCRIPT),
        ]

    def test_waits_for_the_shared_source_cache_lock(self):
        ready = self.root / "ready"
        holder = self.root / "hold_lock.cmake"
        holder.write_text(
            f'file(LOCK "{self.root / "klogg-patch.lock"}" GUARD PROCESS TIMEOUT 5)\n'
            f'file(WRITE "{ready}" "ready")\n'
            'execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.5)\n'
        )
        process = subprocess.Popen(["cmake", "-P", str(holder)])
        deadline = time.monotonic() + 5
        while not ready.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(ready.exists())

        started = time.monotonic()
        result = subprocess.run(self.apply_command(), check=False, capture_output=True, text=True)
        elapsed = time.monotonic() - started
        process.wait(timeout=5)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertGreaterEqual(elapsed, 0.3)
        self.assertEqual((self.repo / "value.txt").read_text(), "after\n")

    def test_concurrent_invocations_are_idempotent(self):
        first = subprocess.Popen(self.apply_command(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        second = subprocess.Popen(self.apply_command(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        first_stdout, first_stderr = first.communicate(timeout=10)
        second_stdout, second_stderr = second.communicate(timeout=10)

        self.assertEqual(first.returncode, 0, first_stdout + first_stderr)
        self.assertEqual(second.returncode, 0, second_stdout + second_stderr)
        self.assertEqual((self.repo / "value.txt").read_text(), "after\n")
        reverse = subprocess.run(
            ["git", "apply", "--reverse", "--check", str(self.patch)],
            cwd=self.repo,
            check=False,
        )
        self.assertEqual(reverse.returncode, 0)


if __name__ == "__main__":
    unittest.main()
