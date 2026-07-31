import hashlib
import os
import pathlib
import subprocess
import tempfile
import time
import unittest


ROOT = pathlib.Path(__file__).parents[2]
APPLY_SERIES_SCRIPT = ROOT / "cmake" / "apply_git_patch_series.cmake"


def source_tree_digest(source: pathlib.Path) -> str:
    files = [
        path for path in source.rglob("*") if path.is_file() and ".git" not in path.parts
    ]
    manifest = ""
    for path in sorted(files, key=lambda item: item.relative_to(source).as_posix()):
        relative = path.relative_to(source).as_posix()
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        manifest += f"{relative}:{digest}\n"
    return hashlib.sha256(manifest.encode()).hexdigest()


class ApplyGitPatchSeriesTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temporary.name)
        self.repo = self.root / "source"
        self.repo.mkdir()
        self.environment = os.environ.copy()
        self.environment.update(
            {
                "GIT_CONFIG_GLOBAL": os.devnull,
                "GIT_CONFIG_NOSYSTEM": "1",
                "HOME": str(self.root / "isolated-home"),
            }
        )
        self.git = [
            "git",
            "-c",
            "core.autocrlf=false",
            "-c",
            "commit.gpgsign=false",
        ]
        subprocess.run(
            self.git + ["init", "-q", str(self.repo)],
            check=True,
            env=self.environment,
        )
        (self.repo / "value.txt").write_text("before\n")
        subprocess.run(
            self.git + ["add", "value.txt"],
            cwd=self.repo,
            check=True,
            env=self.environment,
        )
        subprocess.run(
            self.git
            + [
                "-c",
                "user.name=Klogg Test",
                "-c",
                "user.email=klogg@example.invalid",
                "commit",
                "-q",
                "-m",
                "initial",
            ],
            cwd=self.repo,
            check=True,
            env=self.environment,
        )
        self.revision = subprocess.check_output(
            self.git + ["rev-parse", "HEAD"],
            cwd=self.repo,
            text=True,
            env=self.environment,
        ).strip()
        self.clean_digest = source_tree_digest(self.repo)

        self.first_patch = self.root / "first.patch"
        self.first_patch.write_text(
            "diff --git a/value.txt b/value.txt\n"
            "--- a/value.txt\n"
            "+++ b/value.txt\n"
            "@@ -1 +1 @@\n"
            "-before\n"
            "+middle\n"
        )
        self.second_patch = self.root / "second.patch"
        self.second_patch.write_text(
            "diff --git a/value.txt b/value.txt\n"
            "--- a/value.txt\n"
            "+++ b/value.txt\n"
            "@@ -1 +1 @@\n"
            "-middle\n"
            "+after\n"
        )

        subprocess.run(
            self.git + ["apply", str(self.first_patch)],
            cwd=self.repo,
            check=True,
            env=self.environment,
        )
        subprocess.run(
            self.git + ["apply", str(self.second_patch)],
            cwd=self.repo,
            check=True,
            env=self.environment,
        )
        self.patched_digest = source_tree_digest(self.repo)
        subprocess.run(
            self.git + ["reset", "--hard", "-q", "HEAD"],
            cwd=self.repo,
            check=True,
            env=self.environment,
        )

    def tearDown(self):
        self.temporary.cleanup()

    def command(
        self,
        pause_marker=None,
        release_marker=None,
        before_lock_marker=None,
        lock_acquired_marker=None,
        patches=None,
        lock_timeout=None,
    ):
        patch_files = patches or [self.first_patch, self.second_patch]
        command = [
            "cmake",
            "-DDEPENDENCY=test-dependency",
            f"-DREPO_DIR={self.repo}",
            f"-DEXPECTED_REVISION={self.revision}",
            f"-DCLEAN_TREE_HASH={self.clean_digest}",
            f"-DAPPROVED_TREE_HASHES={self.clean_digest};{self.patched_digest}",
            f"-DPATCHED_TREE_HASH={self.patched_digest}",
            f"-DPATCH_FILES={';'.join(str(path) for path in patch_files)}",
        ]
        if pause_marker is not None:
            command.extend(
                [
                    "-DPAUSE_AFTER_PATCH_INDEX=1",
                    f"-DPAUSE_MARKER={pause_marker}",
                    f"-DPAUSE_RELEASE_MARKER={release_marker}",
                ]
            )
        if before_lock_marker is not None:
            command.append(f"-DBEFORE_LOCK_MARKER={before_lock_marker}")
        if lock_acquired_marker is not None:
            command.append(f"-DLOCK_ACQUIRED_MARKER={lock_acquired_marker}")
        if lock_timeout is not None:
            command.append(f"-DLOCK_TIMEOUT={lock_timeout}")
        command.extend(["-P", str(APPLY_SERIES_SCRIPT)])
        return command

    def wait_for_path(self, path, timeout=5):
        deadline = time.monotonic() + timeout
        while not path.exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(path.exists(), f"timed out waiting for {path}")

    def test_concurrent_series_never_observes_an_intermediate_patch_state(self):
        pause_marker = self.root / "first-patch-applied"
        release_marker = self.root / "release-first-process"
        contender_before_lock = self.root / "contender-before-lock"
        contender_has_lock = self.root / "contender-has-lock"
        first = subprocess.Popen(
            self.command(pause_marker, release_marker),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=self.environment,
        )
        self.wait_for_path(pause_marker)

        contender = subprocess.run(
            self.command(
                before_lock_marker=contender_before_lock,
                lock_acquired_marker=contender_has_lock,
                lock_timeout=0,
            ),
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
        )
        self.assertNotEqual(contender.returncode, 0)
        self.assertTrue(contender_before_lock.exists())
        self.assertFalse(contender_has_lock.exists())
        self.assertIn("Failed to lock test-dependency source", contender.stdout + contender.stderr)

        release_marker.write_text("resume")
        first_stdout, first_stderr = first.communicate(timeout=10)

        self.assertEqual(first.returncode, 0, first_stdout + first_stderr)
        self.assertEqual((self.repo / "value.txt").read_text(), "after\n")
        self.assertEqual(source_tree_digest(self.repo), self.patched_digest)

        idempotent = subprocess.run(
            self.command(),
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
        )
        self.assertEqual(idempotent.returncode, 0, idempotent.stdout + idempotent.stderr)

    def test_failed_series_rolls_back_to_the_approved_clean_tree(self):
        invalid_patch = self.root / "invalid.patch"
        invalid_patch.write_text(
            "diff --git a/value.txt b/value.txt\n"
            "--- a/value.txt\n"
            "+++ b/value.txt\n"
            "@@ -1 +1 @@\n"
            "-not-middle\n"
            "+after\n"
        )

        result = subprocess.run(
            self.command(patches=[self.first_patch, invalid_patch]),
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual((self.repo / "value.txt").read_text(), "before\n")
        self.assertEqual(source_tree_digest(self.repo), self.clean_digest)

    def test_interrupted_prefix_is_recovered_before_reapplying_the_series(self):
        subprocess.run(
            self.git + ["apply", str(self.first_patch)],
            cwd=self.repo,
            check=True,
            env=self.environment,
        )

        result = subprocess.run(
            self.command(),
            check=False,
            capture_output=True,
            text=True,
            env=self.environment,
        )

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.repo / "value.txt").read_text(), "after\n")
        self.assertEqual(source_tree_digest(self.repo), self.patched_digest)


if __name__ == "__main__":
    unittest.main()
